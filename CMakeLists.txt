cmake_minimum_required(VERSION 3.25)

project(KernDB VERSION 0.0.0 LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CTest)
include(KernDBOptions)
include(KernDBWarnings)

add_subdirectory(src)

if(KERNDB_BUILD_SHELL)
    add_subdirectory(apps/kerndb_shell)
endif()

if(BUILD_TESTING AND KERNDB_BUILD_TESTS)
    add_subdirectory(tests)
endif()
