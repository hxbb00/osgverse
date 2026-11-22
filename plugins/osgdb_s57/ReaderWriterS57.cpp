#include <osg/io_utils>
#include <osg/ValueObject>
#include <osg/TriangleIndexFunctor>
#include <osg/MatrixTransform>
#include <osg/Geometry>
#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgDB/Registry>
#include <osgUtil/Tessellator>

#include <pipeline/Drawer2D.h>
#include <readerwriter/FeatureDefinition.h>
#include "mini_s57/iso8211.h"
#include "mini_s57/s57chart.h"
#include "mini_s57/s57catalog.h"

namespace osgS57
{
    // ---------------------------------------------------------------------------
    // S-57 pointers are stored as 40-bit binary "NAME" values:
    //     byte 0  : RCNM of the referenced record (110/120 node, 130 edge, 140 face)
    //     bytes1-4: RCID, little endian
    // ---------------------------------------------------------------------------
    struct SRef
    {
        int kind = 0;           // RCNM code of the referenced record
        long long rcid = 0;     // RCID inside that record class
        int topi = 0;           // VRPT topology indicator (1=begin,2=end ...)
        int ornt = 0;           // FSPT orientation (1=forward, 2=reverse, 3=undef)
        int usag = 0;           // FSPT usage (1=exterior ring, 2=interior hole)
        bool valid() const { return kind >= 100 && rcid >= 0; }
    };

    // One spatial (vector) record.
    struct VecRec
    {
        int kind = 0;
        long long rcid = 0;
        std::vector<S57::ChartPoint> pts;   // decoded coordinates (degrees)
        std::vector<SRef> links;       // VRPT pointer list
        int ruin = 0;                  // update status: 1=insert, 2=delete, 3=modify
        int rver = 0;                  // record version (update chain)
        bool deleted = false;          // tombstone set by RUIN=2 update records
    };

    // One feature record.
    struct FeatureRec
    {
        long long rcid = 0;
        int objl = 0, prim = 0;
        std::vector<S57::AttributeInstance> attrs;
        std::vector<SRef> refs;        // FSPT pointer list
        int ruin = 0;                  // update status: 1=insert, 2=delete, 3=modify
        int rver = 0;                  // record version (update chain)
        bool deleted = false;          // tombstone set by RUIN=2 update records
    };

    // Decode a 40-bit / wider binary NAME subfield into kind + RCID.
    bool decodeName(const I8211::Value& v, SRef& out)
    {
        out = SRef();
        if (!v.valid || v.type != I8211::SubfieldType::BinaryString) return false;
        static auto nib = [](unsigned char c) -> unsigned char {
            if (c >= '0' && c <= '9') return static_cast<unsigned char>(c - '0');
            if (c >= 'A' && c <= 'F') return static_cast<unsigned char>(c - 'A' + 10);
            if (c >= 'a' && c <= 'f') return static_cast<unsigned char>(c - 'a' + 10);
            return 0;
        };

        int bytes = static_cast<int>(v.hex.size()) / 2;
        if (bytes < 5) return false;   // need at least RCNM(1) + RCID(4)
        auto byteAt = [&](int i) -> int {
            unsigned char hi = nib(static_cast<unsigned char>(v.hex[2 * i]));
            unsigned char lo = nib(static_cast<unsigned char>(v.hex[2 * i + 1]));
            return (hi << 4) | lo;
        };

        out.kind = byteAt(0); out.rcid = 0;
        for (int i = 1; i < 5; ++i)
            out.rcid |= static_cast<long long>(byteAt(i)) << (8 * (i - 1));
        return out.valid();
    }

    // ---------------------------------------------------------------------------
    // local tangent-plane projection: metres around the cell centre
    // ---------------------------------------------------------------------------
    struct Projection
    {
        double lon0 = 0.0, lat0 = 0.0;
        double kx = 111194.93, ky = 111194.93;   // metres per degree
        S57::MolodenskyParams datum;             // dataset datum -> WGS-84
        bool degreesOut = false;                 // true: x=lon, y=lat degrees

        void set(double lonMin, double latMin, double lonMax, double latMax)
        {
            lon0 = (lonMin + lonMax) * 0.5;
            lat0 = (latMin + latMax) * 0.5;
            double mPerDeg = 111194.9266445587;  // 6371000 * pi/180
            kx = mPerDeg * std::cos(lat0 * 3.141592653589793 / 180.0);
            ky = mPerDeg;
        }

        osg::Vec3 toXY(const S57::ChartPoint& p) const
        {
            // ENC vectors are referenced to DSPM.HDAT (almost always WGS-84);
            // classic legacy datums are shifted through a 3-parameter
            // Molodensky transform first.
            double lon = p.lon, lat = p.lat;
            if (!datum.identity)
                S57::molodenskyToWGS84(datum, p.lon, p.lat, lon, lat);
            if (degreesOut)
                return osg::Vec3(static_cast<float>(lon),
                                 static_cast<float>(lat), 0.0f);
            // wrap the *difference* so cells crossing the +/-180 antimeridian
            // stay contiguous instead of spanning the whole plane
            double dl = S57::wrapLon180(lon - lon0);
            return osg::Vec3(static_cast<float>(dl * kx),
                             static_cast<float>((lat - lat0) * ky), 0.0f);
        }
    };

    // ---------------------------------------------------------------------------
    // chain-node rebuild: order a set of edges through their shared nodes
    // ---------------------------------------------------------------------------
    struct Chain
    {
        std::vector<osg::Vec3> pts;
        bool closed = false;
        int usag = 0;   // 1=exterior ring, 2=interior hole (0=unknown)
        int ornt = 0;   // orientation of the first oriented edge
    };

    // Signed area of a 2D ring (shoelace); positive = counter-clockwise.
    static double ringArea(const std::vector<osg::Vec3>& r)
    {
        double s = 0.0;
        for (size_t i = 0, j = r.size() - 1; i < r.size(); j = i++)
            s += (double)r[j].x() * r[i].y() - (double)r[i].x() * r[j].y();
        return s * 0.5;
    }

    // Is p inside a ring?  Ray casting; used to attach holes to their outer.
    static bool pointInRing(const osg::Vec2& p, const std::vector<osg::Vec3>& r)
    {
        bool inside = false;
        for (size_t i = 0, j = r.size() - 1; i < r.size(); j = i++)
        {
            const osg::Vec3& a = r[i]; const osg::Vec3& b = r[j];
            if (((a.y() > p.y()) != (b.y() > p.y())) &&
                (p.x() < (b.x() - a.x()) * (p.y() - a.y()) / (b.y() - a.y()) + a.x()))
                inside = !inside;
        }
        return inside;
    }

    // Build a Vec3Array copy of a ring for osgVerse::Feature::addPoints().
    static osg::Vec3Array* toVec3Array(const std::vector<osg::Vec3>& pts)
    {
        osg::Vec3Array* va = new osg::Vec3Array;
        va->insert(va->end(), pts.begin(), pts.end());
        return va;
    }

