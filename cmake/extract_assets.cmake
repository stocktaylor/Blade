# blade - copies the files the dashboard needs out of a flash dump into the
# assets folder (codegen's input and the game data root at run time).
#
#   cmake -DDUMP_DIR=<flash dump> -DASSETS_DIR=<assets folder> -P extract_assets.cmake
#
# The dump is the 1888 (September 2007) dashboard's flash files, as in dash/:
# dash.xex (copied as default.xex), the XUI packages it opens through media:,
# and the fonts. dash.xex is checked against the hash codegen was set up for.
# Files already up to date are left alone, so their timestamps don't trigger
# codegen again.

cmake_minimum_required(VERSION 3.25)

foreach(var DUMP_DIR ASSETS_DIR)
    if(NOT ${var})
        message(FATAL_ERROR "extract_assets: ${var} not set")
    endif()
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/blade_assets.cmake")

set(_missing "")
foreach(_entry IN LISTS BLADE_ASSET_FILES)
    string(REPLACE "=" ";" _pair "${_entry}")
    list(GET _pair 0 _from)
    if(NOT EXISTS "${DUMP_DIR}/${_from}")
        list(APPEND _missing "${_from}")
    endif()
endforeach()
if(_missing)
    list(JOIN _missing ", " _missing)
    message(FATAL_ERROR "extract_assets: ${DUMP_DIR} is missing ${_missing}.\n"
                        "Point BLADE_DUMP_DIR at the 1888 dashboard's flash files.")
endif()

file(SHA256 "${DUMP_DIR}/dash.xex" _hash)
if(NOT _hash STREQUAL BLADE_DASH_SHA256)
    message(FATAL_ERROR "extract_assets: ${DUMP_DIR}/dash.xex is not the 1888 dashboard "
                        "Blade was recompiled from.\n  SHA-256 ${_hash}\n  expected ${BLADE_DASH_SHA256}")
endif()

file(MAKE_DIRECTORY "${ASSETS_DIR}")
foreach(_entry IN LISTS BLADE_ASSET_FILES)
    string(REPLACE "=" ";" _pair "${_entry}")
    list(GET _pair 0 _from)
    list(GET _pair 1 _to)
    file(COPY_FILE "${DUMP_DIR}/${_from}" "${ASSETS_DIR}/${_to}" ONLY_IF_DIFFERENT)
endforeach()
# Newer than every input, so the build doesn't run this again until one changes.
file(TOUCH "${ASSETS_DIR}/.extracted")
message(STATUS "Assets extracted from ${DUMP_DIR} to ${ASSETS_DIR}")
