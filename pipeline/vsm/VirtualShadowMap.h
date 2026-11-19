#ifndef OSGVERSE_VIRTUAL_SHADOW_MAP_H
#define OSGVERSE_VIRTUAL_SHADOW_MAP_H

#include "VirtualShadowPageTable.h"
#include "VirtualShadowTilePool.h"
#include "VirtualShadowScheduler.h"
#include <osg/Referenced>
#include <osg/Image>
#include <osg/Matrix>
#include <osg/StateSet>
#include <osg/Texture2D>
#include <osg/Texture2DArray>
#include <osg/TextureBuffer>
#include <osg/Uniform>
#include <vector>

namespace osgVerse
{
    /** Resource manager of the virtual shadow map infrastructure.

        It owns the virtual page tables (one per shadow-casting light/face), the shared
        physical tile atlas, the per-tile world-to-shadow matrix buffer and the page table
        texture array. It deliberately does not render pages: the page renderer fills a
        physical tile and calls commitPage() with the page matrix, which keeps the
        infrastructure usable by the standalone test before it is merged into ShadowModule.

        Data layout expected by the sampling shader:
        - IndirectionTexture: sampler2DArray, one layer per light/face, RGBA32F. A texel
          stores (tileX, tileY, level, valid). Level 0 is the finest level, and a page at
          level L occupies 2^L x 2^L texels of the layer.
        - ShadowAtlas: sampler2D, R32F normalized light-space depth, tileResolution per tile.
        - PageMatrices: samplerBuffer, 4 RGBA32F texels per tile slot (the four columns of
          the world-to-shadow matrix, in GLSL column order). */
    class VirtualShadowMap : public osg::Referenced
    {
    public:
        VirtualShadowMap();

        /** Create or recreate all CPU/GPU resources. 'numLights' is the number of
            shadow-casting lights that can be given a page table layer; each light uses one
            face for now (point lights will use six faces later). */
        void configure(const VirtualShadowSettings& settings, int numLights);
        const VirtualShadowSettings& getSettings() const { return _settings; }
        int getNumLights() const { return _numLights; }

        VirtualShadowPageTable* getPageTable(int light, int face = 0);
        const VirtualShadowPageTable* getPageTable(int light, int face = 0) const;
        VirtualShadowTilePool* getTilePool() { return _tilePool.get(); }
        VirtualShadowScheduler* getScheduler() { return _scheduler.get(); }
        const VirtualShadowStats& getStats() const { return _stats; }

        osg::Texture2D* getAtlasTexture() { return _atlasTexture.get(); }
        osg::Texture2DArray* getIndirectionTexture() { return _indirectionTexture.get(); }
        osg::TextureBuffer* getTileMatrixBuffer() { return _tileMatrixBuffer.get(); }

        /** Start a frame: clear per-frame stats and unpin every tile. */
        void beginFrame(std::uint64_t frame);

        /** Mark a page as wanted and queue it for rendering. Returns false when it is
            already resident or already queued. */
        bool requestPage(const VirtualShadowPageKey& key, float priority);

        /** Mark a resident page as used by the current frame, touching its tile so that it
            is not evicted before the frame ends, and pinning it. */
        void touchPage(const VirtualShadowPageKey& key);

        /** Store the rendered page into a physical tile and write its world-to-shadow
            matrix. If the page is already resident, only the matrix is refreshed. Returns
            the tile slot, or -1 when no tile could be allocated (all pinned). */
        int commitPage(const VirtualShadowPageKey& key, const osg::Matrix& worldToShadow);

        /** Drop a page and release its tile. */
        void releasePage(const VirtualShadowPageKey& key);

        /** Finish a frame: update stats and unpin tiles that were not touched. */
        void endFrame();

        /** Rebuild the dirty page table layers and upload the changed matrices. It is safe
            to call this once per frame before the sampling stages are drawn. */
        void updateGpuData();

        /** Bind the indirection texture array, the atlas and the page matrix buffer to the
            given state set, and set the parameters the sampling shader expects. */
        void applyToStateSet(osg::StateSet* ss, int startUnit = 8);

        /** Whether this build/platform can use the virtual shadow map path. It needs
            samplerBuffer / floating point textures and is therefore desktop GL3/GLES3
            only; GLES2/WebGL1 falls back to the cascade path. */
        static bool isSupported(int glVersion, int glslVersion);

    protected:
        virtual ~VirtualShadowMap();
        void createTextures();
        void rebuildIndirectionLayer(int layer);
        void updateMatrix(int tile, const osg::Matrix& worldToShadow);
        int allocateTile(const VirtualShadowPageKey& key);
        void evictTile(int tile);

        VirtualShadowSettings _settings;
        int _numLights, _numFaces;
        std::uint64_t _frame;
        bool _matrixDirty;

        std::vector<osg::ref_ptr<VirtualShadowPageTable>> _pageTables;
        osg::ref_ptr<VirtualShadowTilePool> _tilePool;
        osg::ref_ptr<VirtualShadowScheduler> _scheduler;
        VirtualShadowStats _stats;

        osg::ref_ptr<osg::Texture2D> _atlasTexture;
        osg::ref_ptr<osg::Texture2DArray> _indirectionTexture;
        osg::ref_ptr<osg::TextureBuffer> _tileMatrixBuffer;
        std::vector<osg::ref_ptr<osg::Image>> _indirectionImages;
        osg::ref_ptr<osg::Image> _matrixImage;
        std::vector<osg::ref_ptr<osg::Uniform>> _uniforms;
    };
}

#endif
