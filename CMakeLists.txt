# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The AgOpenWeb-HAT contributors

cmake_minimum_required(VERSION 3.18)
project(aow-hatd VERSION 0.3.0 LANGUAGES CXX)  # the next release: nightlies are X.Y.Z-nightly.DATE.RUN (release.yml)

# Optimised unless told otherwise: an unoptimised ISOBUS stack is several times slower.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ISOBUS (docs/PLAN.md section 8): AgIsoStack++ (MIT) at upstream's latest commit. Pinned so a build
# is repeatable, not to hold it back: move the pin to upstream HEAD before each package, then run
# the unit tests and the three tests/isobus_*probe.py on vcan1. AOG-TaskController's file must
# keep building unchanged.
# Carried on top until upstream merges them; drop each at the first pin move after its merge:
# - PR #793 (issue #656), a lock on the transmit queue's push. The queue is single-producer and
#   the stack's hardware thread and our aow-isobus thread both send, so frames were lost (seen on
#   the bench as a missing ETP data packet or DPO).
# - PR #808 (issue #807), a TP/ETP session's first frame sent once. A transmit started on our
#   aow-isobus thread while the hardware thread updates could send its RTS twice; the receiver
#   answered twice and the transfer failed (seen on the bench as a repeated pool upload). Also a
#   lock on ETP's session list.
# Fetched before our warning flags are set, so they do not apply to it. Needs the network at
# the first configure; -DAOW_ISOBUS=OFF builds the daemon without the stack.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  option(AOW_ISOBUS "Build the ISOBUS stack into aow-hatd" ON)
endif()
if(AOW_ISOBUS)
  include(FetchContent)
  set(CAN_DRIVER "SocketCAN" CACHE STRING "" FORCE)
  FetchContent_Declare(isobus
    URL https://github.com/Open-Agriculture/AgIsoStack-plus-plus/archive/7b771274422ea0f62d4eac042fa1684ee373d463.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    PATCH_COMMAND git apply ${CMAKE_CURRENT_SOURCE_DIR}/third_party/agisostack-patches/pr793-transmit-queue.patch
                            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/agisostack-patches/pr808-transport-first-frame.patch)
  FetchContent_MakeAvailable(isobus)
endif()

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
  core/src/settings.cpp
  core/src/steer.cpp
  core/src/motor.cpp
  core/src/gvret.cpp
  core/src/helper_status.cpp
  core/src/um98x.cpp
  core/src/indicators.cpp
  core/src/imu.cpp
  core/src/port_monitor.cpp
  core/src/serial_detect.cpp
  core/src/loop_timing.cpp
  core/src/wifi.cpp
  core/src/can/teensy_compat.cpp
  core/src/can/CANConfigParser.cpp
  core/src/can/CANProtocolEngine.cpp
  core/src/can/TractorCANDriver.cpp)
