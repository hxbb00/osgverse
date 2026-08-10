#ifndef MANA_MODELING_ANNOTATIONCLUSTER_HPP
#define MANA_MODELING_ANNOTATIONCLUSTER_HPP

#include <osg/Geode>
#include <osg/Geometry>
#include <functional>
#include <map>
#include <vector>
#include <iostream>

namespace osgVerse
{

struct AnnotationCell
{
    std::map<std::string, std::string> properties;
    osg::observer_ptr<osg::Node> node;
    std::string label;
    osg::Vec3d worldPos;
    double priority;
    
    AnnotationCell(const osg::Vec3d& pos, osg::Node* n, const std::string& lbl) 
    : node(n), label(lbl), worldPos(pos), priority(1.0) {}
};

/** Cluster massive of annotations in to fewer groups */
class AnnotationCluster : public osg::Referenced
{
public:
    struct TileInfo
    {
        unsigned int zoom, x, y; bool valid;
        TileInfo() : zoom(0), x(0), y(0), valid(false) {}
        TileInfo(unsigned int z, unsigned int x_, unsigned int y_) : zoom(z), x(x_), y(y_), valid(true) {}
        bool operator==(const TileInfo& other) const { return zoom == other.zoom && x == other.x && y == other.y;}
        bool operator!=(const TileInfo& other) const { return !(*this == other); }
    };

    AnnotationCluster();
    void setClusterOptions(float radius = 40.0f, int maxZoom = 16);

    osg::Node* getClusterNode() { return _clusterContainer.get(); }
    const osg::Node* getClusterNode() const { return _clusterContainer.get(); }
    
    void setConvertCellToLatLon(bool b) { _convToLatLon = b; }
    bool getConvertCellToLatLon() const { return _convToLatLon; }

    typedef std::function<void (AnnotationCell&, bool shown)> VisibilityFunc;
    void setCellVisibilityFunc(VisibilityFunc v) { _visibilityFunc = v; }
    VisibilityFunc getCellVisibilityFunc() { return _visibilityFunc; }

    typedef std::function<osg::Node* (const osg::Vec3d&, unsigned int clusterID, int count)> ClusterGroupFunc;
    void setClusterGroupFunc(ClusterGroupFunc v) { _createClusterFunc = v; }
    ClusterGroupFunc setClusterGroupFunc() { return _createClusterFunc; }

    unsigned int add(const osg::Vec3d& worldPos, osg::Node* node, 
                     const std::string& label, double priority = 1.0);
    void set(unsigned int id, AnnotationCell& cell);
    void remove(unsigned int id);
    void dirty(bool rebuildAllCells);
    size_t size() const { return _cells.size(); }

    const std::vector<AnnotationCell>& getAllCells() const { return _cells; }
    std::vector<AnnotationCell>& getAllCells() { return _cells; }
    
    void update(osg::Camera* camera, double refDistance = 1.0);
    bool getLeaves(std::vector<AnnotationCell>& leaves, unsigned int clusterID,
                   unsigned int num = 1000, unsigned int offset = 0);
    
protected:
    virtual ~AnnotationCluster() {}
    TileInfo getTileIndexForCamera(osg::Camera* camera, const osg::Vec3d& eye, double distanceToCenter);
    double calculateZoomLevel(osg::Camera* camera, double distanceToCenter);
    void rebuildClusters(const TileInfo& tileInfo);
    void rebuild(AnnotationCell& cell, int index);
    
    std::vector<AnnotationCell> _cells;
    osg::ref_ptr<osg::Referenced> _internal;
    osg::ref_ptr<osg::Group> _clusterContainer;
    
    VisibilityFunc _visibilityFunc;
    ClusterGroupFunc _createClusterFunc;
    TileInfo _currentTile;
    osg::Vec3d _lastCameraPos;
    int _recollectFlag;
    bool _needUpdate, _convToLatLon;
};

}

#endif
