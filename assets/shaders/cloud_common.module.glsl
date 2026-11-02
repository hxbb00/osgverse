// Common constants, uniforms and density functions shared by the volumetric cloud passes.
// This file is expanded by osgVerse's shader definition pass, so it must not contain a
// #version directive nor the VERSE_* macro definitions themselves.

#ifndef CLOUD_COMMON_MODULE
#define CLOUD_COMMON_MODULE

#if defined(VERSE_GLES3)
precision highp sampler3D;
#endif

#define CLOUD_PI 3.14159265359
#define CLOUD_MAX_STEPS 128
#define CLOUD_MAX_LIGHT_STEPS 16

uniform sampler2D CloudSceneDepth, CloudWeatherMap;
uniform sampler3D CloudShapeVolume, CloudDetailVolume, CloudWeatherVolume;
uniform mat4 CameraToWorld, ScreenToCamera;
uniform vec3 WorldCameraPos, WorldSunDir, SunColor, CloudAmbient, CloudFadeColor;
uniform vec3 CloudWind, CloudPlanetCenter;
uniform vec4 CloudHeightGradient, CloudShapeBias;
uniform vec2 CloudResolution;
uniform float CloudLayerBottom, CloudLayerTop, CloudCoverage, CloudDensity, CloudExtinction,
              CloudAlbedo, CloudType, CloudShapeAmount, CloudDetailAmount, CloudTurbulence,
              CloudAnvilBias, CloudMaxDistance, CloudFadeStart, CloudFadeEnd, CloudTime,
              CloudFrame, CloudPlanetRadius, CloudShapeScale, CloudDetailScale,
              CloudDetailDistance, CloudWeatherScale;
uniform int CloudMaxSteps, CloudLightSteps, CloudUseDetail, CloudUseShadow, CloudUseDepth,
            CloudPlanetMode;

float cloudSaturate(float v) { return clamp(v, 0.0, 1.0); }

float cloudRemap(float v, float a, float b, float c, float d)
{
    return c + (v - a) * (d - c) / max(b - a, 1e-6);
}

// Interleaved gradient noise, used to jitter the first marching sample
float cloudIGN(vec2 p)
{
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

float cloudHG(float g, float cosTheta)
{
    float g2 = g * g;
    float denom = max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4);
    return (1.0 - g2) / (4.0 * CLOUD_PI * pow(denom, 1.5));
}

// Dual-lobe Henyey-Greenstein phase function, renormalized to keep cloud albedo close to 1
float cloudPhase(float cosTheta)
{
    return (0.7 * cloudHG(0.80, cosTheta) + 0.3 * cloudHG(-0.20, cosTheta)) * 4.0 * CLOUD_PI;
}

// Multiple scattering approximation, see "Physically Based Sky, Atmosphere and Cloud
// Rendering in Frostbite" (Schneider, 2015)
vec3 cloudMultiScatter(float opticalDepth, float cosTheta, float albedo)
{
    vec3 result = vec3(0.0);
    vec3 coeff = vec3(1.0);
    for (int i = 0; i < 6; ++i)
    {
        float beer = exp(-opticalDepth * coeff.y);
        float phase = cloudPhase(cosTheta * coeff.z);
        result += coeff.x * beer * phase;
        coeff *= vec3(0.5);
    }
    return result * albedo;
}

float cloudAltitude(vec3 p)
{
    if (CloudPlanetMode > 0)
        return length(p - CloudPlanetCenter) - CloudPlanetRadius;
    return p.z;
}

vec2 cloudWeatherUV(vec3 p)
{
    if (CloudPlanetMode > 0)
    {
        vec3 n = normalize(p - CloudPlanetCenter);
        float phi = atan(n.y, n.x);
        float theta = asin(clamp(n.z, -1.0, 1.0));
        return vec2(phi * 0.15915494 + 0.5, theta * 0.31830989 + 0.5);
    }
    return p.xy * 0.00002 + 0.5;
}

// Weather (coverage / type / warp) lookup. The globe mode reads a 3D volume: the
// equirectangular projection of a 2D map collapses at the poles, which turns into a
// star-like "stitched" pattern when the cloud deck is seen from above a pole
vec4 cloudWeatherSample(vec3 p)
{
    if (CloudPlanetMode > 0) return VERSE_TEX3D(CloudWeatherVolume, p * CloudWeatherScale);
    return VERSE_TEX2D(CloudWeatherMap, cloudWeatherUV(p));
}

float cloudHeightFraction(vec3 p)
{
    return (cloudAltitude(p) - CloudLayerBottom) / max(CloudLayerTop - CloudLayerBottom, 1.0);
}

