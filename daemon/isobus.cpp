// The stack's headers come first: can_stack_logger.hpp defines LOG_ERROR and friends, which are
// the daemon's logging macros too. Drop the stack's before aow/log.h defines ours.
#if AOW_ISOBUS
#include "isobus/hardware_integration/can_hardware_interface.hpp"
#include "isobus/hardware_integration/socket_can_interface.hpp"
#include "isobus/isobus/can_NAME.hpp"
#include "isobus/isobus/can_internal_control_function.hpp"
#include "isobus/isobus/can_network_manager.hpp"
#include "isobus/isobus/can_stack_logger.hpp"
#include "isobus/isobus/isobus_device_descriptor_object_pool_helpers.hpp"
#include "isobus/isobus/isobus_functionalities.hpp"
#include "isobus/isobus/isobus_preferred_addresses.hpp"
#include "settings.hpp"         // daemon/isobus_compat: where the task controller keeps files
#include "task_controller.hpp"  // third_party/aog-taskcontroller
#undef LOG_CRITICAL
#undef LOG_ERROR
#undef LOG_WARNING
#undef LOG_INFO
#undef LOG_DEBUG
#endif

#include "isobus.h"

#include <pthread.h>
#include <sched.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <optional>
#include <vector>

#include "aow/log.h"

namespace aow {

uint32_t Isobus::identityNumber() {
    std::ifstream in("/etc/machine-id");
    std::string id;
    in >> id;
    if (id.empty()) return 20;  // AOG-TaskController's fixed number
    uint32_t h = 2166136261u;   // FNV-1a
    for (char c : id) h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
    return h & 0x1FFFFF;
}

uint64_t Isobus::framesSent() const {
    std::ifstream in("/sys/class/net/" + interface_ + "/statistics/tx_packets");
    uint64_t n = 0;
    in >> n;
    return n;
}

void Isobus::publish() {
    status_.busActive = framesSent() != sentAtStart_;
    shared_.setIsobus(status_);
}

void Isobus::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

#if !AOW_ISOBUS

void Isobus::start(const std::string& interface, uint8_t channel, const std::string&) {
    interface_ = interface;
    status_.channel = channel;
    std::snprintf(status_.interface, sizeof status_.interface, "%s", interface.c_str());
    LOG_ERROR(Can, "ISOBUS is enabled in the settings but this build has no ISOBUS stack");
    status_.state = IsobusStatus::State::NotBuilt;
    publish();
}
void Isobus::run() {}

#else

namespace {

// The stack's log lines go to the daemon's logger, which queues: safe from any thread.
class LogSink : public isobus::CANStackLogger {
public:
    void sink_CAN_stack_log(LoggingLevel level, const std::string& text) override {
        switch (level) {
        case LoggingLevel::Debug: LOG_DEBUG(Can, "ISOBUS: %s", text.c_str()); break;
        case LoggingLevel::Info: LOG_INFO(Can, "ISOBUS: %s", text.c_str()); break;
        case LoggingLevel::Warning: LOG_WARNING(Can, "ISOBUS: %s", text.c_str()); break;
        default: LOG_ERROR(Can, "ISOBUS: %s", text.c_str()); break;
        }
    }
};
LogSink logSink;

using Clock = std::chrono::steady_clock;
bool elapsed(Clock::time_point since, int ms) { return Clock::now() - since >= std::chrono::milliseconds(ms); }

// ISO 11783-10 B.8.1, every 2 s to everyone. Implements connect only to a task controller that
// sends it. As AOG-TaskController's Application::send_task_controller_status_message().
void sendTaskControllerStatus(const std::shared_ptr<isobus::InternalControlFunction>& tc, bool totalsActive) {
    const std::array<std::uint8_t, 8> data = {
        0xFE,  // command 0x0E, element nibble not available
        0xFF, 0xFF, 0xFF,
        static_cast<std::uint8_t>(totalsActive ? 0x01 : 0x00),
        0x00, 0x00, 0xFF};
    isobus::CANNetworkManager::CANNetwork.send_can_message(0xCB00, data.data(), data.size(), tc);
}

// Working width in metres, summed over the sections (as AOG-TaskController's
// derive_implement_details()). 0 if the description has no widths.
float workingWidth(isobus::DeviceDescriptorObjectPool& pool) {
    std::int32_t mm = 0;
    const auto geometry = isobus::DeviceDescriptorObjectPoolHelper::get_implement_geometry(pool);
    for (const auto& boom : geometry.booms) {
        for (const auto& section : boom.sections)
            if (section.width_mm) mm += section.width_mm.get();
        for (const auto& sub : boom.subBooms) {
            if (sub.sections.empty() && sub.width_mm) mm += sub.width_mm.get();
            for (const auto& section : sub.sections)
                if (section.width_mm) mm += section.width_mm.get();
        }
    }
    return static_cast<float>(mm) / 1000.0f;
}

// Upstream's task controller, plus a note of which sections an implement has just reported on.
//
// Reading a section's reported state through upstream's get_section_actual_state() searches the
// whole description for the section's parents: 2.3 ms a call for a 64-section implement on the
// CM4. Every change to those states arrives through on_value_command(), so the sections it
// could have touched are marked here and only those are read again.
class TcServer : public MyTCServer {
public:
    using MyTCServer::MyTCServer;

