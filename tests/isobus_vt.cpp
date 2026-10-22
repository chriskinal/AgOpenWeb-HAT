// A stand-in ISOBUS virtual terminal with no display, for tests/isobus_vt_probe.py: it takes an
// object pool from a client (the daemon's status screen), and prints what a terminal would show.
//
//   isobus_vt INTERFACE
//
// stdout, one line each:  "connected"  "pool ok" / "pool bad OBJECT"  "mask ID"
//                         "string ID TEXT" (newlines as |)   "number ID VALUE"
// stdin:                  "key OBJECT"  press a soft key     "num OBJECT VALUE"  enter a value
//
// Built on AgIsoStack++'s VirtualTerminalServer, which parses the pool and answers the client.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "isobus/hardware_integration/can_hardware_interface.hpp"
#include "isobus/hardware_integration/socket_can_interface.hpp"
#include "isobus/isobus/can_network_manager.hpp"
#include "isobus/isobus/isobus_virtual_terminal_objects.hpp"
#include "isobus/isobus/isobus_virtual_terminal_server.hpp"
#include "isobus/isobus/isobus_virtual_terminal_server_managed_working_set.hpp"

static std::atomic_bool running{true};
static void onSignal(int) { running = false; }

using WorkingSet = isobus::VirtualTerminalServerManagedWorkingSet;

class Terminal : public isobus::VirtualTerminalServer {
public:
    using isobus::VirtualTerminalServer::VirtualTerminalServer;

    bool get_is_enough_memory(std::uint32_t) const override { return true; }
    VTVersion get_version() const override { return VTVersion::Version5; }
    std::uint8_t get_number_of_navigation_soft_keys() const override { return 0; }
    std::uint8_t get_soft_key_descriptor_x_pixel_width() const override { return 80; }
    std::uint8_t get_soft_key_descriptor_y_pixel_height() const override { return 80; }
    std::uint8_t get_number_of_possible_virtual_soft_keys_in_soft_key_mask() const override { return 64; }
    std::uint8_t get_number_of_physical_soft_keys() const override { return 6; }
    std::uint16_t get_data_mask_area_size_x_pixels() const override { return 480; }
    std::uint16_t get_data_mask_area_size_y_pixels() const override { return 480; }
    void suspend_working_set(std::shared_ptr<WorkingSet>) override {}
    SupportedWideCharsErrorCode get_supported_wide_chars(std::uint8_t, std::uint16_t, std::uint16_t, std::uint8_t& ranges,
                                                         std::vector<std::uint8_t>&) override {
        ranges = 0;
        return SupportedWideCharsErrorCode::AnyOtherError;
    }
    // Nothing is kept between runs, so the client always uploads its pool.
    std::vector<std::array<std::uint8_t, 7>> get_versions(isobus::NAME) override { return {}; }
    std::vector<std::uint8_t> get_supported_objects() const override {
        std::vector<std::uint8_t> all;
        for (int i = 0; i <= 47; ++i) all.push_back(static_cast<std::uint8_t>(i));
        return all;
    }
    std::vector<std::uint8_t> load_version(const std::vector<std::uint8_t>&, isobus::NAME) override { return {}; }
    bool save_version(const std::vector<std::uint8_t>&, const std::vector<std::uint8_t>&, isobus::NAME) override { return true; }
    bool delete_version(const std::vector<std::uint8_t>&, isobus::NAME) override { return true; }
    std::vector<std::array<std::uint8_t, 32>> get_extended_versions(isobus::NAME) override { return {}; }
    bool delete_all_versions(std::uint8_t, isobus::NAME) override { return true; }
    bool delete_object_pool(isobus::NAME) override { return true; }