float cloudHeightGradient(float heightFraction)
{
    float fadeIn = cloudSaturate(cloudRemap(heightFraction, CloudHeightGradient.x,
                                           CloudHeightGradient.y, 0.0, 1.0));
    float fadeOut = cloudSaturate(cloudRemap(heightFraction, CloudHeightGradient.z,
                                            CloudHeightGradient.w, 1.0, 0.0));
    return fadeIn * fadeOut;
}

// Returns the extinction coefficient (per meter) at the given world position.
// detailMode: 0 = no erosion (cheap light samples), 1 = full detail volume, 2 = coarse
// erosion from the weather channels (far samples, where the detail noise would alias)
float cloudSampleDensity(vec3 p, int detailMode)
{
    float heightFraction = cloudHeightFraction(p);
    if (heightFraction <= 0.0 || heightFraction >= 1.0) return 0.0;

    float gradient = cloudHeightGradient(heightFraction);
    if (gradient <= 0.001) return 0.0;

    vec3 wp = p + CloudWind * CloudTime;
    vec4 weather = cloudWeatherSample(wp);
    float coverage = cloudSaturate(weather.r * CloudCoverage * 2.0);

    // Domain warp driven by the weather map: it breaks the visible repetition of the
    // tiling 3D noise without any additional texture fetch
    vec3 warp = (vec3(weather.g, weather.b, 0.0) - 0.5) * (CloudTurbulence * 0.5);
    float verticalStretch = (CloudType > 3.5) ? 1.8 : mix(1.6, 0.55, CloudType / 3.0);

    // The noise volume tiles every 1/CloudShapeScale meters, so a short period becomes an
    // obvious repeating pattern when looking down on the deck. CloudShapeScale is set per
    // mode on the CPU (large for the globe, smaller for a local scene). The vertical noise
    // coordinate comes from the height inside the layer instead of the world Z: that avoids
    // horizontal strata and keeps the tile period independent of the layer thickness
    vec3 shapePos = wp * CloudShapeScale + warp;
    shapePos.z = heightFraction * min(verticalStretch * 0.6, 0.9);

    vec4 shape = VERSE_TEX3D(CloudShapeVolume, shapePos);
    float shapeFBM = dot(shape.gba, vec3(0.625, 0.25, 0.125));
    float base = cloudRemap(shape.r, shapeFBM - 1.0, 1.0, 0.0, 1.0);
    base = cloudSaturate(base * CloudShapeAmount) * gradient;

    // A positive shape bias erodes the cloud edges; useful for wispy cirrus presets
    float bias = max(CloudShapeBias.x - 0.5, 0.0) * 0.6;
    base = cloudSaturate(base - bias);

    // Anvil: widen the upper part of tall clouds (cumulonimbus)
    float anvil = CloudAnvilBias * smoothstep(0.55, 1.0, heightFraction);
    coverage = cloudSaturate(coverage + anvil * 0.35);

    float typeShift = (weather.g - 0.5) * 0.15;
    float density = cloudSaturate(base + typeShift);

    float threshold = 1.0 - coverage;
    density = cloudSaturate((density - threshold) / max(coverage, 1e-3));

    if (detailMode == 1 && CloudUseDetail > 0)
    {
        vec3 detailPos = wp * CloudDetailScale;
        detailPos.z = heightFraction * 3.0;
        vec3 detail = VERSE_TEX3D(CloudDetailVolume, detailPos).rgb;
        float detailFBM = dot(detail, vec3(0.625, 0.25, 0.125));
        float modifier = mix(detailFBM, 1.0 - detailFBM, cloudSaturate(heightFraction * 4.0));
        float erosion = modifier * CloudDetailAmount;
        density = cloudSaturate((density - erosion) / max(1.0 - erosion, 1e-3));
    }
    else if (detailMode == 2 && CloudUseDetail > 0)
    {
        // Far away the detail volume would alias into a stitch-like pattern, but dropping
        // it completely leaves a solid overcast sheet. The smooth weather channel is used
        // instead: its features are much larger than the pixel footprint, so it breaks the
        // sheet without the cellular weave of the Worley channels
        float coarse = weather.b;
        float erosion = min(coarse * CloudDetailAmount * 0.6, 0.6);
        density = cloudSaturate((density - erosion) / max(1.0 - erosion, 1e-3));
    }
    return density * CloudDensity;
}