# The firmware's CAN files are ported near-verbatim; keep them quiet about their unused bits.
set_source_files_properties(core/src/can/CANConfigParser.cpp core/src/can/CANProtocolEngine.cpp
  core/src/can/TractorCANDriver.cpp PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter;-Wno-unused-private-field;-Wno-unknown-warning-option")
target_include_directories(aow_core PUBLIC core/include hal/include third_party/ArduinoJson/src)

# web: the firmware's pages and API on POSIX sockets. No hardware; builds on any POSIX system.
add_library(aow_web web/server.cpp web/api.cpp)
target_include_directories(aow_web PUBLIC web)
target_link_libraries(aow_web PUBLIC aow_core)

find_package(Threads REQUIRED)

# daemon: Linux only (the HAT drivers need spidev, SocketCAN, termios).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(aow-hatd daemon/main.cpp daemon/linux_io.cpp daemon/shared.cpp daemon/web_thread.cpp
    daemon/isobus.cpp daemon/can_watch.cpp daemon/serial_hub.cpp daemon/wifi.cpp
    boards/hat/hat_board.cpp boards/hat/hat_leds.cpp boards/hat/hat_piezo.cpp)
  target_include_directories(aow-hatd PRIVATE boards/hat daemon)
  set(AOW_VERSION "${PROJECT_VERSION}" CACHE STRING "Version reported by aow-hatd and stamped on update packages")
  target_compile_definitions(aow-hatd PRIVATE AOW_VERSION="${AOW_VERSION}")
  target_link_libraries(aow-hatd PRIVATE aow_core aow_web Threads::Threads)
  if(AOW_ISOBUS)
    target_compile_definitions(aow-hatd PRIVATE AOW_ISOBUS=1)
    # AOG-TaskController's task controller, unmodified, with the daemon's stand-ins for the two
    # headers it expects (third_party/aog-taskcontroller/README.md).
    target_sources(aow-hatd PRIVATE third_party/aog-taskcontroller/task_controller.cpp
      daemon/isobus_compat/compat.cpp daemon/isobus_vt.cpp)
    # Its status screen for a terminal (an object pool), as a byte array, the way upstream does it.
    set(iop ${CMAKE_CURRENT_SOURCE_DIR}/third_party/aog-taskcontroller/AOG_TC.iop)
    file(READ ${iop} iop_hex HEX)
    string(REGEX REPLACE "([0-9a-fA-F][0-9a-fA-F])" "0x\\1," iop_bytes "${iop_hex}")
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/AOG_TC_iop_data.hpp
      "#pragma once\n\n#include <cstdint>\n\ninline constexpr std::uint8_t AOG_TC_IOP_DATA[] = {${iop_bytes}};\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${iop})
    target_include_directories(aow-hatd PRIVATE third_party/aog-taskcontroller daemon/isobus_compat
      ${CMAKE_CURRENT_BINARY_DIR}/generated)
    # It uses std::find_if without <algorithm>, which GCC 14 does not pull in for it: include
    # it from here, so the file stays as upstream has it.
    set_source_files_properties(third_party/aog-taskcontroller/task_controller.cpp PROPERTIES
      COMPILE_OPTIONS "-include;algorithm;-Wno-unused-parameter;-Wno-sign-compare;-Wno-pedantic")
    target_link_libraries(aow-hatd PRIVATE isobus::Isobus isobus::HardwareIntegration isobus::Utility)
  endif()
  install(TARGETS aow-hatd RUNTIME DESTINATION bin)

  # The AOW link gateway (docs/PLAN_VT.md section 2): its own program and service, off by default.
  # It lives in SharpIsoTerm (gateway/linux, MIT), fetched at a pinned commit; the pin follows its
  # main. Built with this package's version, and put beside aow-hatd in the build directory.
  include(FetchContent)
  set(AOW_LINKD_VERSION "${AOW_VERSION}" CACHE STRING "" FORCE)
  FetchContent_Declare(sharpisoterm
    URL https://github.com/chriskinal/SharpIsoTerm/archive/356997c2e194ed66a3013ca7913cf505a25a2b8d.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR gateway/linux)
  FetchContent_MakeAvailable(sharpisoterm)
  set_target_properties(aow-linkd PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endif()

# A simulated ISOBUS implement, for tests/isobus_tc_probe.py. Built from the stack's example.
if(AOW_ISOBUS)
  set(isobus_example ${isobus_SOURCE_DIR}/examples/task_controller_client)
  add_executable(isobus_implement tests/isobus_implement.cpp ${isobus_example}/section_control_implement_sim.cpp)
  target_include_directories(isobus_implement PRIVATE ${isobus_example})
  set_source_files_properties(${isobus_example}/section_control_implement_sim.cpp PROPERTIES COMPILE_OPTIONS "-w")
  target_link_libraries(isobus_implement PRIVATE isobus::Isobus isobus::HardwareIntegration isobus::Utility Threads::Threads)
  # A stand-in virtual terminal for tests/isobus_vt_probe.py.
  add_executable(isobus_vt tests/isobus_vt.cpp)
  target_link_libraries(isobus_vt PRIVATE isobus::Isobus isobus::HardwareIntegration isobus::Utility Threads::Threads)
endif()

enable_testing()
foreach(t pgn wifi app_link helper_status gnss gnss_link imu port_monitor serial_detect log settings steer steer_motor can can_stall gvret um98x indicators loop_timing web_server web_api hat_leds)
  add_executable(test_${t} tests/test_${t}.cpp)
  target_link_libraries(test_${t} PRIVATE aow_web)
  target_include_directories(test_${t} PRIVATE boards/hat)
  target_compile_definitions(test_${t} PRIVATE AOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")  # test data files
  add_test(NAME ${t} COMMAND test_${t})
endforeach()