    void step() {
        update();
        using State = WorkingSet::ObjectPoolProcessingThreadState;
        for (const auto& ws : managedWorkingSetList) {
            const State state = ws->get_object_pool_processing_state();
            if (state == State::Fail && !reportedBad_) {
                reportedBad_ = true;
                std::printf("pool bad %u\n", ws->get_object_pool_faulting_object_id());
            }
            if (state == State::Joined && !activeWorkingSet) {
                activeWorkingSet = ws;
                std::printf("pool ok\nconnected\n");
            }
        }
        if (activeWorkingSet) report();
        std::fflush(stdout);
    }

    void command(const std::string& line) {
        if (!activeWorkingSet) return;
        unsigned id = 0, value = 0;
        const auto client = activeWorkingSet->get_control_function();
        if (std::sscanf(line.c_str(), "key %u", &id) == 1) {
            send_soft_key_activation_message(KeyActivationCode::ButtonPressedOrLatched, static_cast<std::uint16_t>(id), mask_, 1, client);
            send_soft_key_activation_message(KeyActivationCode::ButtonUnlatchedOrReleased, static_cast<std::uint16_t>(id), mask_, 1, client);
        } else if (std::sscanf(line.c_str(), "num %u %u", &id, &value) == 2) {
            send_change_numeric_value_message(static_cast<std::uint16_t>(id), value, client);
        }
    }

private:
    // What changed since the last look: the page, every string a page can show, every number.
    void report() {
        using Type = isobus::VirtualTerminalObjectType;
        const auto& tree = activeWorkingSet->get_object_tree();
        const auto ws = std::static_pointer_cast<isobus::WorkingSet>(activeWorkingSet->get_working_set_object());
        if (ws && ws->get_active_mask() != mask_) {
            mask_ = ws->get_active_mask();
            std::printf("mask %u\n", mask_);
        }
        for (const auto& entry : tree) {
            if (!entry.second) continue;
            std::string now;
            const Type type = entry.second->get_object_type();
            if (type == Type::OutputString) {
                now = "string " + std::to_string(entry.first) + " " +
                      std::static_pointer_cast<isobus::OutputString>(entry.second)->displayed_value(tree);
                for (char& c : now)
                    if (c == '\n' || c == '\r') c = '|';
            } else if (type == Type::NumberVariable) {
                now = "number " + std::to_string(entry.first) + " " +
                      std::to_string(std::static_pointer_cast<isobus::NumberVariable>(entry.second)->get_value());
            } else if (type == Type::OutputNumber) {
                now = "number " + std::to_string(entry.first) + " " +
                      std::to_string(std::static_pointer_cast<isobus::OutputNumber>(entry.second)->get_value());
            } else {
                continue;
            }
            std::string& last = last_[entry.first];
            if (now != last) {
                last = now;
                std::printf("%s\n", now.c_str());
            }
        }
    }

    std::map<std::uint16_t, std::string> last_;
    std::uint16_t mask_ = 0xFFFF;
    bool reportedBad_ = false;
};

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: isobus_vt INTERFACE [IDENTITY]\n");
        return 2;
    }
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
    name.set_device_class(0);
    name.set_function_code(static_cast<std::uint8_t>(isobus::NAME::Function::VirtualTerminal));
    name.set_identity_number(argc > 2 ? static_cast<std::uint32_t>(std::atoi(argv[2])) : 3);
    name.set_manufacturer_code(1407);
    auto ecu = isobus::CANNetworkManager::CANNetwork.create_internal_control_function(name, 0, 0x26);

    Terminal terminal(ecu);
    terminal.initialize();

    std::mutex mu;
    std::vector<std::string> lines;
    std::thread input([&] {
        std::string line;
        while (std::getline(std::cin, line)) {
            std::lock_guard<std::mutex> l(mu);
            lines.push_back(line);
        }
    });
    input.detach();

    while (running) {
        {
            std::lock_guard<std::mutex> l(mu);
            for (const auto& line : lines) terminal.command(line);
            lines.clear();
        }
        terminal.step();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // The input thread is still in getline(), and the server's destructor waits on the stack:
    // leave at once.
    std::fflush(stdout);
    std::_Exit(0);
}
