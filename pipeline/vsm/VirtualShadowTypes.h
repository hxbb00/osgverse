#ifndef OSGVERSE_VIRTUAL_SHADOW_TYPES_H
#define OSGVERSE_VIRTUAL_SHADOW_TYPES_H

#include <cstdint>

namespace osgVerse
{
    /** Global settings of the virtual shadow map infrastructure. The defaults follow the
        first implementation milestone: desktop GL3, 512px physical tiles, 64 tiles in the
        atlas and a 64x64 virtual page table per light. */
    struct VirtualShadowSettings
    {
        int pageTableSize;             // finest page grid dimension, must be a power of two
        int maxLevels;                 // 0 (finest) .. maxLevels-1 (coarsest); clamped to log2+1
        int tileResolution;            // pixels of one physical tile
        int maxTiles;                  // number of tiles in the physical atlas
        int maxPageRequestsPerFrame;   // page render budget
        int maxLights;                 // shadow-casting lights sharing the atlas
        int pagePadding;               // padding texels around a page, avoids atlas bleeding
        float lodBias;                 // >1 selects finer pages, <1 selects coarser ones
        bool enablePCSS;               // soft shadows in the sampling shader

        VirtualShadowSettings()
        :   pageTableSize(64), maxLevels(7), tileResolution(512), maxTiles(64),
            maxPageRequestsPerFrame(4), maxLights(8), pagePadding(2),
            lodBias(1.0f), enablePCSS(true)
        {}
    };

    /** A page is identified in light space, independent of the camera, so that a page can
        stay resident while the camera moves. 'light' selects one of the shadow-casting
        lights, 'face' is reserved for point-light cube faces (0 otherwise). Level 0 is the
        finest level; page coordinates are (0, 0) at the region minimum. */
    struct VirtualShadowPageKey
    {
        int light;
        int face;
        int level;
        int x;
        int y;

        VirtualShadowPageKey() : light(-1), face(0), level(0), x(0), y(0) {}
        VirtualShadowPageKey(int l, int f, int lv, int px, int py)
            : light(l), face(f), level(lv), x(px), y(py) {}

        bool operator==(const VirtualShadowPageKey& rhs) const
        {
            return light == rhs.light && face == rhs.face && level == rhs.level &&
                   x == rhs.x && y == rhs.y;
        }

        bool operator!=(const VirtualShadowPageKey& rhs) const { return !(*this == rhs); }

        bool operator<(const VirtualShadowPageKey& rhs) const
        {
            if (light != rhs.light) return light < rhs.light;
            if (face != rhs.face) return face < rhs.face;
            if (level != rhs.level) return level < rhs.level;
            if (x != rhs.x) return x < rhs.x;
            return y < rhs.y;
        }
    };

    /** Pack a key into 64 bits so that hash containers and schedulers can use a single
        integer. Limits: light < 256, face < 8, level < 32, page coordinates < 2^24. */
    inline std::uint64_t packVirtualShadowPageKey(const VirtualShadowPageKey& key)
    {
        std::uint64_t value = static_cast<std::uint64_t>(key.light & 0xFF);
        value |= static_cast<std::uint64_t>(key.face & 0x7) << 8;
        value |= static_cast<std::uint64_t>(key.level & 0x1F) << 11;
        value |= static_cast<std::uint64_t>(key.x & 0xFFFFFF) << 16;
        value |= static_cast<std::uint64_t>(key.y & 0xFFFFFF) << 40;
        return value;
    }

    inline VirtualShadowPageKey unpackVirtualShadowPageKey(std::uint64_t value)
    {
        VirtualShadowPageKey key;
        key.light = static_cast<int>(value & 0xFF);
        key.face = static_cast<int>((value >> 8) & 0x7);
        key.level = static_cast<int>((value >> 11) & 0x1F);
        key.x = static_cast<int>((value >> 16) & 0xFFFFFF);
        key.y = static_cast<int>((value >> 40) & 0xFFFFFF);
        return key;
    }

    enum class VirtualShadowPageState : std::uint8_t
    {
        Unmapped = 0,   // not requested, or evicted
        Requested,      // queued for rendering
        Resident        // has a physical tile and can be sampled
    };

    /** CPU-side state of one page. The physical matrix is owned by the page renderer and
        uploaded to the per-tile matrix buffer separately. */
    struct VirtualShadowPageRecord
    {
        VirtualShadowPageKey key;
        VirtualShadowPageState state;
        int tile;                    // physical tile slot, -1 if not resident
        std::uint64_t lastUsedFrame; // for LRU and selection
        std::uint64_t signature;     // caster/light signature used for static page caching
        bool dirty;                  // page table indirection needs a redraw

        VirtualShadowPageRecord()
        :   state(VirtualShadowPageState::Unmapped), tile(-1),
            lastUsedFrame(0), signature(0), dirty(false)
        {}

        VirtualShadowPageRecord(const VirtualShadowPageKey& k, std::uint64_t frame)
        :   key(k), state(VirtualShadowPageState::Unmapped), tile(-1),
            lastUsedFrame(frame), signature(0), dirty(false)
        {}
    };

    struct VirtualShadowStats
    {
        int residentPages;
        int pendingRequests;
        int updatedTiles;
        int evictedTiles;
        int cacheHits;
        int cacheMisses;

        VirtualShadowStats()
        :   residentPages(0), pendingRequests(0), updatedTiles(0),
            evictedTiles(0), cacheHits(0), cacheMisses(0)
        {}

        void resetPerFrame()
        {
            updatedTiles = 0; evictedTiles = 0; cacheHits = 0; cacheMisses = 0;
        }
    };
}

#endif
