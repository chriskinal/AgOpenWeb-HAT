// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors
//
// A simulated ISOBUS implement for tests/isobus_tc_probe.py: a section control client that
// connects to a task controller and prints its section states whenever they change.
//
// The implement is AgIsoStack++'s SectionControlImplementSimulator, from the stack's
// examples/task_controller_client (MIT); this file is that example's main() with the section
// count as an argument and the printing added.
//
//     isobus_implement INTERFACE [SECTIONS]      e.g. isobus_implement vcan1 16
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include "isobus/hardware_integration/can_hardware_interface.hpp"
#include "isobus/hardware_integration/socket_can_interface.hpp"
#include "isobus/isobus/can_network_manager.hpp"
#include "isobus/isobus/can_partnered_control_function.hpp"
#include "isobus/isobus/isobus_task_controller_client.hpp"
#include "section_control_implement_sim.hpp"

static std::atomic_bool running{true};
static void onSignal(int) { running = false; }

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: isobus_implement INTERFACE [SECTIONS]\n");
        return 2;
    }
    const int sections = argc > 2 ? std::atoi(argv[2]) : 16;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    auto driver = std::make_shared<isobus::SocketCANInterface>(argv[1]);
    isobus::CANHardwareInterface::set_number_of_can_channels(1);
    isobus::CANHardwareInterface::assign_can_channel_frame_handler(0, driver);
    if (!isobus::CANHardwareInterface::start() || !driver->get_is_valid()) {
        std::fprintf(stderr, "could not open %s\n", argv[1]);
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    isobus::NAME name(0);
    name.set_arbitrary_address_capable(true);
    name.set_industry_group(2);
    name.set_device_class(6);
    name.set_function_code(static_cast<std::uint8_t>(isobus::NAME::Function::RateControl));
    name.set_identity_number(2);
    name.set_manufacturer_code(1407);

    using P = isobus::NAME::NAMEParameters;
    const std::vector<isobus::NAMEFilter> tcFilters = {
        isobus::NAMEFilter(P::FunctionCode, static_cast<std::uint8_t>(isobus::NAME::Function::TaskController)),
        isobus::NAMEFilter(P::FunctionInstance, 0),
        isobus::NAMEFilter(P::IndustryGroup, static_cast<std::uint8_t>(isobus::NAME::IndustryGroup::AgriculturalAndForestryEquipment)),
        isobus::NAMEFilter(P::DeviceClass, static_cast<std::uint8_t>(isobus::NAME::DeviceClass::NonSpecific))};
    auto ecu = isobus::CANNetworkManager::CANNetwork.create_internal_control_function(name, 0);
    auto tc = isobus::CANNetworkManager::CANNetwork.create_partnered_control_function(0, tcFilters);
    auto client = std::make_shared<isobus::TaskControllerClient>(tc, ecu, nullptr);

    SectionControlImplementSimulator sim;
    sim.set_number_of_sections(static_cast<std::uint8_t>(sections));
    auto ddop = std::make_shared<isobus::DeviceDescriptorObjectPool>();
    if (!sim.create_ddop(ddop, ecu->get_NAME())) {
        std::fprintf(stderr, "could not build the device description\n");
        return 1;
    }
    client->configure(ddop, 1, static_cast<std::uint8_t>(sections), 1, true, false, true, false, true);
    client->add_request_value_callback(SectionControlImplementSimulator::request_value_command_callback, &sim);
    client->add_value_command_callback(SectionControlImplementSimulator::value_command_callback, &sim);
    client->initialize(true);

    // One line a change: "sections" and one character a section, section 1 first; and whether
    // the task controller has us connected.
    std::string last;
    bool lastConnected = false;
    while (running) {
        std::string now;
        for (int i = 0; i < sections; ++i) now += sim.get_section_state(static_cast<std::uint8_t>(i)) ? '1' : '0';
        if (now != last) {
            std::printf("sections %s\n", now.c_str());
            std::fflush(stdout);
            last = now;
        }
        const bool connected = client->get_is_connected();
        if (connected != lastConnected) {
            std::printf("connected %d\n", connected ? 1 : 0);
            std::fflush(stdout);
            lastConnected = connected;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    client->terminate();
    isobus::CANHardwareInterface::stop();
    return 0;
}
