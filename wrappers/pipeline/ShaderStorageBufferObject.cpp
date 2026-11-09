#include <osg/BufferObject>
#include <osgDB/ObjectWrapper>

/** Shader storage buffers only contain GPU-side runtime data; the meaningful contents are
    rebuilt when the owning object (e.g. osgVerse::GaussianGeometry) is finalized. OSG has no
    wrapper for this class, which would otherwise break saving any scene containing an SSBO. */
REGISTER_OBJECT_WRAPPER(ShaderStorageBufferObject,
                        new osg::ShaderStorageBufferObject,
                        osg::ShaderStorageBufferObject,
                        "osg::Object osg::BufferObject osg::ShaderStorageBufferObject")
{
}
