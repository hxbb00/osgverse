#include "VirtualShadowMap.h"
#include "../Pipeline.h"
#include "../Utilities.h"
#include <cstring>

#ifndef GL_R32F
#   define GL_R32F 0x822E
#endif

namespace osgVerse
{
    VirtualShadowMap::VirtualShadowMap()
    :   _numLights(0), _numFaces(1), _frame(0), _matrixDirty(false)
    {}

    VirtualShadowMap::~VirtualShadowMap()
    {}

    bool VirtualShadowMap::isSupported(int glVersion, int glslVersion)
    {
        // The page table texture array and the per-tile matrix buffer rely on floating point
        // textures and samplerBuffer. Desktop GL3 (and GLES3.1+) provides both; GLES2/WebGL1
        // keeps using the cascade shadow path
        return glVersion >= 310 && isTextureBufferSupported(glVersion, glslVersion);
    }

    void VirtualShadowMap::configure(const VirtualShadowSettings& settings, int numLights)
    {
        _settings = settings;
        if (_settings.pageTableSize < 2) _settings.pageTableSize = 2;
        if (_settings.maxLevels < 1) _settings.maxLevels = 1;
        if (_settings.tileResolution < 1) _settings.tileResolution = 1;
        if (_settings.maxTiles < 1) _settings.maxTiles = 1;
        if (_settings.maxPageRequestsPerFrame < 1) _settings.maxPageRequestsPerFrame = 1;
        if (_settings.maxLights < 1) _settings.maxLights = 1;

        _numLights = (numLights > 0) ? numLights : 1;
        _numLights = osg::minimum(_numLights, _settings.maxLights);
        _numFaces = 1;  // point-light cube faces will be added later
        _frame = 0; _matrixDirty = false;

        _tilePool = new VirtualShadowTilePool(
            _settings.maxTiles, _settings.tileResolution);
        _scheduler = new VirtualShadowScheduler;
        _scheduler->configure(_settings.maxPageRequestsPerFrame);

        _pageTables.clear();
        for (int light = 0; light < _numLights; ++light)
        {
            osg::ref_ptr<VirtualShadowPageTable> table = new VirtualShadowPageTable(
                light, 0, _settings.pageTableSize, _settings.maxLevels);
            _pageTables.push_back(table);
        }

        _stats = VirtualShadowStats();
        createTextures();
    }

    VirtualShadowPageTable* VirtualShadowMap::getPageTable(int light, int face)
    {
        if (light < 0 || light >= _numLights || face < 0 || face >= _numFaces) return NULL;
        int index = light * _numFaces + face;
        return (index < (int)_pageTables.size()) ? _pageTables[index].get() : NULL;
    }

    const VirtualShadowPageTable* VirtualShadowMap::getPageTable(int light, int face) const
    {
        return const_cast<VirtualShadowMap*>(this)->getPageTable(light, face);
    }

    void VirtualShadowMap::createTextures()
    {
        int pageSize = _settings.pageTableSize;
        int tileRes = _tilePool->getTileResolution();

        // One layer per light/face: (tileX, tileY, level, valid), nearest filtering
        _indirectionImages.clear();
        _indirectionTexture = new osg::Texture2DArray;
        _indirectionTexture->setTextureSize(pageSize, pageSize, _numLights * _numFaces);
        _indirectionTexture->setInternalFormat(GL_RGBA32F_ARB);
        _indirectionTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
        _indirectionTexture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
        _indirectionTexture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        _indirectionTexture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);

        for (int layer = 0; layer < _numLights * _numFaces; ++layer)
        {
            osg::ref_ptr<osg::Image> image = new osg::Image;
            image->allocateImage(pageSize, pageSize, 1, GL_RGBA, GL_FLOAT);
            image->setInternalTextureFormat(GL_RGBA32F_ARB);
            image->setOrigin(osg::Image::BOTTOM_LEFT);
            std::memset(image->data(), 0, image->getTotalSizeInBytes());
            _indirectionTexture->setImage(layer, image.get());
            _indirectionImages.push_back(image);
        }
        _indirectionTexture->dirtyTextureObject();

