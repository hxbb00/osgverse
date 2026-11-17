# Vendored VkSplat core (Apache-2.0), modified by osgVerse:
#   - glm replaced with Eigen
#   - Python bindings and the web viewer removed
#   - COLMAP parsing moved to plugins/osgdb_colmap
# Shaders live in assets/shaders/3dgs_trainer (slang sources + prebuilt SPIR-V).

SET(LIB_NAME osgVerseVkSplat)
SET(LIBRARY_FILES
    src/buffer.cpp
    src/knn.cpp
    src/normalize.cpp
    src/scheduler.cpp
    src/gs_pipeline.cpp
    src/gs_renderer.cpp
    src/gs_trainer.cpp
    src/perf_timer.cpp
    src/stb_image_impl.cpp
)
SET(LIBRARY_INCLUDE_FILES
    src/config.h
    src/buffer.h
    src/knn.h
    src/normalize.h
    src/scheduler.h
    src/gs_pipeline.h
    src/gs_renderer.h
    src/gs_trainer.h
    src/perf_timer.h
    src/stb_loader.h
)

FIND_PACKAGE(Vulkan QUIET)
IF(NOT Vulkan_FOUND)
    MESSAGE("[osgVerse] Vulkan not found. VkSplat trainer will be ignored.")
    RETURN()
ENDIF()

ADD_LIBRARY(${LIB_NAME} STATIC ${LIBRARY_FILES} ${LIBRARY_INCLUDE_FILES})
SET_TARGET_PROPERTIES(${LIB_NAME} PROPERTIES DEBUG_POSTFIX "${CMAKE_DEBUG_POSTFIX}")
SET_TARGET_PROPERTIES(${LIB_NAME} PROPERTIES ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${PROJECT_BINARY_DIR}/lib"
                                                 ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${PROJECT_BINARY_DIR}/lib"
                                                 LIBRARY_OUTPUT_DIRECTORY_DEBUG "${PROJECT_BINARY_DIR}/lib"
                                                 LIBRARY_OUTPUT_DIRECTORY_RELEASE "${PROJECT_BINARY_DIR}/lib"
                                                 RUNTIME_OUTPUT_DIRECTORY_DEBUG "${PROJECT_BINARY_DIR}/bin"
                                                 RUNTIME_OUTPUT_DIRECTORY_RELEASE "${PROJECT_BINARY_DIR}/bin")
TARGET_INCLUDE_DIRECTORIES(${LIB_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${VULKAN_INCLUDE_DIR})
TARGET_INCLUDE_DIRECTORIES(${LIB_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../stb)
TARGET_LINK_LIBRARIES(${LIB_NAME} PUBLIC Vulkan::Vulkan)
TARGET_COMPILE_OPTIONS(${LIB_NAME} PUBLIC -D_SCL_SECURE_NO_WARNINGS)
TARGET_COMPILE_FEATURES(${LIB_NAME} PUBLIC cxx_std_17)

INSTALL(TARGETS ${LIB_NAME} EXPORT ${LIB_NAME}
        RUNTIME DESTINATION ${INSTALL_BINDIR} COMPONENT libosgverse
        LIBRARY DESTINATION ${INSTALL_LIBDIR} COMPONENT libosgverse
        ARCHIVE DESTINATION ${INSTALL_ARCHIVEDIR} COMPONENT libosgverse-dev)
INSTALL(FILES ${LIBRARY_INCLUDE_FILES} LICENSE NOTICE CITATION.bib
        DESTINATION ${INSTALL_INCDIR}/osgVerse/vksplat COMPONENT libosgverse-dev)
