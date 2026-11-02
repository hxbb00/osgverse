// Ray-marching implementation shared by the plain and the atmosphere-integrated cloud
// fragment shaders. It must be included after cloud_common.module.glsl.

#ifndef CLOUD_RAYMARCH_MODULE
#define CLOUD_RAYMARCH_MODULE

void cloudRaymarch(vec2 uv, out vec3 scattered, out float transmittance,
                   out float t0, out vec3 frontPoint)
{
    vec3 origin = WorldCameraPos;
    vec3 direction = cloudRayDirection(uv);
    scattered = vec3(0.0); transmittance = 1.0; t0 = 0.0; frontPoint = origin;

    float t1 = 0.0;
    if (!cloudIntersectLayer(origin, direction, t0, t1)) return;

    float sceneDistance = cloudSceneDistance(uv, origin, direction);
    t1 = min(t1, sceneDistance);
    if (t1 <= t0) return;

    frontPoint = origin + direction * (t0 + (t1 - t0) * 0.5);

    float jitter = cloudIGN(gl_FragCoord.xy + vec2(CloudFrame * 5.588238, CloudFrame * 3.17));
    float thickness = max(CloudLayerTop - CloudLayerBottom, 1.0);
    float segmentLength = max(t1 - t0, 1.0);
    // The step has to cover the whole ray segment inside the layer: a ray grazing a thin
    // spherical shell can travel tens of kilometers through it, and using only the layer
    // thickness as the step span would stop the march right at the (empty) layer top
    float baseStep = max(segmentLength / float(max(CloudMaxSteps, 1)), 8.0);
    if (CloudPlanetMode <= 0) baseStep = min(baseStep, thickness * 0.5);
    float cosTheta = dot(WorldSunDir, direction);

    transmittance = 1.0;
    float t = t0 + baseStep * jitter;
    for (int i = 0; i < CLOUD_MAX_STEPS; ++i)
    {
        if (i >= CloudMaxSteps || t > t1) break;

        vec3 position = origin + direction * t;
        // Detail noise has a much shorter period than the shape one: sampling it far away
        // only aliases into a stitch-like pattern, so a coarse erosion is used beyond
        // CloudDetailDistance instead of the detail volume
        float density = cloudSampleDensity(position, t < CloudDetailDistance ? 1 : 2);
        float stepSize = baseStep + (t - t0) * 0.0005;
        stepSize = min(stepSize, segmentLength * 0.25);

        if (density <= 0.0001)
        {
            t += stepSize * 1.5;
            continue;
        }

        float opticalDepth = cloudOpticalDepthToSun(position, jitter);
        vec3 radiance = SunColor * cloudMultiScatter(opticalDepth * CloudExtinction,
                                                     cosTheta, CloudAlbedo);
        // Powder: thin cloud edges scatter less direct sunlight than dense cores
        float powder = 1.0 - exp(-density * stepSize * 2.0);
        radiance *= 0.65 + 0.35 * powder;
        float heightFraction = cloudSaturate(cloudHeightFraction(position));
        radiance += CloudAmbient * CloudAlbedo * (0.28 + 0.72 * heightFraction);

        float extinction = density * CloudExtinction;
        float sampleTransmittance = exp(-extinction * stepSize);
        scattered += transmittance * radiance * (1.0 - sampleTransmittance);
        transmittance *= sampleTransmittance;

        if (transmittance < 0.01) break;
        t += stepSize;
    }
}

#endif
