# blade - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(blade LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Assets: the files the dashboard needs, copied out of its flash dump into
# assets/ by cmake/extract_assets.cmake. Codegen reads assets/default.xex, and
# it is the game data root at run time.
set(BLADE_DUMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dash" CACHE PATH
    "The 1888 dashboard's flash files (dash.xex, *.xzp, *.xtt)")
set(BLADE_ASSETS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets")
include(cmake/blade_assets.cmake)
if(EXISTS "${BLADE_DUMP_DIR}/dash.xex")
    set(_blade_asset_inputs "")
    foreach(_entry IN LISTS BLADE_ASSET_FILES)
        string(REGEX REPLACE "=.*" "" _from "${_entry}")
        list(APPEND _blade_asset_inputs "${BLADE_DUMP_DIR}/${_from}")
    endforeach()
    add_custom_command(
        OUTPUT "${BLADE_ASSETS_DIR}/.extracted"
        COMMAND ${CMAKE_COMMAND} -DDUMP_DIR=${BLADE_DUMP_DIR} -DASSETS_DIR=${BLADE_ASSETS_DIR}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/extract_assets.cmake
        DEPENDS ${_blade_asset_inputs} cmake/extract_assets.cmake cmake/blade_assets.cmake
        COMMENT "Extracting assets from ${BLADE_DUMP_DIR}"
        VERBATIM)
    add_custom_target(blade_assets DEPENDS "${BLADE_ASSETS_DIR}/.extracted")
    add_dependencies(blade_codegen blade_assets)
elseif(EXISTS "${BLADE_ASSETS_DIR}/default.xex")
    message(STATUS "Blade: no flash dump at ${BLADE_DUMP_DIR}; using the assets already in "
                   "${BLADE_ASSETS_DIR}")
else()
    message(FATAL_ERROR "Blade: no flash dump at ${BLADE_DUMP_DIR}. Set BLADE_DUMP_DIR to the "
                        "folder with the 1888 dashboard's dash.xex, *.xzp and *.xtt files.")
endif()

# ON: Blade, the dashboard as a PC launcher for other rexglue recomps (Games >
# Demos lists launcher.toml, strings.toml relabels the UI, Display settings
# drive the window and launched games). OFF: the stock dashboard, with only
# the fixes it needs to run.
option(BLADE_LAUNCHER "Build the dashboard as a launcher for rexglue recomps" ON)

# Sources
set(BLADE_SOURCES
    src/main.cpp
    src/blade_display.cpp
    src/blade_hooks.cpp
)
if(BLADE_LAUNCHER)
    list(APPEND BLADE_SOURCES
        src/blade_launcher.cpp
        src/blade_strings.cpp
    )
endif()

if(WIN32)
    add_executable(blade WIN32 ${BLADE_SOURCES})
else()
    add_executable(blade ${BLADE_SOURCES})
endif()

target_compile_definitions(blade PRIVATE
    BLADE_LAUNCHER=$<BOOL:${BLADE_LAUNCHER}>
    BLADE_PROJECT_ASSETS_DIR="${BLADE_ASSETS_DIR}")

rexglue_setup_target(blade GPU_PLUGINS xenos)

# rexlauncher's in-game menu (F1): its Controls page assigns controllers and
# maps the keyboard as a virtual controller.
if(BLADE_LAUNCHER)
    set(REXLAUNCHER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../rexlauncher" CACHE PATH
        "Path to the rexlauncher source tree")
    add_subdirectory("${REXLAUNCHER_DIR}" rexlauncher)
    target_link_libraries(blade PRIVATE rexlauncher)
endif()
