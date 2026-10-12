cmake_minimum_required(VERSION 3.16)
project(sonora LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

# Every executable lands in the build root, so build/sonora-engine stays where
# tools/build-iso.sh and tools/test-engine-docker.sh look for it.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

find_package(Threads REQUIRED)

# Before any add_subdirectory(), so the root CTestTestfile picks up every
# module's tests and `ctest --test-dir build` runs them all.
enable_testing()

# Modules and the dependency graph: ADR-0007 §1. Each module exposes only its
# own include/ directory, so an include across an edge it does not link fails
# to compile. Listed by layer; a module may only link the ones above it.
add_subdirectory(common/base)
add_subdirectory(common/audiofile)
add_subdirectory(contracts/api)
add_subdirectory(engine/core)
# Temporarily illegal until M2: drivers/jack links engine/core, because
# JackBackend::process_cb still calls Session::process. Do not copy it.
add_subdirectory(drivers/jack)
add_subdirectory(ui/keymap)
add_subdirectory(ui/tui)
add_subdirectory(engine/control)
add_subdirectory(engine/app)
