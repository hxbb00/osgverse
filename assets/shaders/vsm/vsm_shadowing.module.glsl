// Virtual shadow map sampling helpers.
//
// This module is included by the lighting/combine shaders when virtual shadows are enabled.
// It deliberately takes the light-space region and the world-to-light matrix as parameters
// instead of relying on global names, so the same code works for the main light and for the
// per-light loop of the clustered lighting shader.
//
// Texture layout (see VirtualShadowMap.h):
//   VsmIndirectionTexture : sampler2DArray, layer = light index / face,
//                           texel = (tileX, tileY, level, valid)
//   VsmShadowAtlas       : sampler2D, R32F normalized light-space depth, tile per page
//   VsmPageMatrices      : samplerBuffer, 4 RGBA32F texels (matrix columns) per tile slot

uniform sampler2DArray VsmIndirectionTexture;
uniform sampler2D VsmShadowAtlas;
uniform samplerBuffer VsmPageMatrices;

// x = page table size, y = 1 / page table size, z = number of levels, w = unused
uniform vec4 VsmPageParams;

// x = tile resolution, y = tile count per row, z = atlas resolution, w = max tiles
uniform vec4 VsmAtlasParams;

// x = softness, y = softness near, z = softness far, w = PCSS enabled
uniform vec4 VsmPCSSParams;

uniform int VsmLightCount;

float vsmVFetchDepth(vec2 uv)
{
    return VERSE_TEX2D(VsmShadowAtlas, uv).r;
}

// Fetch the page record covering this virtual UV. The lookup is point-sampled at the page
// center, so all pixels of one page see the same record.
vec4 vsmFetchPage(float layer, vec2 uv)
{
    float pageSize = VsmPageParams.x;
    vec2 uvInt = uv - fract(uv * pageSize) / pageSize;
    return VERSE_TEX2DARRAY(VsmIndirectionTexture, vec3(uvInt, layer));
}

mat4 vsmGetPageMatrix(int tile)
{
    int base = tile * 4;
    vec4 c0 = texelFetch(VsmPageMatrices, base + 0);
    vec4 c1 = texelFetch(VsmPageMatrices, base + 1);
    vec4 c2 = texelFetch(VsmPageMatrices, base + 2);
    vec4 c3 = texelFetch(VsmPageMatrices, base + 3);
    return mat4(c0, c1, c2, c3);
}

int vsmGetTileSlot(vec4 page)
{
    return int(page.y) * int(VsmAtlasParams.y) + int(page.x);
}

// Page-local [0, 1] UV to atlas UV, and the atlas rectangle of one tile, used to clamp PCF
// taps so they can not read a neighbouring page
vec2 vsmPageToAtlasUV(vec4 page, vec2 pageUV)
{
    float tileSize = VsmAtlasParams.x, atlasSize = VsmAtlasParams.z;
    return (page.rg * tileSize + pageUV * tileSize) / atlasSize;
}

void vsmGetTileRect(vec4 page, out vec2 uvMin, out vec2 uvMax)
{
    float tileSize = VsmAtlasParams.x, atlasSize = VsmAtlasParams.z;
    uvMin = page.rg * tileSize / atlasSize;
    uvMax = (page.rg + 1.0) * tileSize / atlasSize;
}

// Convert a world position into the virtual page table UV of one light. 'region' is
// (minX, minY, 1 / sizeX, 1 / sizeY) in light space, the same region the page renderer used.
vec2 vsmWorldToRegionUV(vec3 worldPos, mat4 worldToLight, vec4 region)
{
    vec3 localPos = (worldToLight * vec4(worldPos, 1.0)).xyz;
    return (localPos.xy - region.xy) * region.zw;
}

float vsmCompareDepth(float receiverDepth, float casterDepth, float bias)
{
    return (casterDepth >= receiverDepth - bias) ? 1.0 : 0.0;
}

// 3x3 PCF inside one page. The page depth range is tight, so the comparison needs only a
// small receiver-side bias; the caller computes it from the page's texel depth delta.
float vsmSamplePCF3x3(vec4 page, vec3 shadowCoord, float bias)
{
    vec2 uvMin, uvMax;
    vsmGetTileRect(page, uvMin, uvMax);

    float texel = 1.0 / VsmAtlasParams.z;
    vec2 baseUV = vsmPageToAtlasUV(page, shadowCoord.xy);
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec2 uv = clamp(baseUV + vec2(float(x), float(y)) * texel, uvMin, uvMax);
            sum += vsmCompareDepth(shadowCoord.z, vsmVFetchDepth(uv), bias);
        }
    }
    return sum / 9.0;
}

// Hard shadow lookup: the caller has already rebuilt the world position and the
// world-to-light matrix, and selected the light layer.
float vsmSampleShadow(int layer, vec3 worldPos, mat4 worldToLight, vec4 region,
                      float bias)
{
    vec2 uv = vsmWorldToRegionUV(worldPos, worldToLight, region);
    vec4 page = vsmFetchPage(float(layer), uv);
    if (page.w < 0.5) return 1.0;  // not resident yet: leave the surface lit

    int tile = vsmGetTileSlot(page);
    vec3 shadowCoord = (vsmGetPageMatrix(tile) * vec4(worldPos, 1.0)).xyz;
    return vsmSamplePCF3x3(page, shadowCoord, bias);
}

// PCSS will be added together with the sampling integration (P3/P4). The hard PCF path above
// is intentionally complete and cheap, which is what the first standalone test needs.

