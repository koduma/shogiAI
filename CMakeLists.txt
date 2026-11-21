cmake_minimum_required(VERSION 3.14)
project(ShogiEngine CXX)
find_package(Threads REQUIRED)
include(CheckCXXCompilerFlag)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (default Release for engine speed)" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SHOGIAI_MARCH_NATIVE "Enable host SIMD instructions (-march=native) for NNUE speed" ON)

# Warnings & optimization defaults
if(MSVC)
    add_compile_options(/W3)
    if(SHOGIAI_MARCH_NATIVE)
        check_cxx_compiler_flag("/arch:AVX2" COMPILER_SUPPORTS_ARCH_AVX2)
        if(COMPILER_SUPPORTS_ARCH_AVX2)
            add_compile_options(/arch:AVX2)
        endif()
    endif()
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    if(SHOGIAI_MARCH_NATIVE)
        check_cxx_compiler_flag("-march=native" COMPILER_SUPPORTS_MARCH_NATIVE)
        if(COMPILER_SUPPORTS_MARCH_NATIVE)
            add_compile_options(-march=native)
        endif()
    endif()
endif()

# ---- Engine library (shared between engine and tests) ----
add_library(shogi_core STATIC
    src/board.cpp
    src/movegen.cpp
    src/eval.cpp
    src/search.cpp
)
target_include_directories(shogi_core PUBLIC src)
target_compile_definitions(shogi_core PRIVATE SHOGIAI_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# ---- USI engine executable ----
add_executable(shogi_engine src/main.cpp)
target_link_libraries(shogi_engine PRIVATE shogi_core Threads::Threads)

# ---- Tests ----
add_executable(shogi_tests tests/test_main.cpp)
target_link_libraries(shogi_tests PRIVATE shogi_core)
target_include_directories(shogi_tests PRIVATE src)

enable_testing()
add_test(NAME shogi_tests COMMAND shogi_tests)

# ---- Copy eval parameter files (nn.bin, progress.bin) to build directory ----
file(COPY ${CMAKE_SOURCE_DIR}/src/eval DESTINATION ${CMAKE_BINARY_DIR})