    // Return false (and leave 'out' untouched) when the node has no geometry.
    // Returning (0,0,0) here used to inject the projection origin -- the image
    // centre -- into chains, producing the fan of long straight lines.
    static bool nodeXY(const std::vector<VecRec>& recs, const Projection& pr,
                       int nodeIdx, osg::Vec3& out)
    {
        if (nodeIdx >= 0 && nodeIdx < static_cast<int>(recs.size()) &&
            !recs[nodeIdx].pts.empty())
            { out = pr.toXY(recs[nodeIdx].pts[0]); return true; }
        return false;
    }

    // Append one edge's vertices to a chain, oriented startNode -> endNode.
    static void appendEdge(const std::vector<VecRec>& recs,
                           const Projection& pr, int eIdx, int fromNode, int toNode,
                           std::vector<osg::Vec3>& out)
    {
        const VecRec& e = recs[eIdx];
        if (e.pts.empty())
        {
            // straight edge: geometry comes from its two nodes, but only if
            // those nodes actually carry coordinates
            osg::Vec3 p;
            if (nodeXY(recs, pr, fromNode, p) && (out.empty() || out.back() != p))
                out.push_back(p);
            if (toNode != fromNode && nodeXY(recs, pr, toNode, p) &&
                (out.empty() || out.back() != p)) out.push_back(p);
            return;
        }

        bool forward = true;
        osg::Vec3 nf;
        bool haveFrom = nodeXY(recs, pr, fromNode, nf);
        if (haveFrom)
        {
            const S57::ChartPoint& f0 = e.pts.front();
            const S57::ChartPoint& f1 = e.pts.back();
            osg::Vec3 p0 = pr.toXY(f0), p1 = pr.toXY(f1);
            auto d2 = [](const osg::Vec3& a, const osg::Vec3& b) { return (a - b).length2(); };
            forward = d2(p0, nf) <= d2(p1, nf);
        }

        // S-57 requires SG2D's first/last points to equal the connected NODE
        // coordinates, so the endpoint nodes are already present.  Do NOT
        // prepend/append them again: that only duplicates ~3% of the vertices.
        if (forward)
        {
            for (const S57::ChartPoint& p : e.pts)
            {
                osg::Vec3 v = pr.toXY(p);
                if (out.empty() || out.back() != v) out.push_back(v);
            }
        }
        else
        {
            for (auto it = e.pts.rbegin(); it != e.pts.rend(); ++it)
            {
                osg::Vec3 v = pr.toXY(*it);
                if (out.empty() || out.back() != v) out.push_back(v);
            }
        }

    }

    static std::vector<Chain> buildChains(const std::vector<VecRec>& recs,
                                          const Projection& pr, const std::vector<int>& edgeIdx,
                                          const std::vector<int>* usagList = NULL,
                                          const std::vector<int>* orntList = NULL)
    {
        std::vector<Chain> result;

        // node index -> edges incident at that node
        std::map<int, std::vector<int>> adj;
        std::vector<int> nodeA(edgeIdx.size(), -1), nodeB(edgeIdx.size(), -1);
        for (size_t k = 0; k < edgeIdx.size(); ++k)
        {
            const VecRec& e = recs[edgeIdx[k]]; int a = -1, b = -1;
            for (const SRef& r : e.links)
            {
                if (r.kind != 110 && r.kind != 120) continue;
                // find the vector record index of that node
                int idx = -1;
                for (size_t j = 0; j < recs.size(); ++j)
                    if (recs[j].kind == r.kind && recs[j].rcid == r.rcid) { idx = static_cast<int>(j); break; }
                if (idx < 0) continue;
                if (r.topi == 1) a = idx;
                else if (r.topi == 2) b = idx;
                else if (a < 0) a = idx;
                else if (b < 0) b = idx;
            }
            nodeA[k] = a; nodeB[k] = b;
            if (a >= 0) adj[a].push_back(static_cast<int>(k));
            if (b >= 0 && b != a) adj[b].push_back(static_cast<int>(k));
        }

        std::vector<bool> used(edgeIdx.size(), false);
        for (size_t s = 0; s < edgeIdx.size(); ++s)
        {
            if (used[s]) continue;
            int e0 = static_cast<int>(s);
            used[e0] = true;
            Chain chain;
            int startNode = nodeA[e0];
            if (startNode < 0) startNode = nodeB[e0];

            // prefer to begin at a degree-1 node (open polyline end)
            if (nodeA[e0] >= 0 && adj[nodeA[e0]].size() == 1 && adj[nodeB[e0]].size() != 1)
                startNode = nodeA[e0];
            else if (nodeB[e0] >= 0 && adj[nodeB[e0]].size() == 1)
                startNode = nodeB[e0];

            int curNode = startNode, curEdge = e0;
            int guard = 0, limit = static_cast<int>(edgeIdx.size()) * 2 + 8;
            while (guard++ < limit)
            {
                int a = nodeA[curEdge], b = nodeB[curEdge], other = -1;
                if (a == curNode) other = b;
                else if (b == curNode) other = a;
                appendEdge(recs, pr, edgeIdx[curEdge], curNode, other, chain.pts);
                // remember USAG/ORNT of the first edge that carries one
                if (usagList && chain.usag == 0 &&
                    curEdge < static_cast<int>(usagList->size()) && (*usagList)[curEdge] != 0)
                    chain.usag = (*usagList)[curEdge];
                if (orntList && chain.ornt == 0 &&
                    curEdge < static_cast<int>(orntList->size()) && (*orntList)[curEdge] != 0)
                    chain.ornt = (*orntList)[curEdge];
                curNode = other;
                if (other >= 0 && other == startNode)
                {
                    chain.closed = true;
                    break;
                }

                int nxt = -1;
                if (curNode >= 0)
                {
                    auto it = adj.find(curNode);
                    if (it != adj.end())
                        for (int cand : it->second)
                            if (!used[cand]) { nxt = cand; break; }
                }
                if (nxt < 0) break;   // open end of the chain reached
                used[nxt] = true; curEdge = nxt;
            }

            if (!chain.pts.empty())
            {
                // drop the duplicated closing vertex that the topology may imply
                if (chain.pts.size() > 2 && chain.pts.front() == chain.pts.back())
                    chain.pts.pop_back();
                result.push_back(chain);
            }
        }
        return result;
    }

    // ---------------------------------------------------------------------------
    // colour helpers (kept on the Feature as user values so that readNode can
    // reproduce the per-object-class colours)
    // ---------------------------------------------------------------------------
    static void setFeatureColor(osgVerse::Feature* f, const osg::Vec4& c)
    {
        f->setUserValue("_ColorR", (double)c.r());
        f->setUserValue("_ColorG", (double)c.g());
        f->setUserValue("_ColorB", (double)c.b());
        f->setUserValue("_ColorA", (double)c.a());
    }

