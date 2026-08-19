#include <mapbox/supercluster.hpp>
#include <osg/MatrixTransform>
#include "AnnotationCluster.h"
#include "Math.h"
#include <limits.h>
using namespace osgVerse;

namespace
{
    static const double EARTH_RADIUS = 6371000.0;
    static const double EARTH_CIRCUMFERENCE = 2.0 * osg::PI * EARTH_RADIUS;

    void webMercatorToLatLon(double worldX, double worldY, double& lat, double& lon)
    {
        lon = worldX * 360.0 - 180.0;
        if (worldY <= 0.0 || worldY >= 1.0)
            lat = 90.0 - worldY * 180.0;
        else
        {
            double y = 0.5 - worldY;
            lat = 2.0 * std::atan(std::exp(y * 2.0 * osg::PI)) * 180.0 / osg::PI - 90.0;
        }
    }

    void getTileBounds(int zoom, int x, int y, double& minLat, double& maxLat, 
                       double& minLon, double& maxLon)
    {
        double tiles = std::pow(2.0, zoom);
        double minWorldX = static_cast<double>(x) / tiles;
        double maxWorldX = static_cast<double>(x + 1) / tiles;
        double minWorldY = static_cast<double>(y) / tiles;
        double maxWorldY = static_cast<double>(y + 1) / tiles;
        
        webMercatorToLatLon(minWorldX, minWorldY, minLat, minLon);
        webMercatorToLatLon(maxWorldX, maxWorldY, maxLat, maxLon);
        if (minLat > maxLat) std::swap(minLat, maxLat);
        if (minLon > maxLon) std::swap(minLon, maxLon);
    }
}

struct SuperCluster : public osg::Referenced
{
    mapbox::feature::feature_collection<double> features;
    std::unique_ptr<mapbox::supercluster::Supercluster> engine;
    mapbox::supercluster::Options options;

    SuperCluster()
    {
        options.minZoom = 0; options.maxZoom = 16;
        options.radius = 40; options.extent = 256; options.minPoints = 2;
    }

    osg::Vec3d tileCoordToWorld(const AnnotationCluster::TileInfo& tile,
                                const mapbox::geometry::point<std::int16_t>& tilePoint) const
    {
        double normalizedX = static_cast<double>(tilePoint.x) / (double)options.extent;
        double normalizedY = static_cast<double>(tilePoint.y) / (double)options.extent;
        double tiles = std::pow(2.0, tile.zoom);

        double worldX = (static_cast<double>(tile.x) + normalizedX) / tiles;
        double worldY = (static_cast<double>(tile.y) + normalizedY) / tiles;
        double lon = osg::inDegrees(worldX * 360.0 - 180.0), lat = 0.0;
        if (worldY <= 0.0 || worldY >= 1.0)
            lat = osg::inDegrees(90.0 - worldY * 180.0);
        else
        {
            double y = 0.5 - worldY;
            lat = 2.0 * std::atan(std::exp(y * 2.0 * osg::PI)) - osg::PI_2;
        }
        return osg::Vec3d(lat, lon, 0.0);
    }

    static bool isTileVisible(const AnnotationCluster::TileInfo& tile,
                              osg::Polytope& frustum, const osg::Vec3d& eyePos)
    {
        double minLat = 0.0, maxLat = 0.0, minLon = 0.0, maxLon = 0.0;
        getTileBounds(tile.zoom, tile.x, tile.y, minLat, maxLat, minLon, maxLon);
        
        double centerLat = (minLat + maxLat) / 2.0, centerLon = (minLon + maxLon) / 2.0;
        osg::Vec3d ecef = Coordinate::convertLLAtoECEF(osg::Vec3d(centerLat, centerLon, 0.0));
        osg::Vec3d viewDir = eyePos; viewDir.normalize();
        osg::Vec3d normalDir = ecef; normalDir.normalize();
        double dotProduct = viewDir * normalDir;
        if (dotProduct < -0.0) return false;  // back of earth
        
        double latSpan = (maxLat - minLat), lonSpan = (maxLon - minLon);
        double angularRadius = osg::maximum(latSpan, lonSpan) / 2.0;
        double radius = EARTH_RADIUS * angularRadius * 1.2;
        return frustum.contains(osg::BoundingSphere(ecef, radius));
    }
};

