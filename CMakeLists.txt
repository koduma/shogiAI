cmake_minimum_required(VERSION 3.14)
project(ShogiEngine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Warnings
if(MSVC)
    add_compile_options(/W3)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---- Engine library (shared between engine and tests) ----
add_library(shogi_core STATIC
    src/board.cpp
    src/movegen.cpp
    src/eval.cpp
    src/search.cpp
)
target_include_directories(shogi_core PUBLIC src)
# Bake the repository's absolute source directory into eval.cpp so fv.bin
# auto-discovery has a cwd-independent, always-correct candidate path
# (src/eval/fv.bin), regardless of where the built binary is launched
# from. See src/eval.cpp's build_discovery_candidates().
target_compile_definitions(shogi_core PRIVATE SHOGIAI_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# ---- USI engine executable ----
add_executable(shogi_engine src/main.cpp)
target_link_libraries(shogi_engine PRIVATE shogi_core)

# ---- Tests ----
add_executable(shogi_tests tests/test_main.cpp)
target_link_libraries(shogi_tests PRIVATE shogi_core)
target_include_directories(shogi_tests PRIVATE src)

enable_testing()
add_test(NAME shogi_tests COMMAND shogi_tests)

# ---- Copy eval parameter files to build directory ----
# This makes eval/fv.bin available relative to the build directory,
# which matches the auto-discovery path "eval/fv.bin".
# Re-run cmake to pick up changes to src/eval/ files.
file(COPY ${CMAKE_SOURCE_DIR}/src/eval DESTINATION ${CMAKE_BINARY_DIR})
