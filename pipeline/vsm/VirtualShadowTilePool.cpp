#include "VirtualShadowTilePool.h"
#include <cmath>

namespace osgVerse
{
    VirtualShadowTilePool::VirtualShadowTilePool()
    :   _tileCount(0), _tileResolution(0), _tilingCount(0), _atlasResolution(0)
    {}

    VirtualShadowTilePool::VirtualShadowTilePool(int tileCount, int tileResolution)
    :   _tileCount(0), _tileResolution(0), _tilingCount(0), _atlasResolution(0)
    {
        configure(tileCount, tileResolution);
    }

    void VirtualShadowTilePool::configure(int tileCount, int tileResolution)
    {
        _tileCount = (tileCount > 0) ? tileCount : 0;
        _tileResolution = (tileResolution > 0) ? tileResolution : 1;

        _tilingCount = 1;
        while ((_tilingCount * _tilingCount) < _tileCount) ++_tilingCount;
        _atlasResolution = _tilingCount * _tileResolution;

        _tiles.assign(_tilingCount * _tilingCount, TileInfo());
    }

    void VirtualShadowTilePool::clear()
    {
        _tiles.assign(_tilingCount * _tilingCount, TileInfo());
    }

    bool VirtualShadowTilePool::isInUse(int tile) const
    {
        return tile >= 0 && tile < (int)_tiles.size() && _tiles[tile].inUse;
    }

    bool VirtualShadowTilePool::isPinned(int tile) const
    {
        return tile >= 0 && tile < (int)_tiles.size() && _tiles[tile].pinned;
    }

    int VirtualShadowTilePool::getOccupantTile(const VirtualShadowPageKey& key) const
    {
        for (int i = 0; i < _tileCount; ++i)
        {
            if (_tiles[i].inUse && _tiles[i].occupant == key) return i;
        }
        return -1;
    }

    int VirtualShadowTilePool::findFreeTile() const
    {
        for (int i = 0; i < _tileCount; ++i)
        {
            if (!_tiles[i].inUse) return i;
        }
        return -1;
    }

    int VirtualShadowTilePool::findEvictableTile() const
    {
        int best = -1; std::uint64_t oldest = ~static_cast<std::uint64_t>(0);
        for (int i = 0; i < _tileCount; ++i)
        {
            if (!_tiles[i].inUse || _tiles[i].pinned) continue;
            if (_tiles[i].lastUsedFrame < oldest)
            {
                oldest = _tiles[i].lastUsedFrame; best = i;
            }
        }
        return best;
    }

    int VirtualShadowTilePool::findEvictableTile(std::uint64_t frame,
                                                 std::uint64_t maxAge) const
    {
        if (maxAge == 0) return findEvictableTile();

        int best = -1; std::uint64_t oldest = ~static_cast<std::uint64_t>(0);
        for (int i = 0; i < _tileCount; ++i)
        {
            if (!_tiles[i].inUse || _tiles[i].pinned) continue;
            if (frame > _tiles[i].lastUsedFrame &&
                (frame - _tiles[i].lastUsedFrame) < maxAge) continue;
            if (_tiles[i].lastUsedFrame < oldest)
            {
                oldest = _tiles[i].lastUsedFrame; best = i;
            }
        }
        return best;
    }

    void VirtualShadowTilePool::markUsed(int tile, const VirtualShadowPageKey& key,
                                         std::uint64_t frame)
    {
        if (tile < 0 || tile >= (int)_tiles.size()) return;
        _tiles[tile].inUse = true;
        _tiles[tile].pinned = false;
        _tiles[tile].lastUsedFrame = frame;
        _tiles[tile].occupant = key;
    }

    void VirtualShadowTilePool::touch(int tile, std::uint64_t frame)
    {
        if (tile >= 0 && tile < (int)_tiles.size())
            _tiles[tile].lastUsedFrame = frame;
    }

    void VirtualShadowTilePool::setPinned(int tile, bool pinned)
    {
        if (tile >= 0 && tile < (int)_tiles.size())
            _tiles[tile].pinned = pinned;
    }

    void VirtualShadowTilePool::release(int tile)
    {
        if (tile < 0 || tile >= (int)_tiles.size()) return;
        _tiles[tile] = TileInfo();
    }

    int VirtualShadowTilePool::getTileX(int tile) const
    {
        return (tile >= 0 && tile < (int)_tiles.size()) ? (tile % _tilingCount) : 0;
    }

    int VirtualShadowTilePool::getTileY(int tile) const
    {
        return (tile >= 0 && tile < (int)_tiles.size()) ? (tile / _tilingCount) : 0;
    }

    VirtualShadowPageKey VirtualShadowTilePool::getOccupant(int tile) const
    {
        if (tile < 0 || tile >= (int)_tiles.size()) return VirtualShadowPageKey();
        return _tiles[tile].occupant;
    }

    int VirtualShadowTilePool::getUsedCount() const
    {
        int count = 0;
        for (int i = 0; i < _tileCount; ++i)
            if (_tiles[i].inUse) ++count;
        return count;
    }

    int VirtualShadowTilePool::getPinnedCount() const
    {
        int count = 0;
        for (int i = 0; i < _tileCount; ++i)
            if (_tiles[i].inUse && _tiles[i].pinned) ++count;
        return count;
    }
}
