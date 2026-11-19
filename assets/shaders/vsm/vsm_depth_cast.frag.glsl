// Depth-only caster shader for one virtual shadow page. The output is the normalized
// light-space depth in [0, 1] (the same space the sampling shader compares against), stored
// in the R channel of the page result / atlas texture.
VERSE_FS_IN vec4 lightProjVec;
VERSE_FS_OUT vec4 fragData;

void main()
{
    fragData = vec4(lightProjVec.z / lightProjVec.w * 0.5 + 0.5, 0.0, 0.0, 1.0);
    VERSE_FS_FINAL(fragData);
}
