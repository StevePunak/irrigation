cmake_minimum_required(VERSION 3.16)

set(PROJ_MAINS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

set(MAIN_PROJ irrigation)
project(${MAIN_PROJ} VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

option(IRRIGATION_USE_MOLD "Use mold linker on Linux if installed" ON)
if(IRRIGATION_USE_MOLD AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_program(MOLD_EXECUTABLE mold)
    if(MOLD_EXECUTABLE)
        add_link_options(-fuse-ld=mold)
        message(STATUS "Using mold linker: ${MOLD_EXECUTABLE}")
    endif()
endif()

file(WRITE "${CMAKE_SOURCE_DIR}/.clangd"
    "CompileFlags:\n"
    "  CompilationDatabase: ${CMAKE_BINARY_DIR}\n"
)

add_subdirectory(KanoopCommonQt   EXCLUDE_FROM_ALL)
add_subdirectory(KanoopDatabaseQt EXCLUDE_FROM_ALL)
add_subdirectory(KanoopPiQt       EXCLUDE_FROM_ALL)

add_subdirectory(IrrigationD)
