#include "../basic_lighting.module.glsl"

#define M_PI 3.1415926535897932384626433832795
uniform sampler2D DiffuseMap, NormalMap, SpecularMap, ShininessMap;
uniform sampler2D AmbientMap, EmissiveMap, ReflectionMap;
#ifdef VERSE_LIGHT_BUFFER
// Texture buffer object (TBO) copy of the light parameter table: element (id, row) is stored at
// the flat index 'id + 1024 * row' (see LightModule, which uploads both forms)
uniform samplerBuffer LightParameterBuffer;
#else
uniform sampler2D LightParameterMap;  // (r0: col+type, r1: pos+att1, r2: dir+att0, r3: spotProp)
#endif
uniform vec2 LightNumber;  // (num, max_num)
#ifdef VERSE_LIGHT_CLUSTER
// Same clustered light culling as the deferred lighting pass (std_pbr_lighting.frag.glsl):
// the CPU side (LightCluster) bins the lights into a 2D tile x exponential depth slice grid and
// the fragment only walks the lights of the cluster it falls in. The light positions stored in
// the parameter table are eye-space, and so is eyeVertex below, so the same assignment applies
uniform samplerBuffer LightIndexBuffer;    // light indices of each cluster, in a flat list
uniform samplerBuffer LightClusterBuffer;  // (offset, count) of the index list of each cluster
uniform vec3 LightClusterGrid;             // grid size: (tilesX, tilesY, slices)
uniform vec2 LightClusterTile;             // size of one screen tile, in pixels
uniform vec2 LightClusterDepth;            // (near, far) range the slices are distributed over
uniform int LightClusterEnabled;           // 1: loop over the lights of the current cluster only
#endif
VERSE_FS_IN vec4 texCoord0, texCoord1, color, eyeVertex;
VERSE_FS_IN vec3 eyeNormal, eyeTangent, eyeBinormal;
VERSE_FS_OUT vec4 fragData;

const vec2 invAtan = vec2(0.1591, 0.3183);
const int maxLights = 1024;
#ifdef VERSE_LIGHT_CLUSTER
const int maxClusterLights = 256;  // must match LightCluster::MaxLightsPerCluster
#endif

/// PBR functions
vec2 sphericalUV(vec3 v)
{
    vec2 uv = vec2(atan(v.z, v.x), asin(v.y));
    uv *= invAtan; uv += 0.5; return uv;
}

vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness)
{
    float val = 1.0 - cosTheta;
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * (val*val*val*val*val); //Faster than pow
}

vec3 fresnelSchlick(float cosTheta, vec3 F0)
{
    float val = 1.0 - cosTheta;
    return F0 + (1.0 - F0) * (val*val*val*val*val); //Faster than pow
}

int getLightAttributes(in int id, out vec3 color, out vec3 pos, out vec3 dir,
                       out float range, out float spotCutoff)
{
#ifdef VERSE_LIGHT_BUFFER
    vec4 attr0 = texelFetch(LightParameterBuffer, id + 1024 * 0); // color, type
    vec4 attr1 = texelFetch(LightParameterBuffer, id + 1024 * 1); // pos, att
    vec4 attr2 = texelFetch(LightParameterBuffer, id + 1024 * 2); // dir, spot
#else
    const vec2 halfP = vec2(0.5 / 1024.0, 0.5 / 4.0), step = vec2(1.0 / 1024.0, 1.0 / 4.0);
    float fid = float(id);
    vec4 attr0 = VERSE_TEX2D(LightParameterMap, halfP + vec2(fid * step.x, 0.0 * step.y)); // color, type
    vec4 attr1 = VERSE_TEX2D(LightParameterMap, halfP + vec2(fid * step.x, 1.0 * step.y)); // pos, att
    vec4 attr2 = VERSE_TEX2D(LightParameterMap, halfP + vec2(fid * step.x, 2.0 * step.y)); // dir, spot
#endif
    color = attr0.xyz; pos = attr1.xyz; dir = attr2.xyz; range = attr1.w;
    spotCutoff = attr2.w; return int(attr0.w);
}

#ifdef VERSE_LIGHT_CLUSTER
// Linear index of the cluster a fragment belongs to (see LightCluster::build(), which uses the
// same grid, the same exponential slices and the same near/far range on the CPU side)
int getClusterIndex(vec3 eyePosition)
{
    ivec2 gridXY = ivec2(LightClusterGrid.xy);
    ivec2 tile = ivec2(gl_FragCoord.xy / LightClusterTile);
    tile = clamp(tile, ivec2(0), gridXY - 1);
    float viewZ = max(-eyePosition.z, LightClusterDepth.x);
    float f = log(viewZ / LightClusterDepth.x) / log(LightClusterDepth.y / LightClusterDepth.x);
    int slices = int(LightClusterGrid.z);
    int slice = clamp(int(f * float(slices)), 0, slices - 1);
    return (slice * gridXY.y + tile.y) * gridXY.x + tile.x;
}
#endif

