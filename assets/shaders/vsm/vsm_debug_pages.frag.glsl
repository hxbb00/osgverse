// Debug preview of one page table layer. Resident pages are coloured by level (fine = cyan,
// coarse = orange); unmapped texels stay dark blue.
uniform sampler2DArray VsmDebugPages;
uniform vec2 VsmDebugPageParams;  // (tiling count, number of levels)

VERSE_FS_IN vec4 texCoord0;
VERSE_FS_OUT vec4 fragData;

void main()
{
    vec4 page = VERSE_TEX2DARRAY(VsmDebugPages, vec3(texCoord0.xy, 0.0));
    if (page.w < 0.5)
        fragData = vec4(0.02, 0.02, 0.08, 1.0);
    else
    {
        float level = page.z / max(VsmDebugPageParams.y - 1.0, 1.0);
        vec3 color = mix(vec3(0.1, 0.9, 1.0), vec3(1.0, 0.45, 0.1), level);
        fragData = vec4(color, 1.0);
    }
    VERSE_FS_FINAL(fragData);
}