    static osg::Vec4 featureColor(osgVerse::Feature* f,
        const osg::Vec4& def = osg::Vec4(1.0f, 1.0f, 1.0f, 1.0f))
    {
        double r = def.r(), g = def.g(), b = def.b(), a = def.a();
        f->getUserValue("_ColorR", r); f->getUserValue("_ColorG", g);
        f->getUserValue("_ColorB", b); f->getUserValue("_ColorA", a);
        return osg::Vec4((float)r, (float)g, (float)b, (float)a);
    }

    // ---------------------------------------------------------------------------
    // convert all spatial/feature records into a FeatureCollection
    // ---------------------------------------------------------------------------
    struct ChartData
    {
        std::vector<VecRec> recs;
        std::vector<FeatureRec> features;
        std::map<std::pair<int, long long>, int> spatialIndex; // (kind,rcid)->rec
        double lonMin = 1e9, lonMax = -1e9, latMin = 1e9, latMax = -1e9;
        bool haveGeo = false;

        void updateBounds(const VecRec& v)
        {
            for (const S57::ChartPoint& p : v.pts)
            {
                lonMin = std::min(lonMin, p.lon); lonMax = std::max(lonMax, p.lon);
                latMin = std::min(latMin, p.lat); latMax = std::max(latMax, p.lat);
                haveGeo = true;
            }
        }

        // deleted records stay in place as tombstones so that the indices
        // stored in spatialIndex (and in FeatureRec edge indices) stay valid
        int findSpatial(int kind, long long rcid) const
        {
            auto it = spatialIndex.find({ kind, rcid });
            if (it != spatialIndex.end())
            {
                int idx = it->second;
                if (idx >= 0 && idx < static_cast<int>(recs.size()) && !recs[idx].deleted)
                    return idx;
                return -1;
            }
            // tolerate producers that mix node classes
            if (kind == 110 || kind == 120)
                for (std::size_t i = 0; i < recs.size(); ++i)
                    if (!recs[i].deleted && recs[i].rcid == rcid &&
                        (recs[i].kind == 110 || recs[i].kind == 120))
                        return static_cast<int>(i);
            return -1;
        }

        int addVector(const VecRec& vr)
        {
            int idx = static_cast<int>(recs.size());
            recs.push_back(vr);
            spatialIndex[{vr.kind, vr.rcid}] = idx;
            updateBounds(vr);
            return idx;
        }
    };

    // Parse every vector/feature record of one ISO8211 dataset (base cell or
    // update cell) into 'chart'.  Update cells carry no DSPM of their own, so
    // the caller passes the base cell's parameters (COMF/SOMF inheritance);
    // RUIN/RVER subfields are stored on the records for the update merge.
    static void parseDataset(const I8211::Iso8211Reader& rdr,
                             const S57::DatasetParameters& dspm, ChartData& chart)
    {
        std::size_t n = rdr.recordCount();

        // ---- pass 1: vector records -------------------------------------------
        for (std::size_t i = 0; i < n; ++i)
        {
            const auto& rec = rdr.record(i);
            if (!rec.find("VRID")) continue;

            bool ok = false;
            int kind = static_cast<int>(rdr.intSubfield(rec, "VRID", 0, "RCNM", 0, &ok));
            if (!ok || kind < 110 || kind > 140) continue;

            VecRec vr;
            vr.kind = kind;
            vr.rcid = rdr.intSubfield(rec, "VRID", 0, "RCID", 0, &ok);
            vr.ruin = static_cast<int>(rdr.intSubfield(rec, "VRID", 0, "RUIN", 0, &ok));
            vr.rver = static_cast<int>(rdr.intSubfield(rec, "VRID", 0, "RVER", 0, &ok));
            vr.pts = S57::spatialCoordinates(rdr, rec, dspm);

            // VRPT pointer list (nodes for edges, edges for faces)
            if (rec.find("VRPT"))
            {
                const I8211::FieldDef* f = rec.find("VRPT");
                int reps = rdr.repeatCount(*f);
                for (int k = 0; k < reps; ++k)
                {
                    SRef r;
                    I8211::Value name = rdr.valueSubfield(rec, "VRPT", 0, "NAME", k);
                    if (!decodeName(name, r)) continue;
                    r.topi = static_cast<int>(rdr.intSubfield(rec, "VRPT", 0, "TOPI", k, &ok));
                    vr.links.push_back(r);
                }
            }

            chart.addVector(vr);
        }

        // ---- pass 2: feature records ------------------------------------------
        for (std::size_t i = 0; i < n; ++i)
        {
            const auto& rec = rdr.record(i);
            if (!rec.find("FRID")) continue;

            FeatureRec fr;
            bool ok = false;
            fr.rcid = rdr.intSubfield(rec, "FRID", 0, "RCID", 0, &ok);
            fr.prim = static_cast<int>(rdr.intSubfield(rec, "FRID", 0, "PRIM", 0, &ok));
            fr.objl = static_cast<int>(rdr.intSubfield(rec, "FRID", 0, "OBJL", 0, &ok));
            fr.ruin = static_cast<int>(rdr.intSubfield(rec, "FRID", 0, "RUIN", 0, &ok));
            fr.rver = static_cast<int>(rdr.intSubfield(rec, "FRID", 0, "RVER", 0, &ok));
            fr.attrs = S57::readAttributes(rdr, rec);
            if (rec.find("FSPT"))
            {
                const I8211::FieldDef* f = rec.find("FSPT");
                int reps = rdr.repeatCount(*f);
                for (int k = 0; k < reps; ++k)
                {
                    SRef r;
                    I8211::Value name = rdr.valueSubfield(rec, "FSPT", 0, "NAME", k);
                    if (decodeName(name, r))
                    {
                        r.topi = static_cast<int>(rdr.intSubfield(rec, "FSPT", 0, "TOPI", k, &ok));
                        r.ornt = static_cast<int>(rdr.intSubfield(rec, "FSPT", 0, "ORNT", k, &ok));
                        r.usag = static_cast<int>(rdr.intSubfield(rec, "FSPT", 0, "USAG", k, &ok));
                        fr.refs.push_back(r);
                    }
                }
            }
            chart.features.push_back(fr);
        }
    }