void main()
{
    vec2 uv0 = texCoord0.xy, uv1 = texCoord1.xy;
    vec4 diffuse = VERSE_TEX2D(DiffuseMap, uv0) * color;
    vec4 normalValue = VERSE_TEX2D(NormalMap, uv0);
    vec4 emission = VERSE_TEX2D(EmissiveMap, uv1);
    vec3 specular = VERSE_TEX2D(SpecularMap, uv0).rgb;
    vec3 metalRough = VERSE_TEX2D(ShininessMap, uv0).rgb;

    // Compute eye-space normal. Normal mapping needs the tangent frame built by the vertex
    // shader: a model without tangents gets zeros there, in which case the normal map is given up
    // and the geometry normal is used directly
    vec3 eyeNormal2 = eyeNormal;
    if (normalValue.a > 0.1 &&
        dot(eyeTangent, eyeTangent) > 1e-4 && dot(eyeBinormal, eyeBinormal) > 1e-4)
    {
        vec3 tsNormal = normalize(2.0 * normalValue.rgb - vec3(1.0));
        eyeNormal2 = normalize(mat3(eyeTangent, eyeBinormal, eyeNormal) * tsNormal);
    }

    // Components common to all light types
    vec3 viewDir = -normalize(eyeVertex.xyz / eyeVertex.w);
    vec3 R = reflect(-viewDir, eyeNormal2), albedo = diffuse.rgb;
    float metallic = metalRough.b, roughness = metalRough.g, ao = metalRough.r;
    float nDotV = max(dot(eyeNormal2, viewDir), 0.0);

    // Calculate reflectance at normal incidence; if dia-electric (like plastic) use F0 of 0.04;
    // if it's a metal, use the albedo color as F0 (metallic workflow)
    vec3 F0 = mix(vec3(0.04), albedo, metallic), radianceOut = vec3(0.0);

    // Compute lights: either the ones assigned to the cluster of this pixel (clustered light
    // culling, computed on the CPU by LightCluster), or the whole table when it is disabled
    vec3 lightColor, lightPos, lightDir; float lightRange = 0.0, lightSpot = 0.0;
    int numLights = int(min(LightNumber.x, LightNumber.y));
#ifdef VERSE_LIGHT_CLUSTER
    if (LightClusterEnabled == 1)
    {
        vec2 clusterRange = texelFetch(LightClusterBuffer, getClusterIndex(eyeVertex.xyz)).rg;
        int start = int(clusterRange.x), count = int(clusterRange.y);
        for (int i = 0; i < maxClusterLights; ++i)
        {
            if (i >= count) break;  // to avoid 'WebGL: Loop index cannot be compared with non-constant expression'
            int id = int(texelFetch(LightIndexBuffer, start + i).r);
            int type = getLightAttributes(id, lightColor, lightPos, lightDir, lightRange, lightSpot);
            if (type == 1)
            {
                radianceOut += get_directional_light_contribution(
                        viewDir, eyeVertex.xyz, lightPos, lightDir, lightColor, albedo, metallic,
                        roughness, eyeNormal, lightRange);
            }
            else if (type == 2)
            {
                radianceOut += get_point_light_contribution(
                        viewDir, eyeVertex.xyz, lightPos, lightColor, albedo, metallic, roughness,
                        eyeNormal, lightRange);
            }
            else if (type == 3)
            {
                radianceOut += get_spot_light_contribution(
                        viewDir, eyeVertex.xyz, lightPos, lightDir, lightColor, albedo, metallic,
                        roughness, eyeNormal, lightRange, lightSpot);
            }
        }
    }
    else
#endif
    for (int i = 0; i < maxLights; ++i)
    {
        if (numLights <= i) break;  // to avoid 'WebGL: Loop index cannot be compared with non-constant expression'
        int type = getLightAttributes(i, lightColor, lightPos, lightDir, lightRange, lightSpot);
        if (type == 1)
        {
            //radianceOut += computeDirectionalLight(
            //      lightDir, lightColor, eyeNormal, viewDir, albedo, specular, roughness, metallic, F0);
            radianceOut += get_directional_light_contribution(
                    viewDir, eyeVertex.xyz, lightPos, lightDir, lightColor, albedo, metallic, roughness,
                    eyeNormal, lightRange);
        }
        else if (type == 2)
        {
            radianceOut += get_point_light_contribution(
                    viewDir, eyeVertex.xyz, lightPos, lightColor, albedo, metallic, roughness, eyeNormal, lightRange);
        }
        else if (type == 3)
        {
            radianceOut += get_spot_light_contribution(
                    viewDir, eyeVertex.xyz, lightPos, lightDir, lightColor, albedo, metallic, roughness,
                    eyeNormal, lightRange, lightSpot);
        }
    }

    vec3 ambient = vec3(0.25) * albedo;
    radianceOut = mix(radianceOut, radianceOut * emission.rgb, emission.a);
    fragData = vec4(ambient + radianceOut, diffuse.a);
    VERSE_FS_FINAL(fragData);
}
