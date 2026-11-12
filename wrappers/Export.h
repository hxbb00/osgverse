#ifndef MANA_WRAPPERS_EXPORT_HPP
#define MANA_WRAPPERS_EXPORT_HPP

#if defined(VERSE_STATIC_BUILD)
#  define OSGVERSE_WRAPPERS_EXPORT
#elif defined(VERSE_WINDOWS)
#  if defined(VERSE_WRAPPERS_LIBRARY)
#    define OSGVERSE_WRAPPERS_EXPORT __declspec(dllexport)
#  else
#    define OSGVERSE_WRAPPERS_EXPORT __declspec(dllimport)
#  endif
#else
#  define OSGVERSE_WRAPPERS_EXPORT
#endif

#include <osg/Version>
#include <string>

/** Declare all osgVerse serializers for static builds. Every wrapper source file registers
    one 'wrapper_serializer_<ClassName>' entry, and referencing it here makes sure the
    corresponding object file is kept by the linker. Add new wrapper classes to this list. */
#define USE_VERSE_WRAPPERS() \
    USE_SERIALIZER_WRAPPER(DracoGeometry) \
    USE_SERIALIZER_WRAPPER(GaussianGeometry) \
    USE_SERIALIZER_WRAPPER(DynamicPointLine) \
    USE_SERIALIZER_WRAPPER(DynamicPolyline) \
    USE_SERIALIZER_WRAPPER(DynamicPolygon) \
    USE_SERIALIZER_WRAPPER(IndirectCommandDrawElements) \
    USE_SERIALIZER_WRAPPER(CSGNode) \
    USE_SERIALIZER_WRAPPER(LightDrawable) \
    USE_SERIALIZER_WRAPPER(LightCullCallback) \
    USE_SERIALIZER_WRAPPER(ShaderStorageBufferObject) \
    USE_SERIALIZER_WRAPPER(SkyBox) \
    USE_SERIALIZER_WRAPPER(MultiEffectGeode) \
    USE_SERIALIZER_WRAPPER(MultiEffectGroup) \
    USE_SERIALIZER_WRAPPER(ScriptableProgram) \
    USE_SERIALIZER_WRAPPER(SymbolManager) \
    USE_SERIALIZER_WRAPPER(VolumetricCloud) \
    USE_SERIALIZER_WRAPPER(Drawer2D) \
    USE_SERIALIZER_WRAPPER(OSGVERSE_StringObject) \
    USE_SERIALIZER_WRAPPER(UserCallback) \
    USE_SERIALIZER_WRAPPER(CameraDrawCallback) \
    USE_SERIALIZER_WRAPPER(CameraBaseDrawCallback) \
    USE_SERIALIZER_WRAPPER(ComponentCallback) \
    USE_SERIALIZER_WRAPPER(PlayerAnimation) \
    USE_SERIALIZER_WRAPPER(TweenAnimation) \
    USE_SERIALIZER_WRAPPER(BlendShapeAnimation) \
    USE_SERIALIZER_WRAPPER(ParticleCloud) \
    USE_SERIALIZER_WRAPPER(ParticleSystemU3D) \
    USE_SERIALIZER_WRAPPER(PhysicsEngine) \
    USE_SERIALIZER_WRAPPER(PhysicsUpdateCallback) \
    USE_SERIALIZER_WRAPPER(BvhUserData) \
    USE_SERIALIZER_WRAPPER(Feature) \
    USE_SERIALIZER_WRAPPER(FeatureCollection) \
    USE_SERIALIZER_WRAPPER(TileGeometryHandler) \
    USE_SERIALIZER_WRAPPER(TileCallback)

namespace osgVerse
{
    class RewrapperManager;

#if OSG_VERSION_LESS_THAN(3, 5, 0)
    inline bool updateOsgBinaryWrappers(const std::string& libName = "osg") { return false; }
    inline bool fixOsgBinaryWrappers(const std::string& libName = "osg") { return false; }
#else
    /** Add necessary methods to OSG class wrappers */
    OSGVERSE_WRAPPERS_EXPORT bool updateOsgBinaryWrappers(const std::string& libName = "osg");

    /** A quick function to help fix .osgb dead lock problem */
    OSGVERSE_WRAPPERS_EXPORT bool fixOsgBinaryWrappers(const std::string& libName = "osg");

    /** Load library and rewrapper manager instance used in wrapper classes */
    OSGVERSE_WRAPPERS_EXPORT RewrapperManager* loadRewrappers();
#endif

    /** Setup draco encoding parameters */
    enum EncodingDracoFlag {
        COMPRESS_LEVEL = 0, POSITION_QUANTIZATION = 1,
        UV_QUANTIZATION = 2, NORMAL_QUANTIZATION = 3
    };
    OSGVERSE_WRAPPERS_EXPORT void setEncodingDracoFlag(EncodingDracoFlag flag, int value);

}

#endif
