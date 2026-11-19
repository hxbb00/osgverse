VERSE_SRCIPT_DEF
VERSE_VS_OUT vec4 lightProjVec;

void main()
{
    lightProjVec = VERSE_MATRIX_MVP * osg_Vertex;
    gl_Position = lightProjVec;
}