    bool on_value_command(std::shared_ptr<isobus::ControlFunction> partner, std::uint16_t ddi,
                          std::uint16_t element, std::int32_t value, std::uint8_t& errorCodes) override {
        const bool handled = MyTCServer::on_value_command(partner, ddi, element, value, errorCodes);
        const auto first = static_cast<std::uint16_t>(isobus::DataDescriptionIndex::ActualCondensedWorkState1_16);
        if (ddi >= first && ddi < first + 4) {
            dirty_.fetch_or(0xFFFFull << (16 * (ddi - first)));  // the 16 sections of this message
        } else if (ddi == static_cast<std::uint16_t>(isobus::DataDescriptionIndex::ActualWorkState)) {
            dirty_.fetch_or(~0ull);  // one element's work state: it, or everything under it
        }
        return handled;
    }

    // Sections (bit 0 = section 1) reported on since the last call.
    uint64_t takeDirty() { return dirty_.exchange(0); }
    void markAllDirty() { dirty_.store(~0ull); }

private:
    std::atomic<uint64_t> dirty_{0};  // set on the stack's thread, taken on the ISOBUS thread
};

}  // namespace

void Isobus::start(const std::string& interface, uint8_t channel, const std::string& directory) {
    interface_ = interface;
    directory_ = directory;
    status_ = {};
    status_.state = IsobusStatus::State::Starting;
    status_.channel = channel;
    std::snprintf(status_.interface, sizeof status_.interface, "%s", interface.c_str());
    sentAtStart_ = framesSent();
    stop_ = false;
    publish();
    thread_ = std::thread([this] { run(); });
}

void Isobus::run() {
    // systemd starts the whole process SCHED_FIFO. This thread, and the stack's threads, which
    // it starts and which inherit from it, must not compete with the control loop.
    sched_param normal{};
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &normal);
    pthread_setname_np(pthread_self(), "aow-isobus");

    isobus::CANStackLogger::set_can_stack_logger_sink(&logSink);
    isobus::CANStackLogger::set_log_level(isobus::CANStackLogger::LoggingLevel::Info);
    ::Settings::set_directory(directory_);

    auto driver = std::make_shared<isobus::SocketCANInterface>(interface_);
    isobus::CANHardwareInterface::set_number_of_can_channels(1);
    isobus::CANHardwareInterface::assign_can_channel_frame_handler(0, driver);
    if (!isobus::CANHardwareInterface::start() || !driver->get_is_valid()) {
        LOG_ERROR(Can, "ISOBUS: could not open %s", interface_.c_str());
        isobus::CANHardwareInterface::stop();
        status_.state = IsobusStatus::State::Failed;
        publish();
        return;
    }
    // As AOG-TaskController: implement descriptions are large, send them in big bursts.
    isobus::CANNetworkManager::CANNetwork.get_configuration().set_number_of_packets_per_cts_message(255);

    // The NAME, as AOG-TaskController's but for the identity number, which is this unit's.
    isobus::NAME name(0);
    name.set_arbitrary_address_capable(true);
    name.set_industry_group(2);
    name.set_device_class(0);
    name.set_function_code(static_cast<std::uint8_t>(isobus::NAME::Function::TaskController));
    name.set_identity_number(identityNumber());
    name.set_ecu_instance(0);
    name.set_function_instance(0);
    name.set_device_class_instance(0);
    name.set_manufacturer_code(1407);
    status_.name = name.get_full_name();
    publish();

    auto tc = isobus::CANNetworkManager::CANNetwork.create_internal_control_function(
        name, 0, isobus::preferred_addresses::IndustryGroup2::TaskController_MappingComputer);
    LOG_INFO(Can, "ISOBUS on CAN%u (%s), NAME %016llX, claiming an address", status_.channel, interface_.c_str(),
             static_cast<unsigned long long>(status_.name));

