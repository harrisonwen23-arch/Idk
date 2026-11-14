cmake_minimum_required(VERSION 3.25)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(custom-modmenu VERSION 1.0.0)

file(GLOB_RECURSE SOURCES
    src/*.cpp
)

add_library(${PROJECT_NAME} SHARED ${SOURCES})

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find GEODE_SDK environment variable!")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)
setup_geode_mod(${PROJECT_NAME})
