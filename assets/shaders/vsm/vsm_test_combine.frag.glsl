// Test-only virtual shadow combine pass. It replaces std_shadow_combine.frag.glsl in the
// standalone example so the main directional light is shaded from the VSM atlas instead of the
// CSM cascade. The production integration will move the same lookup into the per-light loop of
// std_pbr_lighting.frag.glsl.
#include "vsm/vsm_shadowing.module.glsl"

uniform sampler2D ColorBuffer, DepthBuffer, EmissionBuffer;
uniform mat4 GBufferMatrices[4];  // w2v, v2w, v2p, p2v

uniform mat4 VsmWorldToLight;     // world -> light view space
uniform vec4 VsmRegion;           // (minX, minY, 1 / sizeX, 1 / sizeY) in light space
uniform float VsmBias;            // normalized depth bias

VERSE_FS_IN vec4 texCoord0;
VERSE_FS_OUT vec4 fragData;

void main()
{
    vec2 uv0 = texCoord0.xy;
    vec4 color = VERSE_TEX2D(ColorBuffer, uv0);
    float depthValue = VERSE_TEX2D(DepthBuffer, uv0).r * 2.0 - 1.0;
    if (depthValue >= 1.0)
    {
        fragData = color;
        VERSE_FS_FINAL(fragData);
        return;
    }

    // Rebuild the world position of this pixel from the deferred depth
    vec4 vecInProj = vec4(uv0 * 2.0 - 1.0, depthValue, 1.0);
    vec4 eyeVertex = GBufferMatrices[3] * vecInProj;
    vec3 eyePos = eyeVertex.xyz / eyeVertex.w;
    vec4 worldPos = GBufferMatrices[1] * vec4(eyePos, 1.0);
    vec3 positionWS = worldPos.xyz / worldPos.w;

    // Sample the virtual shadow map. Shadows only affect the direct lighting result
    float shadow = vsmSampleShadow(0, positionWS, VsmWorldToLight, VsmRegion, VsmBias);
    color.rgb *= shadow;
    fragData = color;
    VERSE_FS_FINAL(fragData);
}