AnnotationCluster::AnnotationCluster()
:   _recollectFlag(-1), _needUpdate(true), _convToLatLon(true)
{
    _visibilityFunc = [](AnnotationCell& c, bool shown) {
        if (c.node.valid()) c.node->setNodeMask(shown ? 0xffffffff : 0);
    };
    _clusterContainer = new osg::Group; _clusterContainer->setName("AnnotationCluster");
    _internal = new SuperCluster; _currentZoom = -1;
}

void AnnotationCluster::setClusterOptions(float radius, int maxZoom)
{
    SuperCluster* sc = static_cast<SuperCluster*>(_internal.get());
    sc->options.radius = static_cast<unsigned short>(radius);
    sc->options.maxZoom = static_cast<uint8_t>(maxZoom);
    _needUpdate = true;
}

unsigned int AnnotationCluster::add(const osg::Vec3d& worldPos, osg::Node* node, 
                                    const std::string& label, double priority)
{
    AnnotationCell cell(worldPos, node, label); cell.priority = priority;
    unsigned int index = _cells.size(); _cells.push_back(cell);
    if (_recollectFlag < 0) _recollectFlag = (int)index;
    else _recollectFlag = INT_MAX;  // rebuild all cells
    _needUpdate = true; return index;
}

void AnnotationCluster::set(unsigned int id, AnnotationCell& cell)
{
    if (id < _cells.size())
    {
        _cells[id] = cell; _needUpdate = true;
        if (_recollectFlag < 0) _recollectFlag = (int)id;
        else _recollectFlag = INT_MAX;  // rebuild all cells
    }
    else
        add(cell.worldPos, cell.node.get(), cell.label, cell.priority);
}

void AnnotationCluster::remove(unsigned int id)
{
    if (id < _cells.size())
    {
        _cells.erase(_cells.begin() + id); _needUpdate = true;
        _recollectFlag = INT_MAX;  // rebuild all cells
    }
}

void AnnotationCluster::dirty(bool rebuildAllCells)
{
    _needUpdate = true;
    if (rebuildAllCells) _recollectFlag = INT_MAX;
}

void AnnotationCluster::update(osg::Camera* camera, double refDistance)
{
    SuperCluster* sc = static_cast<SuperCluster*>(_internal.get());
    if (!camera || !sc) return;

    osg::Vec3d eye, center, up;
    camera->getViewMatrixAsLookAt(eye, center, up, refDistance);
    double distanceToCenter = eye.length();
    int zoom = (int)std::round(calculateZoomLevel(camera, distanceToCenter));
    if (!_needUpdate && zoom == _currentZoom)
    {
        double distance = (eye - _lastCameraPos).length();
        if (distance < 10.0) return;
    }

    // Pre-work...
    if (_visibilityFunc) { for (size_t i = 0; i < _cells.size(); ++i) _visibilityFunc(_cells[i], false); }
    _clusterContainer->removeChildren(0, _clusterContainer->getNumChildren());

    // Traverse all possible tiles at current zoom
    int tiles = (int)std::pow(2.0, (double)zoom);
    osg::Polytope frustum; frustum.setToUnitFrustum(true, true);
    frustum.transformProvidingInverse(camera->getViewMatrix() * camera->getProjectionMatrix());
    for (int y = 0; y < tiles; ++y)
        for (int x = 0; x < tiles; ++x)
        {
            TileInfo tile = TileInfo(zoom, x, y);
            if (SuperCluster::isTileVisible(tile, frustum, eye)) rebuildClusters(tile);
        }
    _lastCameraPos = eye; _currentZoom = zoom; _needUpdate = false;
}

bool AnnotationCluster::getLeaves(std::vector<AnnotationCell>& leaves, unsigned int clusterID,
                                  unsigned int num, unsigned int offset)
{
    SuperCluster* sc = static_cast<SuperCluster*>(_internal.get());
    if (!sc || (sc && !sc->engine)) return false;
    
    try
    {
        auto results = sc->engine->getLeaves(clusterID, num, offset);
        for (auto& leaf : results)
        {
            auto indexIt = leaf.properties.find("cellIndex");
            if (indexIt != leaf.properties.end() && indexIt->second.is<size_t>())
            {
                size_t id = indexIt->second.get<size_t>();
                if (id < _cells.size()) leaves.push_back(_cells[id]);
            }
        }
    }
    catch(const std::exception& e)
        { OSG_WARN << "[AnnotationCluster] Failed with: " << e.what() << std::endl; }
    return !leaves.empty();
}

