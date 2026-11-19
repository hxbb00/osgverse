#ifndef OSGVERSE_VIRTUAL_SHADOW_PAGE_TABLE_H
#define OSGVERSE_VIRTUAL_SHADOW_PAGE_TABLE_H

#include "VirtualShadowTypes.h"
#include <osg/Referenced>
#include <unordered_map>
#include <vector>

namespace osgVerse
{
    /** Multi-level page table of one shadow-casting light (and one cube face). Pages are
        world-anchored in light space: their keys are absolute page coordinates inside a
        fixed light-space region, so a page stays valid across frames while the camera moves.
        Level 0 is the finest level. The coarsest level always contains a single page that
        covers the whole region, which the owner keeps resident as the fallback. */
    class VirtualShadowPageTable : public osg::Referenced
    {
    public:
        VirtualShadowPageTable();
        VirtualShadowPageTable(int lightIndex, int face, int pageTableSize, int maxLevels);

        void configure(int lightIndex, int face, int pageTableSize, int maxLevels);
        void clear();

        int getLightIndex() const { return _lightIndex; }
        int getFace() const { return _face; }
        int getPageTableSize() const { return _pageTableSize; }
        int getNumLevels() const { return _numLevels; }
        int getGridSize(int level) const;
        int getNumPagesAtLevel(int level) const;

        bool isValidKey(const VirtualShadowPageKey& key) const;
        VirtualShadowPageRecord* find(const VirtualShadowPageKey& key);
        const VirtualShadowPageRecord* find(const VirtualShadowPageKey& key) const;

        /** Return the record of a page, creating an unmapped one if needed. A world-anchored
            page that is already resident is not reset, which is what makes cross-frame
            caching work. */
        VirtualShadowPageRecord& getOrCreate(const VirtualShadowPageKey& key,
                                             std::uint64_t frame = 0);

        bool isResident(const VirtualShadowPageKey& key) const;
        bool markRequested(const VirtualShadowPageKey& key, std::uint64_t frame);
        bool markResident(const VirtualShadowPageKey& key, int tile, std::uint64_t frame);
        bool markUnmapped(const VirtualShadowPageKey& key, bool keepRecord = true);

        /** Request the page, or the closest coarser page that is already resident. This is
            the sampling fallback: a missing fine page must not become 'no shadow', it should
            use the coarse page that covers the same area. */
        VirtualShadowPageKey getFallback(const VirtualShadowPageKey& key) const;

        /** Parent page at 'levels' coarser levels, clamped to the coarsest level. */
        VirtualShadowPageKey getParent(const VirtualShadowPageKey& key, int levels = 1) const;

        /** Keys of all resident pages, in no particular order. */
        void getResidentPages(std::vector<VirtualShadowPageKey>& result) const;
        int getResidentCount() const;

        /** Mark that the indirection texture of this table has to be rebuilt. */
        void setDirty() { _dirty = true; }
        bool consumeDirty() { bool d = _dirty; _dirty = false; return d; }
        bool isDirty() const { return _dirty; }

        /** Frame bookkeeping: pages not touched for a long time can be evicted first. */
        void touch(const VirtualShadowPageKey& key, std::uint64_t frame);

    protected:
        VirtualShadowPageKey normalizeKey(const VirtualShadowPageKey& key) const;

        std::unordered_map<std::uint64_t, VirtualShadowPageRecord> _pages;
        int _lightIndex, _face;
        int _pageTableSize, _numLevels;
        bool _dirty;
    };
}

#endif
