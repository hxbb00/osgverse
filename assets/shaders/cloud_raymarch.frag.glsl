#include "cloud_common.module.glsl"
#include "cloud_raymarch.module.glsl"

VERSE_FS_IN vec4 texCoord;
VERSE_FS_OUT vec4 fragColor;

void main()
{
    vec3 scattered; float transmittance, startDistance; vec3 frontPoint;
    cloudRaymarch(texCoord.xy, scattered, transmittance, startDistance, frontPoint);

    float alpha = 1.0 - transmittance;
    float fade = smoothstep(CloudFadeStart, CloudFadeEnd, startDistance);
    scattered = mix(scattered, CloudFadeColor * alpha, fade * 0.85);

    fragColor = vec4(scattered, transmittance);
    VERSE_FS_FINAL(fragColor);
}
