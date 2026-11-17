#pragma import_defines(FAR_DISTANCE, MID_DISTANCE)
#ifdef FAR_DISTANCE
uniform sampler2D IconTexture;
#else
uniform sampler2D BackgroundTexture, TextTexture;
#endif

VERSE_FS_IN vec2 TexCoord, TexCoordBG;
VERSE_FS_IN vec4 UserColor;
VERSE_FS_OUT vec4 fragData;

void main()
{
#ifdef FAR_DISTANCE
    vec4 baseColor = VERSE_TEX2D(IconTexture, TexCoordBG);
    fragData = baseColor * UserColor;
#else
    vec4 textColor = VERSE_TEX2D(TextTexture, TexCoord);
    vec4 bgColor = VERSE_TEX2D(BackgroundTexture, TexCoordBG);
    fragData = mix(bgColor * UserColor, textColor, textColor.a);
#endif
    VERSE_FS_FINAL(fragData);
}
