#include <mapbox/supercluster.hpp>
#include <osg/MatrixTransform>
#include "AnnotationCluster.h"
#include "Math.h"
#include <limits.h>
using namespace osgVerse;

struct SuperCluster : public osg::Referenced
{
    mapbox::feature::feature_collection<double> features;
    std::unique_ptr<mapbox::supercluster::Supercluster> engine;
    mapbox::supercluster::Options options;

    SuperCluster()
    {
        options.minZoom = 0; options.maxZoom = 18;
        options.radius = 40; options.extent = 512; options.minPoints = 2;
    }

    osg::Vec3d tileCoordToWorld(const AnnotationCluster::TileInfo& tile,
                                const mapbox::geometry::point<std::int16_t>& tilePoint) const
    {
        double normalizedX = static_cast<double>(tilePoint.x) / options.extent;
        double normalizedY = static_cast<double>(tilePoint.y) / options.extent;
        double tiles = std::pow(2.0, tile.zoom);

        double worldX = (static_cast<double>(tile.x) + normalizedX) / tiles;
        double worldY = (static_cast<double>(tile.y) + normalizedY) / tiles;
        double lon = worldX * 360.0 - 180.0, lat = 0.0;
        if (worldY <= 0.0 || worldY >= 1.0)
            lat = 90.0 - worldY * 180.0;
        else
        {
            double y = 0.5 - worldY;
            double angle = 2.0 * std::atan(std::exp(y * 2.0 * osg::PI)) - osg::PI_2;
            lat = angle * 180.0 / osg::PI;
        }
        return osg::Vec3d(lat, lon, 0.0);
    }
};

AnnotationCluster::AnnotationCluster()
:   _recollectFlag(-1), _needUpdate(true), _convToLatLon(true)
{
    _clusterContainer = new osg::Group; _clusterContainer->setName("AnnotationCluster");
    _internal = new SuperCluster;
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
    double distance = (eye - center).length();
    TileInfo currentTile = getTileIndexForCamera(camera, eye, distance);
    if (!currentTile.valid) return;
    
    if (!_needUpdate && currentTile == _currentTile)
    {
        double distance = (eye - _lastCameraPos).length();
        if (distance < 10.0) return;
    }
    rebuildClusters(currentTile); _currentTile = currentTile;
    _lastCameraPos = eye; _needUpdate = false;
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
            auto indexIt = leaf.properties.find("index");
            if (indexIt != leaf.properties.end() && indexIt->second.is<uint64_t>())
            {
                size_t id = (size_t)indexIt->second.get<uint64_t>();
                if (id < _cells.size()) leaves.push_back(_cells[id]);
            }
        }
    }
    catch(const std::exception& e)
        { OSG_WARN << "[AnnotationCluster] Failed with: " << e.what() << std::endl; }
    return !leaves.empty();
}

AnnotationCluster::TileInfo AnnotationCluster::getTileIndexForCamera(osg::Camera* camera, const osg::Vec3d& eye,
                                                                     double distanceToCenter)
{
    SuperCluster* sc = static_cast<SuperCluster*>(_internal.get());
    double zoomLevel = calculateZoomLevel(camera, distanceToCenter);
    uint8_t zoom = static_cast<uint8_t>(std::round(zoomLevel));
    
    osg::Vec3d p = Coordinate::convertECEFtoLLA(eye);
    double normalizedX = (osg::RadiansToDegrees(p[1]) + 180.0) / 360.0;
    double normalizedY = (90.0 - osg::RadiansToDegrees(p[0])) / 180.0;
    
    double tiles = std::pow(2.0, zoom);
    uint32_t tileX = static_cast<uint32_t>(std::floor(normalizedX * tiles));
    uint32_t tileY = static_cast<uint32_t>(std::floor(normalizedY * tiles));
    tileX = osg::maximum(0u, osg::minimum(static_cast<uint32_t>(tiles) - 1, tileX));
    tileY = osg::maximum(0u, osg::minimum(static_cast<uint32_t>(tiles) - 1, tileY));
    return TileInfo(zoom, tileX, tileY);
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
    
    static const double EARTH_CIRCUMFERENCE = 40075000.0;
    double visibleHeight = 2.0 * distanceToCenter * std::tan(fovy * osg::PI / 360.0);
    double visibleWidth = visibleHeight * (width / height), maxVisible = EARTH_CIRCUMFERENCE;
    double zoom = std::log2(maxVisible / osg::maximum(visibleWidth, visibleHeight));
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
        _recollectFlag = -1;
    }

    sc->engine = std::make_unique<mapbox::supercluster::Supercluster>(sc->features, sc->options);
    auto tileFeatures = sc->engine->getTile(tileInfo.zoom, tileInfo.x, tileInfo.y);

    // Get new cluster results
    if (_visibilityFunc) { for (size_t i = 0; i < _cells.size(); ++i) _visibilityFunc(_cells[i], false); }
    _clusterContainer->removeChildren(0, _clusterContainer->getNumChildren());

    for (auto& feature : tileFeatures)
    {
        osg::ref_ptr<osg::Node> node;
        if (feature.properties["cluster"].is<bool>() && feature.properties["cluster"].get<bool>())
        {
            uint32_t clusterId = feature.properties["cluster_id"].get<uint64_t>();
            int ptCount = feature.properties["point_count"].get<uint64_t>();
            auto& point = feature.geometry.get<mapbox::geometry::point<std::int16_t>>();
            osg::Vec3d worldPos = sc->tileCoordToWorld(tileInfo, point);
            if (_createClusterFunc)
            {
                osg::ref_ptr<osg::Node> node = _createClusterFunc(worldPos, clusterId, ptCount);
                if (node.valid()) _clusterContainer->addChild(node.get());
            }
        }
        else
        {
            size_t index = (size_t)feature.properties["index"].get<uint64_t>();
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
    props["index"] = static_cast<uint64_t>(index);
    props["height"] = p.z();
    props["priority"] = cell.priority;
    props["label"] = cell.label;
    feature.properties = props;

    SuperCluster* sc = static_cast<SuperCluster*>(_internal.get());
    if (index < 0) sc->features.push_back(feature);
    else if (index < sc->features.size()) sc->features[index] = feature;
}