double AnnotationCluster::calculateZoomLevel(osg::Camera* camera, double distanceToCenter)
{
    SuperCluster* sc = static_cast<SuperCluster*>(_internal.get());
    const osg::Viewport* viewport = camera->getViewport();
    if (!viewport) return 0.0;
    
    double width = viewport->width(), height = viewport->height(), fovy = 45.0;
    osg::Matrixd projMat = camera->getProjectionMatrix();
    if (projMat.ptr()[5] != 0.0)
        fovy = 2.0 * std::atan(1.0 / projMat.ptr()[5]) * 180.0 / osg::PI;
    
    double altitude = distanceToCenter - EARTH_RADIUS;
    double pixelSize = 2.0 * altitude * std::tan(fovy * osg::PI / 360.0) / height;
    double zoom = std::log2(EARTH_CIRCUMFERENCE / (pixelSize * sc->options.extent));
    return osg::maximum(static_cast<double>(sc->options.minZoom), 
                        osg::minimum(static_cast<double>(sc->options.maxZoom + 1), zoom));
}

void AnnotationCluster::rebuildClusters(const TileInfo& tileInfo)
{
    SuperCluster* sc = static_cast<SuperCluster*>(_internal.get());
    if (_cells.empty()) return;
    
    // Collect features and cluster
    if (_recollectFlag >= 0)
    {
        if (_recollectFlag < _cells.size())
            rebuild(_cells[_recollectFlag], _recollectFlag);
        else
        {
            sc->features.clear();
            for (size_t i = 0; i < _cells.size(); ++i) rebuild(_cells[i], -1);
        }

        sc->engine = std::make_unique<mapbox::supercluster::Supercluster>(sc->features, sc->options);
        _recollectFlag = -1;
    }
    else if (!sc->engine)
        { OSG_WARN << "[AnnotationCluster] No supercluster engine\n"; return; }

    // Get new cluster results  // FIXME: high level may contain no features... How to find that?
    auto tileFeatures = sc->engine->getTile(tileInfo.zoom, tileInfo.x, tileInfo.y);
    for (auto& feature : tileFeatures)
    {
        osg::ref_ptr<osg::Node> node;
        if (feature.properties["cluster"].is<bool>() && feature.properties["cluster"].get<bool>())
        {
            uint64_t clusterId = feature.properties["cluster_id"].get<uint64_t>();
            uint64_t ptCount = feature.properties["point_count"].get<uint64_t>();
            auto& point = feature.geometry.get<mapbox::geometry::point<std::int16_t>>();
            osg::Vec3d worldPos = sc->tileCoordToWorld(tileInfo, point);
            if (_createClusterFunc)
            {
                osg::ref_ptr<osg::Node> node = _createClusterFunc(worldPos, (unsigned int)clusterId, (unsigned int)ptCount);
                if (node.valid()) _clusterContainer->addChild(node.get());
            }
        }
        else if (feature.properties["cellIndex"].is<size_t>())
        {
            size_t index = feature.properties["cellIndex"].get<size_t>();
            if (index < _cells.size() && _visibilityFunc) _visibilityFunc(_cells[index], true);
        }
    }
}

void AnnotationCluster::rebuild(AnnotationCell& cell, int index)
{
    osg::Vec3d p = _convToLatLon ? Coordinate::convertECEFtoLLA(cell.worldPos) : cell.worldPos;
    mapbox::geometry::point<double> pos(
        osg::RadiansToDegrees(p[1]), osg::RadiansToDegrees(p[0]));  // lon, lat
    mapbox::feature::feature<double> feature;
    feature.geometry = pos;
    
    mapbox::feature::property_map props;
    SuperCluster* sc = static_cast<SuperCluster*>(_internal.get());
    props["cellIndex"] = (index < 0) ? sc->features.size() : size_t(index);
    props["label"] = cell.label;
    props["priority"] = cell.priority;
    feature.properties = props;
    if (index < 0) sc->features.push_back(feature);
    else if (index < sc->features.size()) sc->features[index] = feature;
}
