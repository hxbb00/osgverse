// Debug preview of the physical shadow atlas: near depths are dark, far depths are light.
uniform sampler2D VsmDebugAtlas;

VERSE_FS_IN vec4 texCoord0;
VERSE_FS_OUT vec4 fragData;

void main()
{
    float depth = clamp(VERSE_TEX2D(VsmDebugAtlas, texCoord0.xy).r, 0.0, 1.0);
    fragData = vec4(vec3(1.0 - depth), 1.0);
    VERSE_FS_FINAL(fragData);
}
