SET(LIB_NAME osgVerseWrappers)
SET(LIBRARY_INCLUDE_FILES
    Export.h
)
SET(LIBRARY_FILES ${LIBRARY_INCLUDE_FILES}
    modeling/GaussianGeometry.cpp modeling/DynamicPointLine.cpp modeling/DynamicPolyline.cpp
    modeling/DynamicPolygon.cpp modeling/IndirectCommandDrawElements.cpp
    pipeline/CSGNode.cpp pipeline/LightDrawable.cpp pipeline/SkyBox.cpp
    pipeline/MultiEffectGeode.cpp pipeline/MultiEffectGroup.cpp
    pipeline/ScriptableProgram.cpp pipeline/SymbolManager.cpp pipeline/VolumetricCloud.cpp
    pipeline/LightCullCallback.cpp pipeline/ShaderStorageBufferObject.cpp
    animation/PlayerAnimation.cpp animation/TweenAnimation.cpp animation/BlendShapeAnimation.cpp
    animation/ParticleCloud.cpp animation/ParticleSystemU3D.cpp
    readerwriter/DracoGeometry.cpp readerwriter/Feature.cpp readerwriter/FeatureCollection.cpp
    readerwriter/TileGeometryHandler.cpp readerwriter/TileCallback.cpp
)

IF(OSG_MAJOR_VERSION GREATER 2 AND OSG_MINOR_VERSION GREATER 4)
    SET(LIBRARY_FILES ${LIBRARY_FILES}
        WrapperMethods.cpp Geometry_FixedWrapper.cpp)
ENDIF()

INCLUDE_DIRECTORIES(. .. ../script modeling pipeline animation readerwriter)
ADD_DEFINITIONS(-DVERSE_WRAPPERS_LIBRARY)
IF(DRACO_FOUND)
    ADD_DEFINITIONS(-DVERSE_USE_DRACO)
ENDIF(DRACO_FOUND)

IF(VERSE_STATIC_BUILD)
    NEW_LIBRARY(${LIB_NAME} STATIC)
ELSE()
    NEW_LIBRARY(${LIB_NAME} SHARED)
ENDIF()

TARGET_COMPILE_OPTIONS(${LIB_NAME} PUBLIC -D_SCL_SECURE_NO_WARNINGS)
TARGET_LINK_LIBRARIES(${LIB_NAME} osgVerseDependency osgVerseModeling osgVerseScript
                      osgVerseAnimation osgVersePipeline osgVerseAI osgVerseReaderWriter)
LINK_OSG_LIBRARY(${LIB_NAME} OpenThreads osg osgDB osgUtil osgGA osgViewer)

INSTALL(TARGETS ${LIB_NAME} EXPORT ${LIB_NAME}
        RUNTIME DESTINATION ${INSTALL_BINDIR} COMPONENT libosgverse
        LIBRARY DESTINATION ${INSTALL_LIBDIR} COMPONENT libosgverse
        ARCHIVE DESTINATION ${INSTALL_ARCHIVEDIR} COMPONENT libosgverse-dev)
INSTALL(FILES ${LIBRARY_INCLUDE_FILES} DESTINATION ${INSTALL_INCDIR}/osgVerse/wrappers COMPONENT libosgverse-dev)
