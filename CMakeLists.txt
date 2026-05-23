
# reodyssey - ReXGlue Recompiled Project
#
# This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(reodyssey LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(REODYSSEY_SOURCES
    src/main.cpp
    src/patches.cpp
)

if(WIN32)
    add_executable(reodyssey WIN32 ${REODYSSEY_SOURCES})
else()
    add_executable(reodyssey ${REODYSSEY_SOURCES})
endif()

rexglue_setup_target(reodyssey)