    // Turn the (possibly update-merged) chart into a FeatureCollection.
    // 'degreesOut' selects the output CRS: false = local tangent plane
    // (metres, default), true = geographic WGS-84 degrees (EPSG:4326).
    static osgVerse::FeatureCollection* assembleChart(const ChartData& chart,
                                                      const S57::DatasetParameters& dspm,
                                                      bool degreesOut)
    {
        if (chart.features.empty()) return NULL;

        // ---- projection centre ------------------------------------------------
        Projection pr;
        if (chart.haveGeo) pr.set(chart.lonMin, chart.latMin, chart.lonMax, chart.latMax);
        pr.degreesOut = degreesOut;
        pr.datum = S57::molodenskyForHdat(dspm.hdat);
        if (!pr.datum.known)
            OSG_WARN << "[ReaderWriterS57] HDAT=" << dspm.hdat
                     << " is not in the built-in datum table (and no M_HOPA is "
                     << "parsed); leaving coordinates in the dataset datum"
                     << std::endl;

        osgVerse::FeatureCollection* collection = new osgVerse::FeatureCollection;
        // CRS metadata for downstream consumers
        collection->setUserValue("CRS", std::string(degreesOut ?
            "WGS84 (EPSG:4326, degrees)" : "LocalTangentPlane (metres)"));
        collection->setUserValue("HorizontalDatum", (double)dspm.hdat);
        if (pr.datum.known && !pr.datum.identity)
            collection->setUserValue("DatumShiftedToWGS84", 1.0);
        for (const FeatureRec& fr : chart.features)
        {
            if (fr.deleted) continue;   // removed by an update cell
            S57::ObjectClassInfo oi = S57::objectClass(fr.objl);
            std::string acr = oi.valid ? oi.acronym : "UNK" + std::to_string(fr.objl);
            std::string cls = oi.valid ? oi.name : "(unknown object class)";
            std::string fname = acr + "_" + std::to_string(fr.rcid);

            // simple deterministic colour from the object class code
            static const osg::Vec4 palette[] = {
                {0.70f, 0.85f, 0.95f, 1.0f}, {0.95f, 0.75f, 0.55f, 1.0f},
                {0.75f, 0.90f, 0.70f, 1.0f}, {0.90f, 0.80f, 0.70f, 1.0f},
                {0.75f, 0.75f, 0.95f, 1.0f}, {0.95f, 0.90f, 0.60f, 1.0f},
                {0.80f, 0.70f, 0.90f, 1.0f}, {0.70f, 0.90f, 0.90f, 1.0f}
            };
            osg::Vec4 color = palette[(unsigned int)(fr.objl) %
                                      (sizeof(palette) / sizeof(palette[0]))];

            // resolve the referenced spatial records (USAG/ORNT stay parallel
            // to edgeRefs so that chain assembly can propagate them)
            std::vector<int> nodeRefs, edgeRefs, faceRefs, edgeUsags, edgeOrnts;
            for (const SRef& r : fr.refs)
            {
                int idx = chart.findSpatial(r.kind, r.rcid);
                if (idx < 0) continue;
                if (r.kind == 110 || r.kind == 120) nodeRefs.push_back(idx);
                else if (r.kind == 130)
                    { edgeRefs.push_back(idx); edgeUsags.push_back(r.usag); edgeOrnts.push_back(r.ornt); }
                else if (r.kind == 140) faceRefs.push_back(idx);
            }

            // numeric attributes that carry the sounding/height units of the
            // cell (DUNI/HUNI): additionally exposed in metres as "<key>_m"
            static const char* k_depthAttrs[] = { "DRVAL1", "DRVAL2", "VALSOU" };
            static const char* k_heightAttrs[] = { "VERCLR", "VERLEN" };

            auto fillMeta = [&](osgVerse::Feature* ft)
            {
                ft->setName(fname);
                setFeatureColor(ft, color);
                ft->setUserValue("ObjectClass", cls);
                ft->setUserValue("ObjectAcronym", acr);
                ft->setUserValue("ObjectCode", (double)fr.objl);
                ft->setUserValue("RCID", (double)fr.rcid);
                ft->setUserValue("Primitive", (double)fr.prim);
                // metadata objects (M_QUAL/M_COVR/M_NPUB/M_NSYS ...) span the
                // whole cell and are presentation-only; flag them so raster
                // rendering can skip them (S-52 does not display metadata)
                if (acr.size() > 2 && acr[0] == 'M' && acr[1] == '_')
                    ft->setUserValue("IsMetaObject", 1.0);
                for (const S57::AttributeInstance& a : fr.attrs)
                {
                    S57::AttributeInfo ai = S57::attribute(static_cast<int>(a.code));
                    std::string key = ai.valid ? ai.acronym : "A" + std::to_string(a.code);
                    if (key.empty()) continue;

                    double dv = 0.0;
                    if (S57::attributeAsDouble(a.value, dv))
                        ft->setUserValue(key, dv);                    // typed number
                    else if (!a.value.empty())
                        ft->setUserValue(key, a.value);               // text

                    if (dv != 0.0 || a.value == "0")
                    {
                        for (const char* d : k_depthAttrs)
                            if (key == d)
                                { ft->setUserValue(key + "_m", dv * dspm.depthUnitMetres()); break; }
                        for (const char* h : k_heightAttrs)
                            if (key == h)
                                { ft->setUserValue(key + "_m", dv * dspm.heightUnitMetres()); break; }
                    }
                }
            };

            bool made = false;
            if (fr.prim == 1)   // ---------------- point ----------------
            {
                osgVerse::Feature* feature = new osgVerse::Feature;
                fillMeta(feature); feature->setType(GL_POINTS);

                for (int idx : nodeRefs)
                {
                    const VecRec& v = chart.recs[idx];
                    if (v.pts.empty()) continue;
                    // A SOUNDG isolated node carries a whole row of SG3D
                    // points ("multi-point soundings"), each with its own
                    // VE3D depth -- add every point, not just the first.
                    int depthCount = 0;
                    double dmin = 1e30, dmax = -1e30;
                    for (const S57::ChartPoint& p3 : v.pts)
                    {
                        osg::Vec3 p = pr.toXY(p3);
                        if (p3.hasDepth)
                        {
                            // S-57 stores positive depths below the sounding
                            // datum, so the 3D Z is negative (down).
                            double depthM = p3.depth * dspm.depthUnitMetres();
                            p.z() = static_cast<float>(-depthM);
                            depthCount++;
                            if (depthM < dmin) dmin = depthM;
                            if (depthM > dmax) dmax = depthM;
                        }
                        feature->addPoint(p);
                        made = true;
                    }
                    if (depthCount > 0)
                    {
                        feature->setUserValue("HasDepth", (double)depthCount);
                        if (depthCount == 1)
                            feature->setUserValue("Depth_m", dmax);
                        else
                        {
                            feature->setUserValue("DepthMin_m", dmin);
                            feature->setUserValue("DepthMax_m", dmax);
                        }
                    }
                }
                for (int idx : edgeRefs)  // fallback: edge-vertex point objects
                {
                    const VecRec& v = chart.recs[idx];
                    if (!v.pts.empty())
                        { feature->addPoint(pr.toXY(v.pts[0])); made = true; }
                }
                if (made) collection->push_back(feature);
            }
            else if (fr.prim == 2)  // ---------------- line ----------------
            {
                std::vector<int> ids = edgeRefs;
                if (ids.empty() && !faceRefs.empty())
                {
                    for (int fi : faceRefs)
                        for (const SRef& l : chart.recs[fi].links)
                            if (l.kind == 130)
                            {
                                int ei = chart.findSpatial(130, l.rcid);
                                if (ei >= 0) ids.push_back(ei);
                            }
                }

                std::vector<Chain> chains = buildChains(chart.recs, pr, ids, &edgeUsags, &edgeOrnts);
                for (const Chain& c : chains)
                {
                    if (c.pts.size() < 2) continue;
                    osgVerse::Feature* cf = new osgVerse::Feature;   // one feature per chain
                    fillMeta(cf); cf->setType(c.closed ? GL_LINE_LOOP : GL_LINE_STRIP);

                    osg::ref_ptr<osg::Vec3Array> va = new osg::Vec3Array;
                    va->insert(va->end(), c.pts.begin(), c.pts.end());
                    cf->addPoints(va.get()); collection->push_back(cf);
                }
            }
            else  // prim == 3 (area) or coverage
            {
                std::vector<int> ids = edgeRefs;
                if (ids.empty() && !faceRefs.empty())
                {
                    for (int fi : faceRefs)
                        for (const SRef& l : chart.recs[fi].links)
                            if (l.kind == 130)
                            {
                                int ei = chart.findSpatial(130, l.rcid);
                                if (ei >= 0) ids.push_back(ei);
                            }
                }

                std::vector<Chain> rings = buildChains(chart.recs, pr, ids, &edgeUsags, &edgeOrnts);
                if (!rings.empty())
                {
                    struct RingData
                    {
                        std::vector<osg::Vec3> pts;
                        double area = 0.0;
                        int usag = 0;        // FSPT.USAG: 1=exterior, 2=hole
                        int depth = 0;       // how many other rings contain this one
                        bool hole = false;   // interior ring (hole)
                    };
                    std::vector<RingData> closed;

                    for (const Chain& r : rings)
                    {
                        if (!r.closed) continue;          // truncated by the cell edge
                        if (r.pts.size() < 3) continue;   // degenerate leftover ring
                        RingData rd; rd.pts = r.pts; rd.usag = r.usag;
                        if (rd.pts.size() > 2 && rd.pts.front() == rd.pts.back()) rd.pts.pop_back();
                        if (rd.pts.size() < 3) continue;  // degenerate after dedup
                        rd.area = ringArea(rd.pts); closed.push_back(rd);
                    }

                    // nesting depth: an even depth is an exterior, odd a hole.
                    // FSPT.USAG overrides the geometric guess when present.
                    for (size_t i = 0; i < closed.size(); ++i)
                    {
                        const osg::Vec3& p = closed[i].pts[0];
                        for (size_t j = 0; j < closed.size(); ++j)
                        {
                            if (i == j) continue;
                            if (pointInRing(osg::Vec2(p.x(), p.y()), closed[j].pts)) closed[i].depth++;
                        }
                        bool hole = (closed[i].depth % 2 == 1);
                        if (closed[i].usag == 1) hole = false;
                        else if (closed[i].usag == 2) hole = true;
                        closed[i].hole = hole;
                    }

                    // attach each hole to the smallest exterior containing it,
                    // then emit ONE feature per exterior: [outer, hole, hole...].
                    // Mixing all rings of a cell into a single polygon (old code)
                    // makes odd/even-odd fill draw spurious long triangles.
                    std::vector<std::vector<size_t>> owned(closed.size());
                    for (size_t h = 0; h < closed.size(); ++h)
                    {
                        if (!closed[h].hole) continue;
                        const osg::Vec3& p = closed[h].pts[0];
                        int best = -1; double bestArea = 1e300;
                        for (size_t o = 0; o < closed.size(); ++o)
                        {
                            if (closed[o].hole || o == h) continue;
                            if (!pointInRing(osg::Vec2(p.x(), p.y()), closed[o].pts)) continue;
                            if (fabs(closed[o].area) < bestArea)
                                { bestArea = fabs(closed[o].area); best = static_cast<int>(o); }
                        }
                        if (best >= 0) owned[best].push_back(h);
                        else closed[h].hole = false;   // orphan hole: keep as exterior
                    }

                    for (size_t o = 0; o < closed.size(); ++o)
                    {
                        if (closed[o].hole) continue;
                        osgVerse::Feature* area = new osgVerse::Feature;
                        fillMeta(area); area->setType(GL_POLYGON);
                        area->addPoints(toVec3Array(closed[o].pts));
                        for (size_t h : owned[o]) area->addPoints(toVec3Array(closed[h].pts));
                        area->setUserValue("ExteriorRings", 1.0);
                        area->setUserValue("InteriorRings", (double)owned[o].size());
                        collection->push_back(area);
                    }

                    // rings truncated by the cell boundary: boundary-only loops
                    for (const Chain& r : rings)
                    {
                        if (r.closed || r.pts.size() < 3) continue;
                        osg::ref_ptr<osg::Vec3Array> va = new osg::Vec3Array;
                        va->insert(va->end(), r.pts.begin(), r.pts.end());
                        if (va->size() > 2 && (*va)[0] == (*va)[va->size() - 1]) va->pop_back();

                        osgVerse::Feature* feature = new osgVerse::Feature;
                        // truncated chains are OPEN: a LINE_LOOP would draw a
                        // spurious long segment from the last point to the first
                        fillMeta(feature); feature->setType(GL_LINE_STRIP);
                        feature->addPoints(va.get()); collection->push_back(feature);
                    }
                }
            }
        }
        return collection;
    }

