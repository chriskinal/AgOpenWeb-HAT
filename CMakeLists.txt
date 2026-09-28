cmake_minimum_required(VERSION 3.18)
project(aow-hatd VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(-Wall -Wextra -Wpedantic)

# core: pure logic. No Linux, no hardware. Builds and tests on any desktop.
add_library(aow_core
  core/src/pgn.cpp)
target_include_directories(aow_core PUBLIC core/include hal/include)

# daemon: Linux only (the HAT drivers need spidev, SocketCAN, termios).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(aow-hatd daemon/main.cpp)
  target_include_directories(aow-hatd PRIVATE boards/hat)
  target_link_libraries(aow-hatd PRIVATE aow_core)
  install(TARGETS aow-hatd RUNTIME DESTINATION bin)
endif()

enable_testing()
add_executable(test_pgn tests/test_pgn.cpp)
target_link_libraries(test_pgn PRIVATE aow_core)
add_test(NAME pgn COMMAND test_pgn)
