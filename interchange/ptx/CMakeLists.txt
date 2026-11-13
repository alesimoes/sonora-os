# .ptx import (ADR-0156): neutral document, reader interface, importer.
# Reached from interchange/CMakeLists.txt by add_subdirectory(ptx) once
# interchange.sessions.aaf_import has merged. Until then configure it
# standalone, with a sonora_interchange target defined by the caller.
add_library(sonora_ptx INTERFACE)
target_include_directories(sonora_ptx INTERFACE include)
target_link_libraries(sonora_ptx INTERFACE sonora_interchange)

add_executable(test_ptx_contract tests/test_ptx_contract.cpp)
target_include_directories(test_ptx_contract PRIVATE tests)
target_link_libraries(test_ptx_contract PRIVATE sonora_ptx)
add_test(NAME interchange_ptx_contract COMMAND test_ptx_contract)

add_library(sonora_ptx_map STATIC src/ptx_map.cpp)
target_include_directories(sonora_ptx_map PUBLIC src)
target_link_libraries(sonora_ptx_map PUBLIC sonora_ptx)

add_executable(test_ptx_map tests/test_ptx_map.cpp)
target_link_libraries(test_ptx_map PRIVATE sonora_ptx_map)
add_test(NAME interchange_ptx_map COMMAND test_ptx_map)

# libptformat adapter. Off by default; found on the system, never fetched.
option(SONORA_WITH_LIBPTFORMAT "Decode .ptx through libptformat" OFF)
if(SONORA_WITH_LIBPTFORMAT)
    find_path(PTFORMAT_INCLUDE_DIR ptformat/ptformat.h)
    find_library(PTFORMAT_LIBRARY NAMES ptformat)
    if(NOT PTFORMAT_INCLUDE_DIR OR NOT PTFORMAT_LIBRARY)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(PTFORMAT_PC QUIET ptformat)
        endif()
    endif()
    if((PTFORMAT_INCLUDE_DIR AND PTFORMAT_LIBRARY) OR PTFORMAT_PC_FOUND)
        add_library(sonora_ptx_libptformat STATIC src/ptx_libptformat_reader.cpp)
        target_include_directories(sonora_ptx_libptformat PUBLIC src)
        target_link_libraries(sonora_ptx_libptformat PUBLIC sonora_ptx sonora_audiofile)
        if(PTFORMAT_PC_FOUND)
            target_include_directories(sonora_ptx_libptformat PRIVATE ${PTFORMAT_PC_INCLUDE_DIRS})
            target_link_libraries(sonora_ptx_libptformat PUBLIC ${PTFORMAT_PC_LINK_LIBRARIES})
        else()
            target_include_directories(sonora_ptx_libptformat PRIVATE ${PTFORMAT_INCLUDE_DIR})
            target_link_libraries(sonora_ptx_libptformat PUBLIC ${PTFORMAT_LIBRARY})
        endif()
        add_executable(test_ptx_libptformat tests/test_ptx_libptformat.cpp)
        target_compile_definitions(test_ptx_libptformat PRIVATE
            FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
        target_link_libraries(test_ptx_libptformat PRIVATE sonora_ptx_libptformat)
        add_test(NAME interchange_ptx_libptformat COMMAND test_ptx_libptformat)
    else()
        message(WARNING "SONORA_WITH_LIBPTFORMAT is ON but libptformat was not found")
    endif()
endif()
