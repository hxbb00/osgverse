// Volumetric cloud pass with Bruneton/Proland atmosphere integration. The atmosphere LUTs
// are used to fade the clouds into the air mass between the camera and the cloud front,
// so distant clouds blend into the same haze as the earth and the sky.
#define SUN_INTENSITY 100.0
#define PLANET_RADIUS 6360000.0

uniform sampler2D TransmittanceSampler, SkyIrradianceSampler, GlareSampler;
uniform sampler3D InscatterSampler;
uniform vec3 EarthOrigin;

#include "cloud_common.module.glsl"
#include "scattering.module.glsl"
#include "cloud_raymarch.module.glsl"

VERSE_FS_IN vec4 texCoord;
VERSE_FS_OUT vec4 fragColor;

void main()
{
    vec3 scattered; float transmittance, startDistance; vec3 frontPoint;
    cloudRaymarch(texCoord.xy, scattered, transmittance, startDistance, frontPoint);

    float alpha = 1.0 - transmittance;
    vec3 extinction = vec3(1.0);
    inScattering(WorldCameraPos + EarthOrigin, frontPoint, WorldSunDir, extinction, 0.0);

    float haze = clamp(1.0 - max(max(extinction.r, extinction.g), extinction.b), 0.0, 1.0);
    scattered = mix(scattered, CloudFadeColor * alpha, haze * 0.9);

    fragColor = vec4(scattered, transmittance);
    VERSE_FS_FINAL(fragColor);
}