// Intersects the view ray with the cloud layer (a slab in local mode, a shell around the
// planet in globe mode). t0/t1 are distances along the ray
bool cloudIntersectLayer(vec3 origin, vec3 dir, out float t0, out float t1)
{
    if (CloudPlanetMode > 0)
    {
        vec3 oc = origin - CloudPlanetCenter;
        float innerRadius = CloudPlanetRadius + CloudLayerBottom;
        float outerRadius = CloudPlanetRadius + CloudLayerTop;
        float b = dot(oc, dir);
        float cOuter = dot(oc, oc) - outerRadius * outerRadius;
        float hOuter = b * b - cOuter;
        if (hOuter < 0.0) return false;

        float tOuter0 = -b - sqrt(hOuter), tOuter1 = -b + sqrt(hOuter);
        float cInner = dot(oc, oc) - innerRadius * innerRadius;
        float hInner = b * b - cInner;
        if (dot(oc, oc) < innerRadius * innerRadius)
        {
            // Below the cloud layer: the ray leaves the inner sphere before entering the shell
            if (hInner <= 0.0) return false;
            t0 = -b + sqrt(hInner); t1 = tOuter1;
        }
        else
        {
            // Inside the shell or above it: stop at the inner sphere only when the ray
            // really hits it in front of the camera, otherwise the outer exit is the end
            t0 = max(tOuter0, 0.0); t1 = tOuter1;
            if (hInner > 0.0)
            {
                float tInner0 = -b - sqrt(hInner);
                if (tInner0 > t0) t1 = min(t1, tInner0);
            }
        }
        t0 = max(t0, 0.0);
        // The shell intersection already bounds the march; CloudMaxDistance is only a
        // local-scene limit and would hide the clouds completely when seen from orbit
        if (CloudPlanetMode <= 0) t1 = min(t1, CloudMaxDistance);
        return t1 > t0;
    }
    else
    {
        if (abs(dir.z) < 1e-5)
        {
            if (origin.z < CloudLayerBottom || origin.z > CloudLayerTop) return false;
            t0 = 0.0; t1 = CloudMaxDistance;
        }
        else
        {
            float ta = (CloudLayerBottom - origin.z) / dir.z;
            float tb = (CloudLayerTop - origin.z) / dir.z;
            t0 = max(min(ta, tb), 0.0);
            t1 = min(max(ta, tb), CloudMaxDistance);
        }
        return t1 > t0;
    }
}

vec3 cloudRayDirection(vec2 uv)
{
    vec4 clip = vec4(uv * 2.0 - 1.0, 1.0, 1.0);
    vec4 view = ScreenToCamera * clip;
    view.xyz /= max(view.w, 1e-6);
    return normalize((CameraToWorld * vec4(view.xyz, 0.0)).xyz);
}

// Distance from the camera to the opaque scene, reconstructed from the depth buffer
float cloudSceneDistance(vec2 uv, vec3 origin, vec3 dir)
{
    // In the globe mode the march is bounded by the cloud shell itself, and every segment
    // inside the shell is in front of the planet surface (the inner sphere clips the ray),
    // so the scene depth is not needed. Using it would also hide the clouds completely
    // whenever the depth texture is stale or does not match the view camera
    if (CloudPlanetMode > 0) return 1.0e12;

    if (CloudUseDepth <= 0)
    {
        if (dir.z < -1e-5)
        {
            if (origin.z <= 0.0) return 0.0;
            return -origin.z / dir.z;
        }
        return CloudMaxDistance;
    }

    float depth = VERSE_TEX2D(CloudSceneDepth, uv).r;
    if (depth >= 0.99999) return CloudMaxDistance;
    vec4 clip = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 view = ScreenToCamera * clip;
    view.xyz /= max(view.w, 1e-6);
    vec3 world = (CameraToWorld * vec4(view.xyz, 1.0)).xyz;
    return max(0.0, length(world - origin));
}

float cloudOpticalDepthToSun(vec3 p, float jitter)
{
    float thickness = max(CloudLayerTop - CloudLayerBottom, 1.0);
    float stepSize = thickness / float(max(CloudLightSteps, 1));
    float t = stepSize * jitter;
    float opticalDepth = 0.0;

    // Cone sampling: offset each light sample perpendicular to the sun direction, which
    // turns the hard density samples into a soft shadow cone and removes banding
    vec3 up = abs(WorldSunDir.z) < 0.99 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent1 = normalize(cross(WorldSunDir, up));
    vec3 tangent2 = cross(WorldSunDir, tangent1);
    float coneRadius = stepSize * 0.15;

    for (int i = 0; i < CLOUD_MAX_LIGHT_STEPS; ++i)
    {
        if (i >= CloudLightSteps) break;
        float angle = jitter * 6.2831853 + float(i) * 2.3999632;
        vec3 offset = (tangent1 * cos(angle) + tangent2 * sin(angle)) * coneRadius;
        vec3 q = p + WorldSunDir * t + offset;
        opticalDepth += cloudSampleDensity(q, 0) * stepSize;
        stepSize *= 1.6; t += stepSize;
        coneRadius += stepSize * 0.2;
    }
    return opticalDepth;
}

#endif
