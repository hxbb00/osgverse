# VerseStrike: fetch external free/CC0 asset packs at INSTALL time.
# Runs via INSTALL(SCRIPT ...) so it executes after every "cmake --build ... --target install".
# Entries come from ../assets_manifest.cmake, each formatted as:
#     name|subdir|url1;url2;...
# Mirrors are tried in order; a pack is skipped (with a warning) if none is reachable.
# The game runs with purely procedural content when nothing is fetched.

include("${CMAKE_CURRENT_LIST_DIR}/../assets_manifest.cmake")

set(_game_dir "${CMAKE_INSTALL_PREFIX}/games/Strike")
file(MAKE_DIRECTORY "${_game_dir}/downloads")

foreach(_pack IN LISTS VERSE_STRIKE_ASSET_PACKS)
    string(REPLACE "|" ";" _parts "${_pack}")
    list(GET _parts 0 _name)
    list(GET _parts 1 _subdir)
    list(GET _parts 2 _urls)

    set(_dest "${_game_dir}/${_subdir}")
    set(_marker "${_dest}/.fetched")
    if(EXISTS "${_marker}")
        message(STATUS "[GameStrike] Asset pack '${_name}' already installed, skipping.")
        continue()
    endif()

    set(_zip "${_game_dir}/downloads/${_name}.zip")
    set(_ok FALSE)
    foreach(_url IN LISTS _urls)
        message(STATUS "[GameStrike] Downloading '${_name}' from ${_url} ...")
        file(DOWNLOAD "${_url}" "${_zip}" STATUS _st TIMEOUT 300 SHOW_PROGRESS
             USER_AGENT "Mozilla/5.0")
        list(GET _st 0 _code)
        if(_code EQUAL 0)
            set(_ok TRUE)
            break()
        else()
            list(GET _st 1 _msg)
            message(WARNING "[GameStrike] Download failed (${_msg}): ${_url}")
            file(REMOVE "${_zip}")
        endif()
    endforeach()

    if(_ok)
        file(MAKE_DIRECTORY "${_dest}")
        if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.18)
            file(ARCHIVE_EXTRACT INPUT "${_zip}" DESTINATION "${_dest}")
        else()
            execute_process(COMMAND "${CMAKE_COMMAND}" -E tar xf "${_zip}"
                            WORKING_DIRECTORY "${_dest}" RESULT_VARIABLE _xrv)
        endif()
        file(WRITE "${_marker}" "${_name}\n")
        file(REMOVE "${_zip}")
        message(STATUS "[GameStrike] Asset pack '${_name}' installed to ${_dest}")
    else()
        message(WARNING "[GameStrike] Asset pack '${_name}' skipped (no reachable mirror). "
                        "The game still runs with purely procedural content.")
    endif()
endforeach()
