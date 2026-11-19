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
//
// The page matrix itself is not uploaded: a page's local coordinates and depth are fully
// determined by the light-space position, the fixed light region, the page level (which gives
// the grid size) and the shared light near/far planes. Deriving them here avoids a per-tile
// matrix lookup and keeps the sampling exactly consistent with the page caster, which
// rasterises with scaleBias * projection * worldToLight.

uniform sampler2DArray VsmIndirectionTexture;
uniform sampler2D VsmShadowAtlas;

// x = page table size, y = 1 / page table size, z = number of levels, w = unused
uniform vec4 VsmPageParams;

// x = tile resolution, y = tile count per row, z = atlas resolution, w = max tiles
uniform vec4 VsmAtlasParams;

// x = near plane, y = far plane, z = 1 / (far - near), w = unused (light view-space z)
uniform vec4 VsmLightDepthRange;

// x = softness, y = softness near, z = softness far, w = PCSS enabled
uniform vec4 VsmPCSSParams;

uniform int VsmLightCount;

float vsmVFetchDepth(vec2 uv)
{
    return VERSE_TEX2D(VsmShadowAtlas, uv).r;
}

// Fetch the page record covering this region UV. The lookup is point-sampled at the page
// center, so all pixels of one page see the same record.
vec4 vsmFetchPage(float layer, vec2 uv)
{
    float pageSize = VsmPageParams.x;
    vec2 uvInt = uv - fract(uv * pageSize) / pageSize;
    return VERSE_TEX2DARRAY(VsmIndirectionTexture, vec3(uvInt, layer));
}

// Page-local [0, 1] UV to atlas UV
vec2 vsmPageToAtlasUV(vec4 page, vec2 pageUV)
{
    float tileSize = VsmAtlasParams.x, atlasSize = VsmAtlasParams.z;
    return (page.rg * tileSize + pageUV * tileSize) / atlasSize;
}

// Atlas rectangle of one tile, used to clamp PCF taps so they can not read a neighbouring page
void vsmGetTileRect(vec4 page, out vec2 uvMin, out vec2 uvMax)
{
    float tileSize = VsmAtlasParams.x, atlasSize = VsmAtlasParams.z;
    uvMin = (page.rg * tileSize) / atlasSize;
    uvMax = (page.rg + 1.0) * tileSize / atlasSize;
}

float vsmCompareDepth(float receiverDepth, float casterDepth, float bias)
{
    return (casterDepth >= receiverDepth - bias) ? 1.0 : 0.0;
}

// 3x3 PCF inside one page. The page depth range is tight, so the comparison needs only a
// small receiver-side bias.
float vsmSamplePCF3x3(vec4 page, vec2 pageUV, float depth, float bias)
{
    vec2 uvMin, uvMax;
    vsmGetTileRect(page, uvMin, uvMax);

    float texel = 1.0 / VsmAtlasParams.z;
    vec2 baseUV = vsmPageToAtlasUV(page, pageUV);
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec2 uv = clamp(baseUV + vec2(float(x), float(y)) * texel, uvMin, uvMax);
            sum += vsmCompareDepth(depth, vsmVFetchDepth(uv), bias);
        }
    }
    return sum / 9.0;
}

// Hard shadow lookup: the caller rebuilds the world position and the world-to-light matrix,
// and selects the light layer.
float vsmSampleShadow(int layer, vec3 worldPos, mat4 worldToLight, vec4 region,
                      float bias)
{
    vec3 local = (worldToLight * vec4(worldPos, 1.0)).xyz;
    vec2 uv = (local.xy - region.xy) * region.zw;  // region UV across the whole light area
    vec4 page = vsmFetchPage(float(layer), uv);
    if (page.w < 0.5) return 1.0;  // not resident yet: leave the surface lit

    // Pages of one level tile the region uniformly, so the page-local UV is the fractional
    // part of the region UV scaled by the grid size
    float grid = VsmPageParams.x / exp2(page.z);
    vec2 pageUV = fract(uv * grid);
    float depth = (-local.z - VsmLightDepthRange.x) * VsmLightDepthRange.z;
    return vsmSamplePCF3x3(page, pageUV, depth, bias);
}

// PCSS will be added together with the sampling integration (P3/P4). The hard PCF path above
// is intentionally complete and cheap, which is what the first standalone test needs.
