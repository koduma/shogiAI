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

# ---- USI engine executable ----
add_executable(shogi_engine src/main.cpp)
target_link_libraries(shogi_engine PRIVATE shogi_core)

# ---- Tests ----
add_executable(shogi_tests tests/test_main.cpp)
target_link_libraries(shogi_tests PRIVATE shogi_core)
target_include_directories(shogi_tests PRIVATE src)

enable_testing()
add_test(NAME shogi_tests COMMAND shogi_tests)
