# .ptx import (ADR-0156): neutral document, reader interface, importer.
# Reached from interchange/CMakeLists.txt by add_subdirectory(ptx).
add_library(sonora_ptx INTERFACE)
target_include_directories(sonora_ptx INTERFACE include)
target_link_libraries(sonora_ptx INTERFACE sonora_interchange)

add_executable(test_ptx_contract tests/test_ptx_contract.cpp)
target_include_directories(test_ptx_contract PRIVATE tests)
target_link_libraries(test_ptx_contract PRIVATE sonora_ptx_map)
add_test(NAME interchange_ptx_contract COMMAND test_ptx_contract)

add_library(sonora_ptx_map STATIC src/ptx_map.cpp src/ptx_import.cpp)
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
        target_link_libraries(sonora_ptx_libptformat PUBLIC sonora_ptx_map sonora_ptx_media sonora_audiofile)
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

# Source copy into Audio Files/ (no libptformat needed).
add_library(sonora_ptx_media STATIC src/ptx_media.cpp)
target_include_directories(sonora_ptx_media PUBLIC src)
target_link_libraries(sonora_ptx_media PUBLIC sonora_ptx sonora_audiofile)

add_executable(test_ptx_convert tests/test_ptx_convert.cpp)
target_include_directories(test_ptx_convert PRIVATE tests ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(test_ptx_convert PRIVATE sonora_interchange)
add_test(NAME interchange_ptx_convert COMMAND test_ptx_convert)

