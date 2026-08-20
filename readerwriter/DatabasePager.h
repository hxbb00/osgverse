#ifndef MANA_READERWRITER_DATABASEPAGER_HPP
#define MANA_READERWRITER_DATABASEPAGER_HPP

#include <osg/ProxyNode>
#include <osg/PagedLOD>
#include <osgDB/Registry>
#include <osgDB/DatabasePager>
#include "Export.h"

namespace osgVerse
{

    class OSGVERSE_RW_EXPORT DatabasePager : public osgDB::DatabasePager
    {
    public:
        DatabasePager(bool originalUpdate = false, bool customizedReadQueue = false);
        void setCompressingImages(bool b) { _compressingImages = true; }
        bool getCompressingImages() const { return _compressingImages; }

        void setDrawBoundingBox(bool b) { _drawExtraBBox = true; }
        bool getDrawBoundingBox() const { return _drawExtraBBox; }

        struct DataMergeCallback : public osg::Referenced
        {
            enum FilterResult
            { MERGE_NOW = 0, MERGE_LATER, DISCARDED };

            virtual FilterResult filter(const osg::FrameStamp& fs, osg::PagedLOD*, const std::string&, osg::Node*)
            { return MERGE_NOW; }

            virtual FilterResult filter(const osg::FrameStamp& fs, osg::Group*, const std::string&, osg::Node*)
            { return MERGE_NOW; }

            virtual void merge(const osg::FrameStamp& fs, osg::Group* p, std::vector<osg::ref_ptr<osg::Node>>& n)
            { for (size_t i = 0; i < n.size(); ++i) p->addChild(n[i].get()); n.clear(); }
        };
        void setDataMergeCallback(DataMergeCallback* cb) { _mergeCallback = cb; }
        DataMergeCallback* getDataMergeCallback() const { return _mergeCallback.get(); }

        virtual void updateSceneGraph(const osg::FrameStamp& fs)
        {
            if (_originalUpdate)
                osgDB::DatabasePager::updateSceneGraph(fs);
            else
            {
                removeExpiredSubgraphs(fs);
                addLoadedDataToSceneGraph_Verse(fs);
            }
        }

        void addLoadedDataToSceneGraph_Verse(const osg::FrameStamp& frameStamp);
        virtual void removeExpiredSubgraphs(const osg::FrameStamp& frameStamp);

        virtual void requestNodeFile(const std::string& fileName, osg::NodePath& nodePath,
                                     float priority, const osg::FrameStamp* framestamp,
                                     osg::ref_ptr<osg::Referenced>& databaseRequest,
                                     const osg::Referenced* options);

    protected:
        struct FocusedReadQueue : public osgDB::DatabasePager::ReadQueue
        {
            FocusedReadQueue(DatabasePager* pager, const std::string& name)
                : osgDB::DatabasePager::ReadQueue(pager, name) {}
            virtual void updateBlock();
        };

        virtual ~DatabasePager() {}
        void createBoundingBox(osg::Node* node);

        typedef std::map<osg::ref_ptr<osg::Group>, std::vector<osg::ref_ptr<osg::Node>>> LoadedNodeMap;
        LoadedNodeMap _loadedNodes;
        osg::ref_ptr<DataMergeCallback> _mergeCallback;
        bool _originalUpdate, _compressingImages, _drawExtraBBox;
    };

}

#endif
