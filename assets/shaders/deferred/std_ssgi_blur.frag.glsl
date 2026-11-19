// Separable bilateral blur of the SSGI output (std_ssgi.frag.glsl): two passes, horizontal and
// vertical, which remove the noise of the interleaved sampling. The radiance in .rgb is
// premultiplied by the coverage in .a, so both ends of the kernel can be averaged with the
// same weights without darkening an uncovered neighborhood. The blur is depth aware like the
// SSAO one, but reads the window depth directly from the G-Buffer depth instead of carrying a
// copy in the buffer, so all four channels stay available for the GI data itself
uniform sampler2D SsgiBuffer, DepthBuffer;
uniform vec2 BlurDirection, InvScreenResolution;
uniform float BlurSharpness;
VERSE_FS_IN vec4 texCoord0;
VERSE_FS_OUT vec4 fragData;

const float KERNEL_RADIUS = 4.0;

vec4 blurFunction(vec2 uv, float r, float center_d, inout float w_total)
{
    vec4 c = VERSE_TEX2D(SsgiBuffer, uv);
    float d = VERSE_TEX2D(DepthBuffer, uv).r;
    const float blurSigma = float(KERNEL_RADIUS) * 0.5;
    const float blurFalloff = 1.0 / (2.0 * blurSigma * blurSigma);
    float ddiff = (d - center_d) * BlurSharpness;
    float w = exp2(-r * r * blurFalloff - ddiff * ddiff);
    w_total += w; return c * w;
}

void main()
{
    vec2 uv0 = texCoord0.xy;
    vec4 center_c = VERSE_TEX2D(SsgiBuffer, uv0);
    float center_d = VERSE_TEX2D(DepthBuffer, uv0).r;
    vec4 c_total = center_c; float w_total = 1.0;

    for (float r = 1.0; r <= KERNEL_RADIUS; ++r)
    {
        vec2 uv = uv0 + BlurDirection * InvScreenResolution * r;
        c_total += blurFunction(uv, r, center_d, w_total);
    }
    for (float r = 1.0; r <= KERNEL_RADIUS; ++r)
    {
        vec2 uv = uv0 - BlurDirection * InvScreenResolution * r;
        c_total += blurFunction(uv, r, center_d, w_total);
    }

    fragData = vec4(c_total.rgb / w_total, c_total.a / w_total);
    VERSE_FS_FINAL(fragData);
}
