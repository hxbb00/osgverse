VERSE_FS_IN vec4 color, invCovariance;
VERSE_FS_IN vec2 center2D;
VERSE_FS_OUT vec4 fragData;
// x: rendering mode, y: max quad radius in sigma, z: alpha cutoff (<= 0 means 0.01)
uniform vec3 GaussianParameters;

void main()
{
    vec2 d = gl_FragCoord.xy - center2D;  // FIXME: use texture for gaussian evaluation
    mat2 cov2Dinv = mat2(invCovariance.xy, invCovariance.zw);
    float g = exp(-0.5 * dot(d, cov2Dinv * d));
    //float dist2 = dot(d, cov2Dinv * d);
    //float g = 1.0 / (1.0 + 0.5 * dist2 + 0.125 * dist2 * dist2);

    float cutoff = (GaussianParameters.z > 0.0) ? GaussianParameters.z : 0.01;
    float alpha = color.a * g; if (alpha <= cutoff/*(1.0 / 255.0)*/) discard;
    alpha = min(alpha, 0.99); fragData = vec4(color.rgb * alpha, alpha);
    VERSE_FS_FINAL(fragData);
}
