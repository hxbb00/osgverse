SET(LIB_NAME osgVerseAI)
SET(LIBRARY_INCLUDE_FILES RecastManager.h McpServer.h Utilities.h)
SET(LIBRARY_FILES ${LIBRARY_INCLUDE_FILES}
    RecastManager.cpp RecastManager_Private.h
    RecastManager_Builder.cpp RecastManager_Builder.h
    McpServer.cpp Utilities.cpp)

INCLUDE_DIRECTORIES(../3rdparty/recastnavigation/Detour)
INCLUDE_DIRECTORIES(../3rdparty/libhv ../3rdparty/libhv/all
                    ../3rdparty/stb ../3rdparty/rapidjson)
ADD_DEFINITIONS(-DHV_STATICLIB)

IF(VERSE_SUPPORT_CPP17)
    ADD_DEFINITIONS(-DVERSE_WITH_BYTETRACK)
    IF(GGML_FOUND)
        INCLUDE_DIRECTORIES(${GGML_INCLUDE_DIR})
        ADD_DEFINITIONS(-DVERSE_WITH_GGML -DVERSE_WITH_DEPTHANYTHING3)
        SET(LIBRARY_FILES ${LIBRARY_FILES} DepthAnythingEngine.h DepthAnythingEngine.cpp
                          FreeSplatterEngine.h FreeSplatterEngine.cpp TrellisPipeline.h TrellisPipeline.cpp)
        SET(LIBRARY_INCLUDE_FILES ${LIBRARY_INCLUDE_FILES} DepthAnythingEngine.h
                                  FreeSplatterEngine.h TrellisPipeline.h)
    ELSE(GGML_FOUND)
        MESSAGE("[osgVerse] Dependency GGML not found. "
                "osgVerseAI features will be ignored: 'depth-anything3';'free-splatter';'trellis2'.")
    ENDIF(GGML_FOUND)

    IF(ONNXRUNTIME_FOUND)
        ADD_DEFINITIONS(-DVERSE_WITH_ONNXRUNTIME)
        INCLUDE_DIRECTORIES(${ONNXRUNTIME_INCLUDE_DIR})
        SET(LIBRARY_FILES ${LIBRARY_FILES} OnnxRuntimeEngine.h OnnxRuntimeEngine.cpp)
        SET(LIBRARY_INCLUDE_FILES ${LIBRARY_INCLUDE_FILES} OnnxRuntimeEngine.h)
    ELSE(ONNXRUNTIME_FOUND)
        MESSAGE("[osgVerse] Dependency onnxruntime not found. "
                "osgVerseAI features will be ignored: 'onnx-runtime'.")
    ENDIF(ONNXRUNTIME_FOUND)
ELSE(VERSE_SUPPORT_CPP17)
    MESSAGE("[osgVerse] C++ 17 standard is disabled at present. "
            "osgVerseAI features will be ignored: 'bytetrack';'depth-anything3';'free-splatter';'trellis2';'onnx-runtime'.")
ENDIF(VERSE_SUPPORT_CPP17)

NEW_LIBRARY(${LIB_NAME} STATIC)
TARGET_LINK_LIBRARIES(${LIB_NAME} osgVerseDependency osgVerseModeling)
TARGET_COMPILE_OPTIONS(${LIB_NAME} PUBLIC -D_SCL_SECURE_NO_WARNINGS)

IF(VERSE_SUPPORT_CPP17 AND ONNXRUNTIME_FOUND)
    TARGET_LINK_LIBRARIES(${LIB_NAME} onnxruntime onnxruntime_providers_shared)
    IF(CUDA_FOUND)
        TARGET_LINK_LIBRARIES(${LIB_NAME} onnxruntime_providers_cuda)
        ADD_DEFINITIONS(-DVERSE_USE_CUDA=1)
    ENDIF(CUDA_FOUND)
ENDIF()

IF(VERSE_SUPPORT_CPP17 AND GGML_FOUND)
    TARGET_LINK_LIBRARIES(${LIB_NAME} ggml ggml-base ggml-cpu)
    IF(CUDA_FOUND)
        TARGET_LINK_LIBRARIES(${LIB_NAME} ggml-cuda)
    ELSEIF(MUSA_FOUND)
        TARGET_LINK_LIBRARIES(${LIB_NAME} ggml-musa)
    ENDIF()
ENDIF()

INSTALL(TARGETS ${LIB_NAME} EXPORT ${LIB_NAME}
        RUNTIME DESTINATION ${INSTALL_BINDIR} COMPONENT libosgverse
        LIBRARY DESTINATION ${INSTALL_LIBDIR} COMPONENT libosgverse
        ARCHIVE DESTINATION ${INSTALL_ARCHIVEDIR} COMPONENT libosgverse-dev)
INSTALL(FILES ${LIBRARY_INCLUDE_FILES} DESTINATION ${INSTALL_INCDIR}/osgVerse/ai COMPONENT libosgverse-dev)
