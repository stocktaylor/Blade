# blade - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(blade LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(BLADE_SOURCES
    src/main.cpp
    src/blade_display.cpp
    src/blade_hooks.cpp
    src/blade_launcher.cpp
    src/blade_strings.cpp
)

if(WIN32)
    add_executable(blade WIN32 ${BLADE_SOURCES})
else()
    add_executable(blade ${BLADE_SOURCES})
endif()

rexglue_setup_target(blade GPU_PLUGINS xenos)