    std::shared_ptr<TcServer> server;
    std::unique_ptr<isobus::ControlFunctionFunctionalities> functionalities;

    // The stack's own thread handles the bus. This loop watches the address claim, runs the task
    // controller's periodic work and passes section states each way.
    const auto started = Clock::now();
    auto claimedAt = started, lastStatusMessage = started, lastSnapshot = started;
    bool claimed = false, reportedFailure = false, busActive = false, reportedAlone = false;
    bool sectionControl = false;
    uint8_t implementSections = 0;
    uint64_t implementName = 0;
    // The implement's reported section states: only the sections TcServer marked, one a pass,
    // from a copy of the implement's state, so a burst of reports never holds up the setpoints.
    std::optional<ClientState> scan;
    uint64_t scanPending = 0;  // sections still to read from the copy
    bool scanChanged = false;  // a section read in this round differed: publish at its end
    while (!stop_) {
        // --- The address. It can be lost later to a control function with a higher-priority NAME.
        const bool valid = tc->get_address_valid();
        const uint8_t address = valid ? tc->get_address() : 0xFE;
        if (valid && (!claimed || address != status_.address)) {
            LOG_INFO(Can, "ISOBUS: claimed address %u", address);
            status_.state = IsobusStatus::State::Claimed;
            status_.address = address;
            claimedAt = Clock::now();
            reportedFailure = false;
            publish();
        } else if (!valid && claimed) {
            LOG_WARNING(Can, "ISOBUS: lost address %u, claiming another", status_.address);
            status_.state = IsobusStatus::State::Starting;
            status_.address = 0xFE;
            publish();
        } else if (!valid && !claimed && !reportedFailure && elapsed(started, 5000)) {
            LOG_ERROR(Can, "ISOBUS: no address claimed on %s (is the bus connected and at 250 kbit/s?)",
                      interface_.c_str());
            status_.state = IsobusStatus::State::Failed;
            reportedFailure = true;
            publish();
        }
        claimed = valid;

        // An unacknowledged claim is retried by the controller for as long as it takes, so this
        // turns true when the first other device appears.
        const bool active = framesSent() != sentAtStart_;
        if (active != busActive) {
            busActive = active;
            if (active && reportedAlone) LOG_INFO(Can, "ISOBUS: another device is on the bus now");
            publish();
        } else if (!active && claimed && !reportedAlone && elapsed(started, 5000)) {
            LOG_WARNING(Can, "ISOBUS: nothing on %s acknowledged the address claim: no other device on the bus",
                        interface_.c_str());
            reportedAlone = true;
        }

        // --- The task controller, once we have had an address for 250 ms (J1939-81: nothing
        // but the claim may be sent before that).
        if (!server && claimed && elapsed(claimedAt, 250)) {
            server = std::make_shared<TcServer>(tc, isobus::TaskControllerServer::TaskControllerVersion::SecondEditionDraft);
            auto& language = server->get_language_command_interface();
            language.set_language_code("en");
            language.set_country_code("US");
            server->initialize();
            server->set_task_totals_active(true);
            // What we announce (PGN 64654), as AOG-TaskController: basic and section control,
            // one boom, 64 sections; no prescription maps.
            using F = isobus::ControlFunctionFunctionalities;
            functionalities = std::make_unique<F>(tc);
            functionalities->set_functionality_is_supported(F::Functionalities::TaskControllerBasicServer, 1, true);
            functionalities->set_functionality_is_supported(F::Functionalities::TaskControllerGeoServer, 1, false);
            functionalities->set_task_controller_geo_server_option_state(
                F::TaskControllerGeoServerOptions::PolygonBasedPrescriptionMapsAreSupported, false);
            functionalities->set_functionality_is_supported(F::Functionalities::TaskControllerSectionControlServer, 1, true);
            functionalities->set_task_controller_section_control_server_option_state(1, 64);
            LOG_INFO(Can, "ISOBUS: task controller running (section control, 1 boom, 64 sections)");
        }
        if (server) {
            server->request_measurement_commands();
            server->update();
            functionalities->update();
            if (claimed && elapsed(lastStatusMessage, 2000)) {
                lastStatusMessage = Clock::now();
                sendTaskControllerStatus(tc, server->get_task_totals_active());
            }

            // What the implement has, and reports. Ten times a second: get_clients() copies.
            bool newImplement = false;
            if (elapsed(lastSnapshot, 100)) {
                lastSnapshot = Clock::now();
                auto clients = server->get_clients();
                IsobusStatus s = status_;
                s.clients = static_cast<uint8_t>(clients.size() > 255 ? 255 : clients.size());
                s.sections = 0;
                s.setpoint = 0;
                uint64_t seenName = 0;
                for (auto& client : clients) {
                    ClientState& state = client.second;
                    if (state.get_number_of_sections() == 0) continue;  // a tractor, a terminal
                    s.sections = state.get_number_of_sections();
                    seenName = client.first->get_NAME().get_full_name();
                    for (uint8_t i = 0; i < s.sections && i < 64; ++i) {
                        if (state.get_section_setpoint_state(i) == SectionState::ON) s.setpoint |= 1ull << i;
                    }
                    if (seenName != implementName || s.sections != implementSections) {
                        newImplement = true;
                        s.widthM = workingWidth(state.get_pool());
                        s.implement[0] = '\0';
                        if (auto device = state.get_pool().get_object_by_index(0))
                            std::snprintf(s.implement, sizeof s.implement, "%s", device->get_designator().c_str());
                        LOG_NOTICE(Can, "ISOBUS: implement \"%s\", %u sections, %.2f m", s.implement, s.sections,
                                   static_cast<double>(s.widthM));
                    }
                    if (newImplement) server->markAllDirty();
                    break;  // the first implement with sections is the one the app drives
                }
                if (!seenName && implementName) {
                    LOG_WARNING(Can, "ISOBUS: implement \"%s\" gone", status_.implement);
                    s.implement[0] = '\0';
                    s.widthM = 0;
                }
                if (!seenName) {
                    s.actual = 0;
                    scan.reset();
                    scanPending = 0;
                }
                implementName = seenName;
                implementSections = s.sections;
                s.sectionControl = sectionControl && s.sections > 0;
                const bool changed = s.clients != status_.clients || s.sections != status_.sections ||
                                     s.setpoint != status_.setpoint || s.actual != status_.actual ||
                                     s.sectionControl != status_.sectionControl || newImplement;
                status_ = s;
                if (changed) publish();
            }

            // Reported states. With nothing in hand, take what has been marked since and a fresh
            // copy of the implement's state; then one marked section a pass.
            if (!scanPending && implementName) {
                scanPending = server->takeDirty();
                if (implementSections < 64) scanPending &= (1ull << implementSections) - 1;
                scan.reset();
                if (scanPending) {
                    for (auto& client : server->get_clients())
                        if (client.first->get_NAME().get_full_name() == implementName) scan = client.second;
                    if (!scan) scanPending = 0;
                }
            }
            if (scanPending) {
                const int i = __builtin_ctzll(scanPending);
                scanPending &= scanPending - 1;
                const uint64_t bit = 1ull << i;
                const bool on = scan->get_section_actual_state(static_cast<uint8_t>(i)) == SectionState::ON;
                if (on != ((status_.actual & bit) != 0)) {
                    status_.actual ^= bit;
                    if (!scanPending) publish();
                    else scanChanged = true;
                } else if (!scanPending && scanChanged) {
                    publish();
                }
                if (!scanPending) scanChanged = false;
            }

            // The app's side. Setpoints are only sent in auto mode, so: switching off, all
            // sections off first and then leave auto; switching on, enter auto first. A new
            // implement is told the mode again.
            const bool wanted = shared_.isoSectionControl.load(std::memory_order_relaxed);
            const uint64_t bits = wanted ? shared_.isoSections.load(std::memory_order_relaxed) : 0;
            std::vector<bool> states(64);
            for (int i = 0; i < 64; ++i) states[i] = (bits >> i) & 1;
            if (!wanted && sectionControl) {
                server->update_section_states(states);
                server->update_section_control_enabled(false);
                LOG_INFO(Can, "ISOBUS: section control off");
            } else if (wanted && (!sectionControl || newImplement)) {
                server->update_section_control_enabled(true);
                if (!sectionControl) LOG_INFO(Can, "ISOBUS: section control on");
            }
            sectionControl = wanted;
            if (wanted) server->update_section_states(states);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (server) {
        // Leaving: sections off while the bus is still up.
        std::vector<bool> off(64, false);
        server->update_section_states(off);
        server->update_section_control_enabled(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        server->terminate();
    }
    isobus::CANNetworkManager::CANNetwork.deactivate_control_function(tc);
    isobus::CANHardwareInterface::stop();
    isobus::CANStackLogger::set_can_stack_logger_sink(nullptr);
}

#endif

}  // namespace aow
