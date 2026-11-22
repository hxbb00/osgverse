// ============================================================================
// osgS57/src/s57chart.cpp
// ============================================================================
#include "s57chart.h"

#include <cctype>

namespace osgS57 {
namespace S57 {

namespace {
long long optInt(const I8211::Iso8211Reader& rdr, const I8211::RecordDef& rec,
                 const char* tag, const char* sub, bool& ok) {
    return rdr.intSubfield(rec, tag, 0, sub, 0, &ok);
}
std::string optStr(const I8211::Iso8211Reader& rdr,
                   const I8211::RecordDef& rec, const char* tag,
                   const char* sub, bool& ok) {
    return rdr.stringSubfield(rec, tag, 0, sub, 0, &ok);
}
}  // namespace

DatasetParameters readDspm(const I8211::Iso8211Reader& rdr,
                           const I8211::RecordDef& rec) {
    DatasetParameters p;
    bool ok = false;
    optInt(rdr, rec, "DSPM", "RCNM", ok);
    if (!ok) return p;  // no DSPM field

    p.present = true;
    p.rcnm = rdr.intSubfield(rec, "DSPM", 0, "RCNM", 0, &ok);
    p.rcid = rdr.intSubfield(rec, "DSPM", 0, "RCID", 0, &ok);
    p.hdat = rdr.intSubfield(rec, "DSPM", 0, "HDAT", 0, &ok);
    p.vdat = rdr.intSubfield(rec, "DSPM", 0, "VDAT", 0, &ok);
    p.sdat = rdr.intSubfield(rec, "DSPM", 0, "SDAT", 0, &ok);
    p.cscl = rdr.intSubfield(rec, "DSPM", 0, "CSCL", 0, &ok);
    p.duni = rdr.intSubfield(rec, "DSPM", 0, "DUNI", 0, &ok);
    p.huni = rdr.intSubfield(rec, "DSPM", 0, "HUNI", 0, &ok);
    p.puni = rdr.intSubfield(rec, "DSPM", 0, "PUNI", 0, &ok);
    p.coun = rdr.intSubfield(rec, "DSPM", 0, "COUN", 0, &ok);
    p.comf = rdr.intSubfield(rec, "DSPM", 0, "COMF", 0, &ok);
    p.somf = rdr.intSubfield(rec, "DSPM", 0, "SOMF", 0, &ok);
    p.comt = rdr.stringSubfield(rec, "DSPM", 0, "COMT", 0, &ok);
    return p;
}

std::vector<ChartPoint> spatialCoordinates(const I8211::Iso8211Reader& rdr,
                                           const I8211::RecordDef& rec,
                                           const DatasetParameters& dspm) {
    std::vector<ChartPoint> pts;
    double sx = dspm.positionScale();
    double sd = dspm.soundingScale();

    for (const char* tag : {"SG3D", "SG2D"}) {
        const I8211::FieldDefn* defn = rdr.fieldDefns().empty() ? nullptr
                                   : rdr.findFieldDefn(tag);
        if (!defn) continue;
        const I8211::FieldDef* fld = rec.find(tag);
        if (!fld) continue;

        // is the group actually laid out as YCOO/XCOO(/VE3D)?
        bool hasX = defn->findSubfield("XCOO") != nullptr;
        bool hasY = defn->findSubfield("YCOO") != nullptr;
        bool hasZ = defn->findSubfield("VE3D") != nullptr;
        if (!hasX || !hasY) continue;

        int reps = rdr.repeatCount(*fld);
        for (int k = 0; k < reps; ++k) {
            ChartPoint pt;
            bool ok = false;
            long long x = rdr.intSubfield(rec, tag, 0, "XCOO", k, &ok);
            if (!ok) continue;
            long long y = rdr.intSubfield(rec, tag, 0, "YCOO", k, &ok);
            if (!ok) continue;
            pt.lon = static_cast<double>(x) / sx;
            pt.lat = static_cast<double>(y) / sx;
            if (hasZ) {
                pt.depth = static_cast<double>(
                               rdr.intSubfield(rec, tag, 0, "VE3D", k, &ok)) /
                           sd;
                pt.hasDepth = ok;
            }
            pt.valid = true;
            pts.push_back(pt);
        }
        if (!pts.empty()) break;  // only one coordinate field per record
    }
    return pts;
}

std::vector<AttributeInstance> readAttributes(
    const I8211::Iso8211Reader& rdr, const I8211::RecordDef& rec) {
    std::vector<AttributeInstance> out;
    for (const char* tag : {"ATTF", "NATF"}) {
        const I8211::FieldDefn* defn = rdr.findFieldDefn(tag);
        if (!defn) continue;
        const I8211::FieldDef* fld = rec.find(tag);
        if (!fld || !defn->findSubfield("ATTL") || !defn->findSubfield("ATVL"))
            continue;
        int reps = rdr.repeatCount(*fld);
        for (int k = 0; k < reps; ++k) {
            bool ok = false;
            long long code = rdr.intSubfield(rec, tag, 0, "ATTL", k, &ok);
            std::string val = rdr.stringSubfield(rec, tag, 0, "ATVL", k, &ok);
            AttributeInstance a;
            a.code = code;
            a.value = val;
            out.push_back(a);
        }
    }
    return out;
}

AttrType classifyAttribute(const std::string& v) {
    if (v.empty()) return AttrType::Text;
    size_t i = (v[0] == '+' || v[0] == '-') ? 1 : 0;
    if (i >= v.size()) return AttrType::Text;
    bool digits = false, dot = false;
    for (; i < v.size(); ++i) {
        char c = v[i];
        if (c >= '0' && c <= '9') { digits = true; continue; }
        if (c == '.' && !dot) { dot = true; continue; }
        return AttrType::Text;   // anything else (space, comma, ...) = text
    }
    if (!digits) return AttrType::Text;   // ".", "-.", ...
    return dot ? AttrType::Real : AttrType::Integer;
}

bool attributeAsDouble(const std::string& v, double& out) {
    if (classifyAttribute(v) == AttrType::Text) return false;
    out = strtod(v.c_str(), nullptr);
    return true;
}

double wrapLon180(double lonDiff) {
    // (dl + 180) may itself overflow the range at |dl| > 540 deg, so use
    // fmod first; ENC cells never span more than a few degrees anyway.
    double dl = fmod(lonDiff + 180.0, 360.0);
    if (dl <= 0.0) dl += 360.0;
    return dl - 180.0;
}

MolodenskyParams molodenskyForHdat(long long hdat) {
    MolodenskyParams p;
    // HDAT=2 is WGS-84 (S-57 Appendix A / ENC UOC).  Unknown codes are left
    // as identity too; callers warn, wrong conversion is worse than none.
    if (hdat == 0 || hdat == 2) return p;
    p.identity = false;
    // Classic geodetic datums still seen in legacy paper-chart-derived ENCs.
    // (Ellipsoid + published 3-parameter shifts to WGS-84, h=0.)
    switch (hdat) {
    case 1:   // WGS-72
        p.a = 6378135.0; p.rf = 298.26; p.dx = 0.0; p.dy = 0.0; p.dz = 4.5;
        break;
    case 5:   // NAD-27 (Clarke 1866)
        p.a = 6378206.4; p.rf = 294.9786982;
        p.dx = -8.0; p.dy = 160.0; p.dz = 176.0;
        break;
    case 6:   // NAD-83 (GRS 80) -- effectively WGS-84
        p.a = 6378137.0; p.rf = 298.257222101; p.dx = 0.0; p.dy = 0.0; p.dz = 0.0;
        break;
    case 7:   // Tokyo (Bessel 1841)
        p.a = 6377397.155; p.rf = 299.1528128;
        p.dx = -148.0; p.dy = 507.0; p.dz = 685.0;
        break;
    case 20:  // European 1950 (Hayford/International 1924)
        p.a = 6378388.0; p.rf = 297.0;
        p.dx = -87.0; p.dy = -96.0; p.dz = -120.0;
        break;
    default:  // keep identity; caller warns about unconverted data
        p.identity = true;
        p.known = false;
        break;
    }
    return p;
}

void molodenskyToWGS84(const MolodenskyParams& p,
                       double lonDeg, double latDeg,
                       double& lonOutDeg, double& latOutDeg) {
    if (p.identity) { lonOutDeg = lonDeg; latOutDeg = latDeg; return; }

    const double D2R = 3.14159265358979323846 / 180.0;
    const double R2D = 180.0 / 3.14159265358979323846;
    double lam = lonDeg * D2R, phi = latDeg * D2R, h = 0.0;
    double f = 1.0 / p.rf, e2 = f * (2.0 - f);

    // local ellipsoid -> ECEF
    double sinPhi = sin(phi), cosPhi = cos(phi);
    double N = p.a / sqrt(1.0 - e2 * sinPhi * sinPhi);
    double X = (N + h) * cosPhi * cos(lam);
    double Y = (N + h) * cosPhi * sin(lam);
    double Z = (N * (1.0 - e2) + h) * sinPhi;

    // datum shift
    X += p.dx; Y += p.dy; Z += p.dz;

    // ECEF -> geodetic on the WGS-84 ellipsoid (closed-form Bowring)
    double aW = 6378137.0, fW = 1.0 / 298.257223563, e2W = fW * (2.0 - fW);
    double r = sqrt(X * X + Y * Y);
    double theta = atan2(Z * aW, r * aW * (1.0 - e2W));
    double latW = atan2(Z + aW * e2W * sin(theta) * sin(theta) * sin(theta),
                        r - e2W * aW * cos(theta) * cos(theta) * cos(theta));
    double lonW = atan2(Y, X);
    // points shifted across a pole are not a real ENC scenario; lon stays
    // un-wrapped here, the caller applies wrapLon180() as needed
    lonOutDeg = lonW * R2D;
    latOutDeg = latW * R2D;
}

int recordRcnm(const I8211::Iso8211Reader& rdr,
               const I8211::RecordDef& rec) {
    // features and spatial vectors carry RCNM in FRID/VRID
    for (const char* tag : {"FRID", "VRID"}) {
        const I8211::FieldDefn* defn = rdr.findFieldDefn(tag);
        if (!defn) continue;
        const I8211::FieldDef* fld = rec.find(tag);
        if (!fld || !defn->findSubfield("RCNM")) continue;
        bool ok = false;
        int v = static_cast<int>(
            rdr.intSubfield(rec, tag, 0, "RCNM", 0, &ok));
        if (ok) return v;
    }
    // general exchange records (DSID/DSPM ...) identify themselves by tag
    if (rec.find("DSID")) return RCNM_DSID;
    if (rec.find("DSPM")) return RCNM_DSPM;
    if (rec.find("CATD")) return RCNM_CATD;
    return 0;
}

}  // namespace S57
}  // namespace osgS57