    // ---------------------------------------------------------------------------
    // P1: update cells (.001 .002 ...).  Per S-57 Appendix 2 an update record
    // carries a RUIN subfield in its VRID/FRID field:
    //     1 = insert (new record), 2 = delete (key-only record), 3 = modify
    // and its RVER must be exactly one higher than the version it replaces.
    // A cell can be cancelled by an update whose DSID.EDTN starts with '0'.
    // ---------------------------------------------------------------------------
    struct UpdateStats
    {
        std::string file;
        int inserts = 0, deletes = 0, modifies = 0, skipped = 0;
        bool cancelled = false;
    };

    static bool rverChainOk(int baseRver, int updRver, const char* what,
                            long long rcid, const std::string& file)
    {
        if (baseRver > 0 && updRver > 0 && updRver != baseRver + 1)
        {
            OSG_WARN << "[ReaderWriterS57] broken RVER chain in "
                     << osgDB::getSimpleFileName(file) << ": " << what
                     << " RCID " << rcid << " expects RVER " << (baseRver + 1)
                     << ", got " << updRver << " (record skipped)" << std::endl;
            return false;
        }
        return true;
    }

    // Apply one update cell to 'chart'.  Returns false when the update could
    // not be applied (the caller must stop the chain: later updates would be
    // built on a wrong state).
    static bool applyUpdateCell(const std::string& path, ChartData& chart,
                                const S57::DatasetParameters& dspm, UpdateStats& stats)
    {
        stats.file = path;
        std::ifstream in(path, std::ios::in | std::ios::binary);
        if (!in)
        {
            OSG_WARN << "[ReaderWriterS57] cannot open update cell " << path << std::endl;
            return false;
        }
        std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
        if (buffer.empty())
        {
            OSG_WARN << "[ReaderWriterS57] empty update cell " << path << std::endl;
            return false;
        }

        I8211::Iso8211Reader rdr;
        I8211::ParseResult pr = rdr.openMemory(buffer);
        if (!pr.ok)
        {
            OSG_WARN << "[ReaderWriterS57] " << osgDB::getSimpleFileName(path)
                     << ": " << pr.message << std::endl;
            return false;
        }

        // cancellation update: announces that the referenced cell is withdrawn
        if (rdr.recordCount() > 0)
        {
            const auto& rec = rdr.record(0);
            bool ok = false;
            std::string edtn = rdr.stringSubfield(rec, "DSID", 0, "EDTN", 0, &ok);
            if (ok && !edtn.empty() && edtn[0] == '0')
            {
                OSG_INFO << "[ReaderWriterS57] " << osgDB::getSimpleFileName(path)
                         << " is a cancellation update (EDTN=" << edtn << "), skipped"
                         << std::endl;
                stats.cancelled = true;
                return true;
            }
        }

        // parse the update's own records; they carry no DSPM, so coordinates
        // are decoded with the BASE cell's COMF/SOMF (inherited parameter)
        ChartData upd;
        parseDataset(rdr, dspm, upd);

        for (const VecRec& uv : upd.recs)
        {
            if (uv.kind < 110 || uv.kind > 140) continue;
            if (uv.ruin == 2)                    // ---- delete
            {
                int idx = chart.findSpatial(uv.kind, uv.rcid);
                if (idx < 0) { stats.skipped++; continue; }
                int baseRver = chart.recs[idx].rver;
                // same version: a re-issued update, already applied -> idempotent skip
                if (uv.rver > 0 && baseRver > 0 && uv.rver == baseRver)
                    { stats.skipped++; continue; }
                if (!rverChainOk(baseRver, uv.rver, "vector", uv.rcid, stats.file))
                    { stats.skipped++; continue; }
                chart.recs[idx].deleted = true;
                stats.deletes++;
            }
            else if (uv.ruin == 3)               // ---- modify: replace in place
            {
                int idx = chart.findSpatial(uv.kind, uv.rcid);
                if (idx < 0) { stats.skipped++; continue; }
                int baseRver = chart.recs[idx].rver;
                if (uv.rver > 0 && baseRver > 0 && uv.rver == baseRver)
                    { stats.skipped++; continue; }   // re-issued update
                if (!rverChainOk(baseRver, uv.rver, "vector", uv.rcid, stats.file))
                    { stats.skipped++; continue; }
                VecRec merged = uv; merged.ruin = 3; merged.deleted = false;
                chart.recs[idx] = merged;
                chart.updateBounds(merged);
                stats.modifies++;
            }
            else                                 // ---- insert
            {
                // re-issued (already applied) updates must not duplicate records
                if (chart.findSpatial(uv.kind, uv.rcid) >= 0)
                    { stats.skipped++; continue; }
                chart.addVector(uv);
                stats.inserts++;
            }
        }

        for (const FeatureRec& uf : upd.features)
        {
            if (uf.ruin == 2)                    // ---- delete
            {
                for (std::size_t i = 0; i < chart.features.size(); ++i)
                {
                    FeatureRec& base = chart.features[i];
                    if (base.deleted || base.rcid != uf.rcid) continue;
                    // same version: re-issued update -> idempotent skip
                    if (uf.rver > 0 && base.rver > 0 && uf.rver == base.rver)
                        { stats.skipped++; break; }
                    if (!rverChainOk(base.rver, uf.rver, "feature", uf.rcid, stats.file))
                        { stats.skipped++; break; }
                    base.deleted = true;
                    stats.deletes++;
                    break;
                }
            }
            else if (uf.ruin == 3)               // ---- modify: replace in place
            {
                for (std::size_t i = 0; i < chart.features.size(); ++i)
                {
                    FeatureRec& base = chart.features[i];
                    if (base.deleted || base.rcid != uf.rcid) continue;
                    if (uf.rver > 0 && base.rver > 0 && uf.rver == base.rver)
                        { stats.skipped++; break; }   // re-issued update
                    if (!rverChainOk(base.rver, uf.rver, "feature", uf.rcid, stats.file))
                        { stats.skipped++; break; }
                    FeatureRec merged = uf; merged.ruin = 3; merged.deleted = false;
                    base = merged;
                    stats.modifies++;
                    break;
                }
            }
            else                                 // ---- insert
            {
                // re-issued (already applied) updates must not duplicate records
                bool dup = false;
                for (std::size_t i = 0; i < chart.features.size(); ++i)
                    if (!chart.features[i].deleted && chart.features[i].rcid == uf.rcid)
                        { dup = true; break; }
                if (dup) { stats.skipped++; continue; }
                chart.features.push_back(uf);
                stats.inserts++;
            }
        }
        return true;
    }

