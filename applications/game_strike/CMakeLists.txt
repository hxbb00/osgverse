SET(EXE_NAME osgVerse_GameStrike)
SET(EXECUTABLE_FILES
    GameMain.cpp Systems.cpp
)

NEW_EXECUTABLE(${EXE_NAME} SHARED)
SET_PROPERTY(TARGET ${EXE_NAME} PROPERTY FOLDER "APPLICATIONS")
TARGET_LINK_LIBRARIES(${EXE_NAME} osgVerseReaderWriter osgVerseAnimation osgVersePipeline
                      osgVerseWrappers osgVerseModeling osgVerseScript osgVerseUI osgVerseAI
                      osgVerseDependency ${EXTERNAL_LIBRARIES})
LINK_OSG_LIBRARY(${EXE_NAME} OpenThreads osg osgDB osgUtil osgGA osgText osgSim osgTerrain osgViewer)
TARGET_COMPILE_OPTIONS(${EXE_NAME} PUBLIC -D_SCL_SECURE_NO_WARNINGS)
IF(MSVC)
    TARGET_COMPILE_OPTIONS(${EXE_NAME} PRIVATE /utf-8)
ENDIF()

# Effekseer is auto-detected (no hard dependency): the flash effects are optional
IF(EFFEKSEER_FOUND)
    TARGET_COMPILE_DEFINITIONS(${EXE_NAME} PRIVATE VERSE_STRIKE_WITH_EFFEKSEER)
ENDIF()

USE_MSVC_DEBUGGER(${EXE_NAME} "${THIRDPARTY_ROOT}/bin;${OSG_BUILD_INCLUDE_DIR}/../bin")
IF(MSVC AND VERSE_INSTALL_PDB_FILES)
    INSTALL(FILES $<TARGET_PDB_FILE:${EXE_NAME}> DESTINATION ${INSTALL_BINDIR} OPTIONAL)
ENDIF()

# External CC0 asset packs: fetched at INSTALL time to <prefix>/games/Strike.
# The game itself is fully procedural, so a failed fetch only means simpler visuals.
INSTALL(SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/cmake/fetch_assets.cmake)
