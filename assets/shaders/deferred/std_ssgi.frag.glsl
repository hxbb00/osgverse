// Screen-space global illumination, one diffuse bounce (SSDO style, Mattausch et al. 2012).
// A set of samples is taken in a screen-space disk around the pixel (exactly like the SSAO
// kernel of std_ssao.frag.glsl); each reconstructed sample point is either
// - an occluder above the horizon of this pixel: it also *emits* light towards it, which is
//   the one-bounce diffuse GI: the outgoing radiance of the occluder (its shadowed direct
//   lighting, read from the shadow combining stage output, already carries its own albedo);
// - beyond the GI radius, or below the horizon, or background: the ray escaped the range the
//   screen-space trace can resolve, and the pixel is reported as "uncovered".
// The output is (RGB GI radiance, hemisphere coverage), the radiance + coverage pair the
// indirect lighting chain works with: the tone mapping stage combines it with the IBL diffuse
// fallback as "GI + (1 - coverage) * IBL" (see std_tonemapping.frag.glsl). The GI radiance is
// premultiplied by the coverage, so the following blur pass can average it without darkening
// an uncovered neighborhood.
// Not implemented yet (later iterations): temporal accumulation (needs the velocity buffer),
// a bent normal output for the specular term, a second bounce.
uniform sampler2D NormalBuffer, DepthBuffer, CombinedBuffer, RandomTexture;
uniform mat4 GBufferMatrices[4];  // w2v, v2w, v2p, p2v
uniform vec2 NearFarPlanes, InvScreenResolution;
uniform float GIRadius;     // world-unit radius of the traced hemisphere; <= 0 uses the
                            // screen-space default below (scenes here range from centimeter
                            // objects to a globe, so a single world radius can not fit all)
uniform float GIIntensity;  // overall gain of the bounce radiance
VERSE_FS_IN vec4 texCoord0;
VERSE_FS_OUT vec4 fragData;
#include "../normal_encoding.module.glsl"

#define AO_RANDOMTEX_SIZE 4.0
const float NUM_SAMPLES = 16.0;
const float TWO_PI = 6.28318530718;
const float DEFAULT_SCREEN_RADIUS = 40.0;  // pixels, ~5% of the screen height

vec3 fetchViewPos(vec2 uv)
{
    mat4 projMatrix = GBufferMatrices[2];
    vec4 projInfo = vec4(2.0 / projMatrix[0][0], 2.0 / projMatrix[1][1],
                         -(1.0 - projMatrix[2][0]) / projMatrix[0][0],
                         -(1.0 + projMatrix[2][1]) / projMatrix[1][1]);
    float depthValue = VERSE_TEX2D(DepthBuffer, uv).r;
    float eyeZ = (NearFarPlanes[0] * NearFarPlanes[1])
               / ((NearFarPlanes[0] - NearFarPlanes[1]) * depthValue + NearFarPlanes[1]);
    return vec3(uv * projInfo.xy + projInfo.zw, 1.0) * eyeZ;
}

float falloff(float distanceSquare, float radiusWorld)
{
    // Same shape as the SSAO kernel: 1 at the center, 0 at the radius
    float negInvR2 = -1.0 / (radiusWorld * radiusWorld);
    return clamp(distanceSquare * negInvR2 + 1.0, 0.0, 1.0);
}

void main()
{
    vec2 uv0 = texCoord0.xy;
    float depth01 = VERSE_TEX2D(DepthBuffer, uv0).r;
    if (depth01 >= 1.0)
    {
        fragData = vec4(0.0, 0.0, 0.0, 0.0);  // background: nothing to trace, no coverage
        VERSE_FS_FINAL(fragData); return;
    }

    vec3 P = fetchViewPos(uv0);
    vec4 normalAlpha = VERSE_TEX2D(NormalBuffer, uv0);
    vec3 N = decode_normal_octahedral(normalAlpha.rg);

    // Radius of the disk the hemisphere projects to, in pixels. With a positive GIRadius this
    // is that world-space radius projected at the depth of the pixel (as the SSAO kernel does);
    // otherwise a constant on-screen radius is used and the world radius it corresponds to is
    // derived back per pixel, so that one setting stays meaningful for scenes of any scale
    float projScale = 1.0 / (tan((3.14159265358979 * 0.25) * 0.5) * 2.0);
    float radiusPixels = (GIRadius > 0.0)
        ? GIRadius * 0.5 * projScale / InvScreenResolution.y / P.z : DEFAULT_SCREEN_RADIUS;
    float radiusWorld = radiusPixels * 2.0 * InvScreenResolution.y * P.z / projScale;

    // Interleaved hemispherical sampling: golden-angle spirals in the disk, rotated by a
    // per-pixel random angle and jittered radially (same idea as computeCoarseAO() there)
    vec4 rand = VERSE_TEX2D(RandomTexture, gl_FragCoord.xy / AO_RANDOMTEX_SIZE);
    float angle = rand.x * TWO_PI;
    vec2 rot = vec2(cos(angle), sin(angle));

    vec3 bounceSum = vec3(0.0);
    float occlusionSum = 0.0;
    float minDistSquare = radiusWorld * 0.05; minDistSquare *= minDistSquare;
    float maxDistSquare = radiusWorld * radiusWorld;
    for (float i = 0.0; i < NUM_SAMPLES; ++i)
    {
        float theta = 2.399963 * i + rand.y * TWO_PI;  // golden angle spiral
        float r = sqrt((i + rand.z) / NUM_SAMPLES);
        vec2 offset = vec2(cos(theta), sin(theta)) * r;
        offset = vec2(offset.x * rot.x - offset.y * rot.y,
                      offset.x * rot.y + offset.y * rot.x);
        vec2 uv = uv0 + offset * radiusPixels * InvScreenResolution;

        float sampleDepth = VERSE_TEX2D(DepthBuffer, uv).r;
        if (sampleDepth >= 1.0) continue;  // a background sample is an escaped ray
        vec3 Q = fetchViewPos(uv);
        vec3 D = Q - P;
        float distSquare = dot(D, D);
        if (distSquare < minDistSquare || distSquare >= maxDistSquare) continue;

        float cosT = dot(N, D * inversesqrt(distSquare));
        if (cosT <= 0.0) continue;  // below the horizon of this pixel: never visible

        // The sample occludes the hemisphere direction towards it, and bounces its own
        // (already shadowed, already colored) radiance back to this pixel
        float w = cosT * falloff(distSquare, radiusWorld);
        occlusionSum += w;
        bounceSum += VERSE_TEX2D(CombinedBuffer, uv).rgb * w;
    }

    // Uniform hemisphere sampling estimates the diffuse irradiance integral as
    // (2*PI / N) * sum(radiance * cos). The coverage is the occluded fraction of the
    // hemisphere weight, which is what the IBL fallback is faded by at the combine
    float coverage = clamp(occlusionSum / NUM_SAMPLES, 0.0, 1.0);
    vec3 gi = bounceSum * (TWO_PI / NUM_SAMPLES) * GIIntensity;

    // Dropping NaN of a broken radiance here, as the lighting stage does for its ambient
    float lum = dot(gi, vec3(0.2126, 0.7152, 0.0722));
    if (lum != lum) gi = vec3(0.0);

    fragData = vec4(gi, coverage);
    VERSE_FS_FINAL(fragData);
}
