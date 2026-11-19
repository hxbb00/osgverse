#include "VirtualShadowPageTable.h"
#include <cstddef>
#include <utility>

namespace osgVerse
{
    VirtualShadowPageTable::VirtualShadowPageTable()
    :   _lightIndex(-1), _face(0), _pageTableSize(64), _numLevels(1), _dirty(false)
    {}

    VirtualShadowPageTable::VirtualShadowPageTable(int lightIndex, int face,
                                                   int pageTableSize, int maxLevels)
    :   _lightIndex(lightIndex), _face(face), _pageTableSize(64), _numLevels(1), _dirty(false)
    {
        configure(lightIndex, face, pageTableSize, maxLevels);
    }

    void VirtualShadowPageTable::configure(int lightIndex, int face,
                                           int pageTableSize, int maxLevels)
    {
        _lightIndex = lightIndex; _face = face;
        _pageTableSize = (pageTableSize > 1) ? pageTableSize : 1;

        // Keep the finest grid a power of two: the level shift in getGridSize() and the
        // shader-side page lookup both rely on it
        int size = 1;
        while (size < _pageTableSize) size <<= 1;
        _pageTableSize = size;

        int levelLimit = 1;
        while ((size >> levelLimit) > 0) ++levelLimit;  // log2(size) + 1
        _numLevels = (maxLevels > 0) ? ((maxLevels < levelLimit) ? maxLevels : levelLimit)
                                     : levelLimit;

        _dirty = false;
        _pages.clear();
    }

    void VirtualShadowPageTable::clear()
    {
        _pages.clear();
        _dirty = true;
    }

    int VirtualShadowPageTable::getGridSize(int level) const
    {
        if (level < 0 || level >= _numLevels) return 0;
        return _pageTableSize >> level;
    }

    int VirtualShadowPageTable::getNumPagesAtLevel(int level) const
    {
        int size = getGridSize(level);
        return size * size;
    }

    bool VirtualShadowPageTable::isValidKey(const VirtualShadowPageKey& key) const
    {
        if (key.light != _lightIndex || key.face != _face) return false;
        if (key.level < 0 || key.level >= _numLevels) return false;

        int size = getGridSize(key.level);
        return key.x >= 0 && key.y >= 0 && key.x < size && key.y < size;
    }

    VirtualShadowPageRecord* VirtualShadowPageTable::find(const VirtualShadowPageKey& key)
    {
        std::unordered_map<std::uint64_t, VirtualShadowPageRecord>::iterator itr =
            _pages.find(packVirtualShadowPageKey(key));
        return (itr != _pages.end()) ? &(itr->second) : NULL;
    }

    const VirtualShadowPageRecord* VirtualShadowPageTable::find(
        const VirtualShadowPageKey& key) const
    {
        std::unordered_map<std::uint64_t, VirtualShadowPageRecord>::const_iterator itr =
            _pages.find(packVirtualShadowPageKey(key));
        return (itr != _pages.end()) ? &(itr->second) : NULL;
    }

    VirtualShadowPageRecord& VirtualShadowPageTable::getOrCreate(
        const VirtualShadowPageKey& key, std::uint64_t frame)
    {
        VirtualShadowPageKey fixed = normalizeKey(key);
        std::uint64_t packed = packVirtualShadowPageKey(fixed);
        std::unordered_map<std::uint64_t, VirtualShadowPageRecord>::iterator itr =
            _pages.find(packed);
        if (itr == _pages.end())
        {
            VirtualShadowPageRecord record(fixed, frame);
            itr = _pages.insert(std::make_pair(packed, record)).first;
        }
        return itr->second;
    }

    bool VirtualShadowPageTable::isResident(const VirtualShadowPageKey& key) const
    {
        const VirtualShadowPageRecord* record = find(normalizeKey(key));
        return record != NULL && record->state == VirtualShadowPageState::Resident &&
               record->tile >= 0;
    }

    bool VirtualShadowPageTable::markRequested(const VirtualShadowPageKey& key,
                                               std::uint64_t frame)
    {
        VirtualShadowPageRecord& record = getOrCreate(key, frame);
        if (record.state == VirtualShadowPageState::Resident) return false;

        record.state = VirtualShadowPageState::Requested;
        record.lastUsedFrame = frame;
        return true;
    }