    // Merge every numbered sibling update cell of 'basePath' in sequence
    // (GB5X01SW.000 -> .001, .002, ...).  The chain stops at the first file
    // that is missing or fails to apply.
    static void applyUpdateCells(const std::string& basePath, ChartData& chart,
                                 const S57::DatasetParameters& dspm)
    {
        std::string dir = osgDB::getFilePath(basePath);
        std::string stem = osgDB::getNameLessExtension(osgDB::getSimpleFileName(basePath));
        std::string baseExt = osgDB::getLowerCaseFileExtension(basePath);
        int num = atoi(baseExt.c_str());
        if (num < 0) return;   // base cells are .000, updates start at .001

        for (++num; num < 1000; ++num)
        {
            char ext[8]; sprintf(ext, "%03d", num);
            std::string f = dir.empty()
                ? (stem + "." + ext) : (dir + "/" + stem + "." + ext);
            if (!osgDB::fileExists(f)) break;

            UpdateStats st;
            if (!applyUpdateCell(f, chart, dspm, st)) break;
            OSG_INFO << "[ReaderWriterS57] applied update "
                     << osgDB::getSimpleFileName(f)
                     << " (+" << st.inserts << " -" << st.deletes
                     << " ~" << st.modifies << ", skipped " << st.skipped << ")"
                     << std::endl;
            if (st.cancelled) break;
        }
    }

    // Convenience wrapper preserving the original single-dataset entry point.
    static osgVerse::FeatureCollection* buildChart(const I8211::Iso8211Reader& rdr)
    {
        std::size_t n = rdr.recordCount();
        if (n == 0) return NULL;

        S57::DatasetParameters dspm;
        for (std::size_t i = 0; i < n; ++i)
        {
            const auto& rec = rdr.record(i);
            if (rec.find("DSPM")) dspm = S57::readDspm(rdr, rec);
        }

        ChartData chart;
        parseDataset(rdr, dspm, chart);
        return assembleChart(chart, dspm, false);
    }

    // Base cell + sequential merge of its sibling update cells.  Update cells
    // never carry a DSPM record, so the base cell's COMF/SOMF drive the
    // coordinate decoding of every update (known pitfall).
    static osgVerse::FeatureCollection* buildChartWithUpdates(
        const I8211::Iso8211Reader& rdr, const std::string& basePath,
        bool applyUpdates, bool degreesOut)
    {
        std::size_t n = rdr.recordCount();
        if (n == 0) return NULL;

        S57::DatasetParameters dspm;
        for (std::size_t i = 0; i < n; ++i)
        {
            const auto& rec = rdr.record(i);
            if (rec.find("DSPM")) dspm = S57::readDspm(rdr, rec);
        }

        ChartData chart;
        parseDataset(rdr, dspm, chart);
        if (applyUpdates && !basePath.empty()) applyUpdateCells(basePath, chart, dspm);
        return assembleChart(chart, dspm, degreesOut);
    }
}  // namespace