        // Physical atlas: one normalized light-space depth value per texel. Pages are
        // rendered directly into their tile rectangle by the page renderer
        _atlasTexture = new osg::Texture2D;
        _atlasTexture->setTextureSize(_tilePool->getAtlasResolution(),
                                      _tilePool->getAtlasResolution());
        _atlasTexture->setInternalFormat(GL_R32F);
        _atlasTexture->setSourceFormat(GL_RED);
        _atlasTexture->setSourceType(GL_FLOAT);
        _atlasTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
        _atlasTexture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
        _atlasTexture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_BORDER);
        _atlasTexture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_BORDER);
        _atlasTexture->setBorderColor(osg::Vec4(1.0f, 0.0f, 0.0f, 0.0f));

        // Per-tile world-to-shadow matrices, four RGBA32F texels per tile (columns)
        _matrixImage = new osg::Image;
        _matrixImage->allocateImage(_settings.maxTiles * 4, 1, 1, GL_RGBA, GL_FLOAT);
        _matrixImage->setInternalTextureFormat(GL_RGBA32F_ARB);
        std::memset(_matrixImage->data(), 0, _matrixImage->getTotalSizeInBytes());

        _tileMatrixBuffer = new osg::TextureBuffer;
        _tileMatrixBuffer->setImage(_matrixImage.get());
        _tileMatrixBuffer->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
        _tileMatrixBuffer->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);

        _uniforms.clear();
        _uniforms.push_back(new osg::Uniform("VsmPageParams", osg::Vec4(
            (float)pageSize, 1.0f / (float)pageSize, (float)_settings.maxLevels, 0.0f)));
        _uniforms.push_back(new osg::Uniform("VsmAtlasParams", osg::Vec4(
            (float)tileRes, (float)_tilePool->getTilingCount(),
            (float)_tilePool->getAtlasResolution(), (float)_settings.maxTiles)));
        _uniforms.push_back(new osg::Uniform("VsmLightCount", _numLights));
        _uniforms.push_back(new osg::Uniform("VsmPCSSParams", osg::Vec4(
            0.5f, 0.2f, 1.0f, _settings.enablePCSS ? 1.0f : 0.0f)));
    }

    void VirtualShadowMap::beginFrame(std::uint64_t frame)
    {
        _frame = frame;
        _stats.resetPerFrame();
        for (int i = 0; i < _tilePool->getTileCount(); ++i)
            _tilePool->setPinned(i, false);
    }

    void VirtualShadowMap::endFrame()
    {
        _stats.pendingRequests = _scheduler->getPendingCount();
        _stats.residentPages = 0;
        for (size_t i = 0; i < _pageTables.size(); ++i)
            _stats.residentPages += _pageTables[i]->getResidentCount();

        for (int i = 0; i < _tilePool->getTileCount(); ++i)
            _tilePool->setPinned(i, false);
    }

    bool VirtualShadowMap::requestPage(const VirtualShadowPageKey& key, float priority)
    {
        VirtualShadowPageTable* table = getPageTable(key.light, key.face);
        if (table == NULL || !table->isValidKey(key)) return false;
        if (table->isResident(key)) { touchPage(key); return false; }
        if (!table->markRequested(key, _frame)) return false;
        return _scheduler->submit(key, priority, _frame);
    }

    void VirtualShadowMap::touchPage(const VirtualShadowPageKey& key)
    {
        VirtualShadowPageTable* table = getPageTable(key.light, key.face);
        if (table == NULL) return;

        table->touch(key, _frame);
        const VirtualShadowPageRecord* record = table->find(key);
        if (record != NULL && record->state == VirtualShadowPageState::Resident &&
            record->tile >= 0)
        {
            _tilePool->touch(record->tile, _frame);
            _tilePool->setPinned(record->tile, true);
            _stats.cacheHits++;
        }
    }

    int VirtualShadowMap::allocateTile(const VirtualShadowPageKey& key)
    {
        int tile = _tilePool->findFreeTile();
        if (tile >= 0) return tile;

        tile = _tilePool->findEvictableTile();
        if (tile < 0) return -1;

        // The previous occupant must lose its mapping before the tile is reused, otherwise
        // the page table would keep pointing at a tile that now holds another page
        VirtualShadowPageKey occupant = _tilePool->getOccupant(tile);
        VirtualShadowPageTable* occupantTable = getPageTable(occupant.light, occupant.face);
        if (occupantTable != NULL) occupantTable->markUnmapped(occupant, false);

        _tilePool->release(tile);
        _stats.evictedTiles++;
        return tile;
    }

    int VirtualShadowMap::commitPage(const VirtualShadowPageKey& key,
                                     const osg::Matrix& worldToShadow)
    {
        VirtualShadowPageTable* table = getPageTable(key.light, key.face);
        if (table == NULL || !table->isValidKey(key)) return -1;

        const VirtualShadowPageRecord* record = table->find(key);
        if (record != NULL && record->state == VirtualShadowPageState::Resident &&
            record->tile >= 0)
        {
            updateMatrix(record->tile, worldToShadow);
            table->touch(key, _frame);
            _tilePool->touch(record->tile, _frame);
            _tilePool->setPinned(record->tile, true);
            return record->tile;
        }

        int tile = allocateTile(key);
        if (tile < 0) return -1;  // every tile is pinned; retry on the next frame

        _tilePool->markUsed(tile, key, _frame);
        _tilePool->setPinned(tile, true);
        table->markResident(key, tile, _frame);
        updateMatrix(tile, worldToShadow);
        _stats.updatedTiles++; _stats.cacheMisses++;
        return tile;
    }

    void VirtualShadowMap::releasePage(const VirtualShadowPageKey& key)
    {
        VirtualShadowPageTable* table = getPageTable(key.light, key.face);
        if (table == NULL) return;

        const VirtualShadowPageRecord* record = table->find(key);
        if (record != NULL && record->tile >= 0)
        {
            _tilePool->release(record->tile);
            _stats.evictedTiles++;
        }
        table->markUnmapped(key, false);
    }

    void VirtualShadowMap::updateMatrix(int tile, const osg::Matrix& worldToShadow)
    {
        if (tile < 0 || tile >= _settings.maxTiles || !_matrixImage.valid()) return;

        // Store the four *columns* of the matrix, because the shader rebuilds it with
        // mat4(column0, column1, column2, column3)
        float* data = reinterpret_cast<float*>(_matrixImage->data()) + tile * 16;
        for (int c = 0; c < 4; ++c)
        {
            for (int r = 0; r < 4; ++r)
                data[c * 4 + r] = (float)worldToShadow(r, c);
        }
        _matrixDirty = true;
    }

    void VirtualShadowMap::updateGpuData()
    {
        for (int layer = 0; layer < (int)_pageTables.size(); ++layer)
        {
            if (_pageTables[layer]->consumeDirty()) rebuildIndirectionLayer(layer);
        }

        if (_matrixDirty && _matrixImage.valid())
        {
            _matrixImage->dirty();
            _matrixDirty = false;
        }
    }

    void VirtualShadowMap::rebuildIndirectionLayer(int layer)
    {
        if (layer < 0 || layer >= (int)_pageTables.size() ||
            layer >= (int)_indirectionImages.size()) return;

        VirtualShadowPageTable* table = _pageTables[layer].get();
        osg::Image* image = _indirectionImages[layer].get();
        int pageSize = table->getPageTableSize();
        if (image == NULL || image->s() != pageSize || image->t() != pageSize) return;

        // Clear the layer, then stamp every resident page as a rectangle. Coarse pages are
        // larger rectangles and fine pages overwrite them where both are active, which gives
        // a natural fallback: a missing fine page keeps the coarse page underneath
        std::memset(image->data(), 0, image->getTotalSizeInBytes());
        float* pixels = reinterpret_cast<float*>(image->data());

        std::vector<VirtualShadowPageKey> pages;
        table->getResidentPages(pages);
        for (size_t i = 0; i < pages.size(); ++i)
        {
            const VirtualShadowPageKey& key = pages[i];
            const VirtualShadowPageRecord* record = table->find(key);
            if (record == NULL || record->tile < 0) continue;

            int cell = 1 << key.level;
            int x0 = key.x * cell, y0 = key.y * cell;
            int x1 = osg::minimum(x0 + cell, pageSize);
            int y1 = osg::minimum(y0 + cell, pageSize);
            x0 = osg::maximum(x0, 0); y0 = osg::maximum(y0, 0);

            float tileX = (float)_tilePool->getTileX(record->tile);
            float tileY = (float)_tilePool->getTileY(record->tile);
            for (int y = y0; y < y1; ++y)
            {
                float* row = pixels + (y * pageSize + x0) * 4;
                for (int x = x0; x < x1; ++x)
                {
                    *row++ = tileX; *row++ = tileY;
                    *row++ = (float)key.level; *row++ = 1.0f;
                }
            }
        }

        image->dirty();
        _indirectionTexture->setImage(layer, image);
        _indirectionTexture->dirtyTextureObject();
    }

    void VirtualShadowMap::applyToStateSet(osg::StateSet* ss, int startUnit)
    {
        if (ss == NULL) return;

        int unit = startUnit;
        applyTextureAttributeAndModes(ss, unit, _indirectionTexture.get());
        applyTextureAttributeAndModes(ss, unit + 1, _atlasTexture.get());
        applyTextureAttributeAndModes(ss, unit + 2, _tileMatrixBuffer.get());

        osg::Uniform* indirectionU = ss->getUniform("VsmIndirectionTexture");
        if (indirectionU == NULL)
        {
            indirectionU = new osg::Uniform("VsmIndirectionTexture", unit);
            ss->addUniform(indirectionU);
        }
        else indirectionU->set(unit);

        osg::Uniform* atlasU = ss->getUniform("VsmShadowAtlas");
        if (atlasU == NULL)
        {
            atlasU = new osg::Uniform("VsmShadowAtlas", unit + 1);
            ss->addUniform(atlasU);
        }
        else atlasU->set(unit + 1);

        osg::Uniform* matrixU = ss->getUniform("VsmPageMatrices");
        if (matrixU == NULL)
        {
            matrixU = new osg::Uniform("VsmPageMatrices", unit + 2);
            ss->addUniform(matrixU);
        }
        else matrixU->set(unit + 2);

        for (size_t i = 0; i < _uniforms.size(); ++i)
        {
            osg::Uniform* uniform = _uniforms[i].get();
            if (ss->getUniform(uniform->getName()) == NULL)
                ss->addUniform(uniform);
        }
    }
}
