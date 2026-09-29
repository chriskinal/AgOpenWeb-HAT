cmake_minimum_required(VERSION 3.18)
project(aow-hatd VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(-Wall -Wextra -Wpedantic)

# core: pure logic. No Linux, no hardware. Builds and tests on any desktop.
add_library(aow_core
  core/src/pgn.cpp
  core/src/steer_pgn.cpp
  core/src/app_link.cpp
  core/src/nmea.cpp
  core/src/gnss.cpp
  core/src/nav.cpp
  core/src/gnss_link.cpp
  core/src/log.cpp
  core/src/settings.cpp)
target_include_directories(aow_core PUBLIC core/include hal/include third_party/ArduinoJson/src)

# web: the firmware's pages and API on POSIX sockets. No hardware; builds on any POSIX system.
add_library(aow_web web/server.cpp web/api.cpp)
target_include_directories(aow_web PUBLIC web)
target_link_libraries(aow_web PUBLIC aow_core)

find_package(Threads REQUIRED)

# daemon: Linux only (the HAT drivers need spidev, SocketCAN, termios).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(aow-hatd daemon/main.cpp daemon/linux_io.cpp daemon/shared.cpp daemon/web_thread.cpp)
  target_include_directories(aow-hatd PRIVATE boards/hat)
  target_link_libraries(aow-hatd PRIVATE aow_core aow_web Threads::Threads)
  install(TARGETS aow-hatd RUNTIME DESTINATION bin)
endif()

enable_testing()
foreach(t pgn app_link gnss gnss_link log settings web_server web_api)
  add_executable(test_${t} tests/test_${t}.cpp)
  target_link_libraries(test_${t} PRIVATE aow_web)
  add_test(NAME ${t} COMMAND test_${t})
endforeach()