class ReaderWriterS57 : public osgDB::ReaderWriter
{
public:
    ReaderWriterS57()
    {
        supportsExtension("verse_s57", "osgVerse pseudo-loader");
        supportsExtension("000", "IHO S-57 ENC base data set (ISO8211)");
        supportsExtension("s57", "IHO S-57 ENC data set (alias)");
        supportsOption("IncludeFeatures", "Add FeatureCollection as UserData of the result Geometry/Image. Default: 0");
        supportsOption("ImageWidth", "Image resolution. Default: 512");
        supportsOption("ImageHeight", "Image resolution. Default: 512");
        supportsOption("ShowMetaObjects",
            "Draw M_QUAL/M_COVR/M_NPUB/... polygons in readImage. Default: 0");
        supportsOption("ApplyUpdates",
            "Merge sibling update cells (.001/.002/...) when the input is a "
            "base cell read from a file. Default: 1");
        supportsOption("OutputCRS",
            "Output coordinate system: Local = local tangent plane in metres "
            "(default), WGS84 = geographic degrees (EPSG:4326). readImage "
            "always draws in local metres regardless of this option.");
    }

    virtual const char* className() const
    {
        return "[osgVerse] S-57 electronic navigational chart reader";
    }

    // helper shared by the three stream entry points
    static osg::ref_ptr<osgVerse::FeatureCollection> loadCollection(
        const osgS57::I8211::Iso8211Reader& rdr, const Options* options)
    {
        std::string upd = options ? options->getPluginStringData("ApplyUpdates") : "";
        bool applyUpd = (upd.empty() || upd[0] != '0');
        std::string basePath = options ? options->getPluginStringData("S57_BASE_PATH") : "";
        std::string crs = options ? options->getPluginStringData("OutputCRS") : "";
        bool degreesOut = (crs == "WGS84" || crs == "wgs84" || crs == "EPSG:4326");
        return osgS57::buildChartWithUpdates(rdr, basePath, applyUpd, degreesOut);
    }

    virtual ReadResult readObject(const std::string& path, const Options* options) const
    {
        std::string ext; std::string fileName = getRealFileName(path, ext);
        if (fileName.empty()) return ReadResult::FILE_NOT_HANDLED;

        osg::ref_ptr<Options> lOptions = options ?
            static_cast<Options*>(options->clone(osg::CopyOp::SHALLOW_COPY)) : new Options;
        lOptions->setPluginStringData("STREAM_FILENAME", osgDB::getSimpleFileName(fileName));
        lOptions->setPluginStringData("S57_BASE_PATH", fileName);

        std::ifstream in(fileName, std::ios::in | std::ios::binary);
        if (!in) return ReadResult::FILE_NOT_HANDLED;;
        return readObject(in, lOptions.get());
    }

    virtual ReadResult readNode(const std::string& path, const Options* options) const
    {
        std::string ext; std::string fileName = getRealFileName(path, ext);
        if (fileName.empty()) return ReadResult::FILE_NOT_HANDLED;

        osg::ref_ptr<Options> lOptions = options ?
            static_cast<Options*>(options->clone(osg::CopyOp::SHALLOW_COPY)) : new Options;
        lOptions->setPluginStringData("STREAM_FILENAME", osgDB::getSimpleFileName(fileName));
        lOptions->setPluginStringData("S57_BASE_PATH", fileName);

        std::ifstream in(fileName, std::ios::in | std::ios::binary);
        if (!in) return ReadResult::FILE_NOT_HANDLED;;
        return readNode(in, lOptions.get());
    }

    virtual ReadResult readImage(const std::string& path, const Options* options) const
    {
        std::string ext; std::string fileName = getRealFileName(path, ext);
        if (fileName.empty()) return ReadResult::FILE_NOT_HANDLED;

        osg::ref_ptr<Options> lOptions = options ?
            static_cast<Options*>(options->clone(osg::CopyOp::SHALLOW_COPY)) : new Options;
        lOptions->setPluginStringData("STREAM_FILENAME", osgDB::getSimpleFileName(fileName));
        lOptions->setPluginStringData("S57_BASE_PATH", fileName);

        std::ifstream in(fileName, std::ios::in | std::ios::binary);
        if (!in) return ReadResult::FILE_NOT_HANDLED;
        return readImage(in, lOptions.get());
    }