    bool VirtualShadowPageTable::markResident(const VirtualShadowPageKey& key,
                                              int tile, std::uint64_t frame)
    {
        if (tile < 0) return false;

        VirtualShadowPageRecord& record = getOrCreate(key, frame);
        record.state = VirtualShadowPageState::Resident;
        record.tile = tile;
        record.lastUsedFrame = frame;
        record.dirty = true; _dirty = true;
        return true;
    }

    bool VirtualShadowPageTable::markUnmapped(const VirtualShadowPageKey& key, bool keepRecord)
    {
        VirtualShadowPageKey fixed = normalizeKey(key);
        std::unordered_map<std::uint64_t, VirtualShadowPageRecord>::iterator itr =
            _pages.find(packVirtualShadowPageKey(fixed));
        if (itr == _pages.end()) return false;

        itr->second.state = VirtualShadowPageState::Unmapped;
        itr->second.tile = -1;
        itr->second.dirty = true; _dirty = true;
        if (!keepRecord) _pages.erase(itr);
        return true;
    }

    VirtualShadowPageKey VirtualShadowPageTable::getFallback(
        const VirtualShadowPageKey& key) const
    {
        VirtualShadowPageKey fixed = normalizeKey(key);
        if (isResident(fixed)) return fixed;

        // Coarser pages cover the same area: parent = (level+1, x>>1, y>>1) and so on until
        // the coarsest level, which the owner keeps resident as the base coverage
        for (int level = fixed.level + 1; level < _numLevels; ++level)
        {
            int shift = level - fixed.level;
            VirtualShadowPageKey parent(fixed.light, fixed.face, level,
                                        fixed.x >> shift, fixed.y >> shift);
            if (isResident(parent)) return parent;
        }
        return VirtualShadowPageKey();
    }

    VirtualShadowPageKey VirtualShadowPageTable::getParent(
        const VirtualShadowPageKey& key, int levels) const
    {
        VirtualShadowPageKey fixed = normalizeKey(key);
        int level = fixed.level + (levels > 0 ? levels : 1);
        if (level >= _numLevels) level = _numLevels - 1;

        int shift = level - fixed.level;
        return VirtualShadowPageKey(fixed.light, fixed.face, level,
                                    fixed.x >> shift, fixed.y >> shift);
    }

    void VirtualShadowPageTable::getResidentPages(
        std::vector<VirtualShadowPageKey>& result) const
    {
        result.clear();
        for (std::unordered_map<std::uint64_t, VirtualShadowPageRecord>::const_iterator
             itr = _pages.begin(); itr != _pages.end(); ++itr)
        {
            if (itr->second.state == VirtualShadowPageState::Resident &&
                itr->second.tile >= 0)
                result.push_back(itr->second.key);
        }
    }

    int VirtualShadowPageTable::getResidentCount() const
    {
        int count = 0;
        for (std::unordered_map<std::uint64_t, VirtualShadowPageRecord>::const_iterator
             itr = _pages.begin(); itr != _pages.end(); ++itr)
        {
            if (itr->second.state == VirtualShadowPageState::Resident &&
                itr->second.tile >= 0)
                ++count;
        }
        return count;
    }

    void VirtualShadowPageTable::touch(const VirtualShadowPageKey& key, std::uint64_t frame)
    {
        VirtualShadowPageRecord* record = find(normalizeKey(key));
        if (record != NULL) record->lastUsedFrame = frame;
    }

    VirtualShadowPageKey VirtualShadowPageTable::normalizeKey(
        const VirtualShadowPageKey& key) const
    {
        VirtualShadowPageKey fixed = key;
        fixed.light = _lightIndex; fixed.face = _face;

        int level = fixed.level;
        if (level < 0) level = 0;
        if (level >= _numLevels) level = _numLevels - 1;

        int size = getGridSize(level);
        int x = fixed.x, y = fixed.y;
        if (size > 0)
        {
            x %= size; if (x < 0) x += size;
            y %= size; if (y < 0) y += size;
        }
        else { x = 0; y = 0; }

        fixed.level = level; fixed.x = x; fixed.y = y;
        return fixed;
    }
}
