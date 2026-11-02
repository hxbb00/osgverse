uniform sampler2D SceneColor, CloudBuffer, SceneDepth;
uniform int DebugMode, UseBilateral;
uniform vec2 CloudBufferSize;

VERSE_FS_IN vec4 texCoord;
VERSE_FS_OUT vec4 fragColor;

// Depth-aware upsample of the (usually half resolution) cloud buffer. A plain bilinear
// fetch would spread cloud pixels over foreground silhouettes (mountains, buildings),
// which shows up as dark halos around geometry edges. Only the four texels used by the
// bilinear fetch are combined, so the cloud detail is not additionally blurred.
vec4 sampleCloud(vec2 uv)
{
    if (UseBilateral <= 0) return VERSE_TEX2D(CloudBuffer, uv);

    vec2 cloudPos = uv * CloudBufferSize - 0.5;
    vec2 base = floor(cloudPos);
    vec2 fraction = cloudPos - base;
    float centerDepth = VERSE_TEX2D(SceneDepth, uv).r;
    vec4 accum = vec4(0.0);
    float weightSum = 0.0;
    for (int j = 0; j < 2; ++j)
    {
        for (int i = 0; i < 2; ++i)
        {
            vec2 sampleUv = (base + vec2(float(i), float(j)) + 0.5) / CloudBufferSize;
            float depth = VERSE_TEX2D(SceneDepth, sampleUv).r;
            float weight = (i == 0 ? 1.0 - fraction.x : fraction.x) *
                           (j == 0 ? 1.0 - fraction.y : fraction.y);
            weight /= 1.0 + 3000.0 * abs(depth - centerDepth);
            accum += VERSE_TEX2D(CloudBuffer, sampleUv) * weight;
            weightSum += weight;
        }
    }
    return accum / max(weightSum, 1e-5);
}

void main()
{
    vec4 scene = VERSE_TEX2D(SceneColor, texCoord.st);
    vec4 cloud = sampleCloud(texCoord.st);
    vec3 color = scene.rgb * cloud.a + cloud.rgb;
    if (DebugMode == 1) color = cloud.rgb;
    else if (DebugMode == 2) color = vec3(cloud.a);
    else if (DebugMode == 3) color = scene.rgb;
    fragColor = vec4(clamp(color, 0.0, 1.0), 1.0);
    VERSE_FS_FINAL(fragColor);
}