    virtual ReadResult readObject(std::istream& fin, const Options* options) const
    {
        std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(fin)),
                                    std::istreambuf_iterator<char>());
        if (buffer.empty()) return ReadResult::ERROR_IN_READING_FILE;

        osgS57::I8211::Iso8211Reader rdr;
        osgS57::I8211::ParseResult pr = rdr.openMemory(buffer);
        if (!pr.ok)
        {
            OSG_WARN << "[ReaderWriterS57] " << pr.message << std::endl;
            return ReadResult::ERROR_IN_READING_FILE;
        }

        osg::ref_ptr<osgVerse::FeatureCollection> collection = loadCollection(rdr, options);
        if (!collection) return ReadResult::ERROR_IN_READING_FILE;
        return collection.get();
    }

    virtual ReadResult readNode(std::istream& fin, const Options* options) const
    {
        std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(fin)),
                             std::istreambuf_iterator<char>());
        if (buffer.empty()) return ReadResult::ERROR_IN_READING_FILE;

        osgS57::I8211::Iso8211Reader rdr;
        osgS57::I8211::ParseResult pr = rdr.openMemory(buffer);
        if (!pr.ok)
        {
            OSG_WARN << "[ReaderWriterS57] " << pr.message << std::endl;
            return ReadResult::ERROR_IN_READING_FILE;
        }

        osg::ref_ptr<osgVerse::FeatureCollection> collection = loadCollection(rdr, options);
        if (!collection) return ReadResult::ERROR_IN_READING_FILE;

        osg::ref_ptr<osg::Geode> geode = new osg::Geode; osg::ref_ptr<osg::Geometry> geom;
        for (size_t i = 0; i < collection->features.size(); ++i)
        {
            if (!geom)
            {
                geom = new osg::Geometry; geode->addDrawable(geom.get());
                geom->setUseDisplayList(false); geom->setUseVertexBufferObjects(true);
            }

            osgVerse::Feature* feature = collection->features[i];
            osgVerse::addFeatureToGeometry(*feature, geom.get(), true);
            if (geom->getNumPrimitiveSets() > 1024) geom = NULL;
        }

        // expose the original data bounds so callers can frame the camera
        const osg::BoundingBox& bb = collection->bound;
        if (bb.valid())
        {
            geode->setUserValue("BoundMinX", (double)bb.xMin());
            geode->setUserValue("BoundMinY", (double)bb.yMin());
            geode->setUserValue("BoundMinZ", (double)bb.zMin());
            geode->setUserValue("BoundMaxX", (double)bb.xMax());
            geode->setUserValue("BoundMaxY", (double)bb.yMax());
            geode->setUserValue("BoundMaxZ", (double)bb.zMax());
        }

        // CRS metadata propagated from the FeatureCollection
        std::string crsName, datumName;
        double hdat = 0.0;
        collection->getUserValue("CRS", crsName);
        collection->getUserValue("HorizontalDatum", hdat);
        if (!crsName.empty()) geode->setUserValue("CRS", crsName);
        if (hdat > 0.0) geode->setUserValue("HorizontalDatum", hdat);

        if (options)
        {
            int toInc = atoi(options->getPluginStringData("IncludeFeatures").c_str());
            if (toInc > 0) geode->setUserData(collection.get());
        }
        return geode.get();
    }

    virtual ReadResult readImage(std::istream& fin, const Options* options) const
    {
        std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(fin)),
                                    std::istreambuf_iterator<char>());
        if (buffer.empty()) return ReadResult::ERROR_IN_READING_FILE;

        osgS57::I8211::Iso8211Reader rdr;
        osgS57::I8211::ParseResult pr = rdr.openMemory(buffer);
        if (!pr.ok)
        {
            OSG_WARN << "[ReaderWriterS57] " << pr.message << std::endl;
            return ReadResult::ERROR_IN_READING_FILE;
        }

        // the raster path scales feature coordinates to pixels, so it always
        // works in local metres regardless of the requested OutputCRS
        osg::ref_ptr<Options> imgOptions = options ?
            static_cast<Options*>(options->clone(osg::CopyOp::SHALLOW_COPY)) : new Options;
        imgOptions->setPluginStringData("OutputCRS", "Local");

        osg::ref_ptr<osgVerse::FeatureCollection> fc = loadCollection(rdr, imgOptions.get());
        if (!fc) return ReadResult::ERROR_IN_READING_FILE;

        std::string wStr = options ? options->getPluginStringData("ImageWidth") : "512";
        std::string hStr = options ? options->getPluginStringData("ImageHeight") : "512";
        int w = atoi(wStr.c_str()), h = atoi(hStr.c_str()); if (w < 1) w = 512; if (h < 1) h = 512;

        osg::ref_ptr<osgVerse::Drawer2D> drawer = new osgVerse::Drawer2D;
        if (options)
        {
            int toInc = atoi(options->getPluginStringData("IncludeFeatures").c_str());
            if (toInc > 0) drawer->setUserData(fc.get());
        }
        drawer->allocateImage(w, h, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        drawer->setPixelBufferObject(new osg::PixelBufferObject(drawer.get()));
        drawer->start(false);
        // opaque sea background; a transparent one makes the biggest area
        // feature (which covers the whole cell) look like a blank white image
        drawer->fillBackground(osg::Vec4(214.0f / 255.0f, 233.0f / 255.0f, 248.0f / 255.0f, 1.0f));

        const osg::BoundingBox& bb = fc->bound;
        float spanX = (float)(bb.xMax() - bb.xMin()), spanY = (float)(bb.yMax() - bb.yMin());
        if (spanX < 1e-3f) spanX = 1.0f;
        if (spanY < 1e-3f) spanY = 1.0f;
        // y flipped so that north is up (image rows run downwards)
        osg::Vec2 off(-(float)bb.xMin(), -(float)bb.yMax()), sc((float)w / spanX, -(float)h / spanY);

        // minimal chart palette, mirroring the S-52-ish colours used by the
        // independent s57render GDI+ reference tool
        auto fillColor = [](osgVerse::Feature* f) -> osg::Vec4
        {
            std::string acr; f->getUserValue("ObjectAcronym", acr);
            if (acr == "DEPARE") return osg::Vec4(0.66f, 0.85f, 0.97f, 1.0f);
            if (acr == "LNDARE" || acr == "LNDRGN" || acr == "LNDELV")
                return osg::Vec4(0.886f, 0.839f, 0.690f, 1.0f);
            if (acr == "BUAARE") return osg::Vec4(0.816f, 0.784f, 0.698f, 1.0f);
            if (acr == "DRGARE" || acr == "DMPGRD") return osg::Vec4(0.784f, 0.824f, 0.471f, 1.0f);
            if (acr == "SEAARE") return osg::Vec4(0.647f, 0.647f, 0.784f, 1.0f);
            return osgS57::featureColor(f, osg::Vec4(0.78f, 0.78f, 0.82f, 1.0f));
        };

        // display-priority passes: area fills -> area outlines -> lines -> points
        // (otherwise a cell-wide DEPARE/M_COVR polygon hides everything after it)
        bool showMeta = options &&
            atoi(options->getPluginStringData("ShowMetaObjects").c_str()) > 0;
        for (int pass = 0; pass < 4; ++pass)
        {
            for (size_t i = 0; i < fc->features.size(); ++i)
            {
                osgVerse::Feature* feature = fc->features[i];
                double isMeta = 0.0; feature->getUserValue("IsMetaObject", isMeta);
                if (!showMeta && isMeta > 0.0) continue;   // presentation-only
                GLenum type = feature->getType();
                osgVerse::DrawerStyleData style;

                if (pass == 0 && type == GL_POLYGON)
                    style = osgVerse::DrawerStyleData(fillColor(feature), true);
                else if (pass == 1 && type == GL_POLYGON)
                    style = osgVerse::DrawerStyleData(osg::Vec4(0.16f, 0.24f, 0.31f, 0.55f), false);
                else if (pass == 2 && (type == GL_LINE_STRIP || type == GL_LINE_LOOP))
                {
                    std::string acr; feature->getUserValue("ObjectAcronym", acr);
                    bool major = (acr == "DEPCNT" || acr == "COALNE");
                    style = osgVerse::DrawerStyleData(major ?
                        osg::Vec4(0.12f, 0.24f, 0.35f, 1.0f) :
                        osg::Vec4(0.55f, 0.42f, 0.34f, 0.75f), false);
                }
                else if (pass == 3 && type == GL_POINTS)
                    style = osgVerse::DrawerStyleData(osg::Vec4(0.90f, 0.47f, 0.08f, 0.90f), true);
                else continue;

                osgVerse::drawFeatureToImage(*feature, drawer.get(), off, sc, &style);
            }
        }
        drawer->finish(); return drawer.get();
    }

protected:
    std::string getRealFileName(const std::string& path, std::string& ext) const
    {
        std::string fileName(path); ext = osgDB::getLowerCaseFileExtension(path);
        if (!acceptsExtension(ext)) return "";

        bool usePseudo = (ext == "verse_s57");
        if (usePseudo)
        {
            fileName = osgDB::getNameLessExtension(path);
            ext = osgDB::getFileExtension(fileName);
        }
        return fileName;
    }
};

// Now register with Registry to instantiate the above reader/writer.
REGISTER_OSGPLUGIN(verse_s57, ReaderWriterS57)
