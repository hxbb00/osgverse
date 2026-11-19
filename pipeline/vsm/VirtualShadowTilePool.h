#ifndef OSGVERSE_VIRTUAL_SHADOW_TILE_POOL_H
#define OSGVERSE_VIRTUAL_SHADOW_TILE_POOL_H

#include "VirtualShadowTypes.h"
#include <osg/Referenced>
#include <vector>

namespace osgVerse
{
    /** Physical tile pool of the virtual shadow atlas. Tiles are arranged in a square
        tilingCount x tilingCount grid; only the first tileCount slots are usable. The pool
        only tracks occupancy and LRU order. Mapping a tile to a page and invalidating the
        previous page is the owner's job, because it needs the page table as well. */
    class VirtualShadowTilePool : public osg::Referenced
    {
    public:
        struct TileInfo
        {
            bool inUse;
            bool pinned;
            std::uint64_t lastUsedFrame;
            VirtualShadowPageKey occupant;

            TileInfo() : inUse(false), pinned(false), lastUsedFrame(0) {}
        };

        VirtualShadowTilePool();
        explicit VirtualShadowTilePool(int tileCount, int tileResolution);

        void configure(int tileCount, int tileResolution);
        void clear();

        int getTileCount() const { return _tileCount; }
        int getTileResolution() const { return _tileResolution; }
        int getTilingCount() const { return _tilingCount; }
        int getAtlasResolution() const { return _atlasResolution; }

        bool isInUse(int tile) const;
        bool isPinned(int tile) const;
        int getOccupantTile(const VirtualShadowPageKey& key) const;

        /** First free slot, or -1 when the pool is full. */
        int findFreeTile() const;

        /** Least-recently-used slot that is in use and not pinned, or -1 when every used
            tile is pinned by the current frame. */
        int findEvictableTile() const;
        int findEvictableTile(std::uint64_t frame, std::uint64_t maxAge) const;

        /** Claim a tile for a page. The caller must have resolved eviction already. */
        void markUsed(int tile, const VirtualShadowPageKey& key, std::uint64_t frame);
        void touch(int tile, std::uint64_t frame);
        void setPinned(int tile, bool pinned);
        void release(int tile);

        int getTileX(int tile) const;
        int getTileY(int tile) const;
        VirtualShadowPageKey getOccupant(int tile) const;

        /** Number of tiles currently in use, and the number of pinned ones. */
        int getUsedCount() const;
        int getPinnedCount() const;

    protected:
        std::vector<TileInfo> _tiles;
        int _tileCount, _tileResolution, _tilingCount, _atlasResolution;
    };
}

#endif
