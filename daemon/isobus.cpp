// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors
//
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
#include "isobus/isobus/isobus_speed_distance_messages.hpp"
#include "isobus/isobus/nmea2000_message_interface.hpp"
#include "settings.hpp"         // daemon/isobus_compat: where the task controller keeps files
#include "task_controller.hpp"  // third_party/aog-taskcontroller
#undef LOG_CRITICAL
#undef LOG_ERROR
#undef LOG_WARNING
#undef LOG_INFO
#undef LOG_DEBUG
#endif

#include "isobus.h"
#include "isobus_vt.h"

#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

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
    shared_.isoTcRunning.store(status_.state == IsobusStatus::State::Claimed, std::memory_order_relaxed);
    shared_.isoTcControl.store(status_.sectionControl, std::memory_order_relaxed);
    shared_.isoTcSections.store(status_.sections, std::memory_order_relaxed);
    shared_.isoTcActual.store(status_.actual, std::memory_order_relaxed);
    shared_.setIsobus(status_);
}

void Isobus::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

#if !AOW_ISOBUS

void Isobus::start(const std::string& interface, uint8_t channel, const std::string&, bool) {
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

// True if some other device on the bus is a tractor ECU.
bool busHasTractorEcu() {
    for (const auto& cf : isobus::CANNetworkManager::CANNetwork.get_control_functions(false)) {
        if (!cf || !cf->get_address_valid() || cf->get_type() == isobus::ControlFunction::Type::Internal) continue;
        if (cf->get_NAME().get_function_code() == static_cast<std::uint8_t>(isobus::NAME::Function::TractorECU))
            return true;
    }
    return false;
}

}  // namespace

void Isobus::start(const std::string& interface, uint8_t channel, const std::string& directory, bool tractorEcu,
                   bool terminal) {
    interface_ = interface;
    directory_ = directory;
    tractorEcu_ = tractorEcu;
    terminal_ = terminal;
    status_ = {};
    status_.terminal = terminal ? IsobusStatus::Terminal::Waiting : IsobusStatus::Terminal::Off;
    status_.tecu = tractorEcu ? IsobusStatus::Tecu::Waiting : IsobusStatus::Tecu::Off;
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
    std::unique_ptr<IsobusVt> vt;  // the status screen on a terminal

    // The tractor ECU role (as AOG-TaskController's, which announces a class 1 tractor ECU and
    // sends speed). Unlike upstream it is never started on a bus that has a tractor ECU, and
    // it gives way if one turns up: its address, 240, is fixed, and taking it from the
    // tractor's own would leave the implement with our speed in place of the tractor's.
    isobus::NAME tecuName = name;
    tecuName.set_function_code(static_cast<std::uint8_t>(isobus::NAME::Function::TractorECU));
    tecuName.set_arbitrary_address_capable(false);
    const std::uint8_t tecuAddress = isobus::preferred_addresses::IndustryGroup2::TractorECU;
    std::shared_ptr<isobus::InternalControlFunction> tecu;
    std::unique_ptr<isobus::ControlFunctionFunctionalities> tecuFunctionalities;
    std::unique_ptr<isobus::SpeedMessagesInterface> speedMessages;
    std::unique_ptr<isobus::NMEA2000MessageInterface> nmea;
    auto tecuAt = Clock::now(), lastTecuCheck = tecuAt, lastJ1939Speed = tecuAt, lastDistance = tecuAt;
    auto lastSpeedPublish = tecuAt;
    bool tecuClaimed = false;
    double distanceMm = 0;
    std::uint8_t nmeaSequence = 0;
    auto setTecu = [this](IsobusStatus::Tecu t) {
        if (status_.tecu == t) return;
        status_.tecu = t;
        if (t != IsobusStatus::Tecu::Active) status_.speedMmS = 0;
        publish();
    };
    auto dropTecu = [&] {
        nmea.reset();
        speedMessages.reset();
        tecuFunctionalities.reset();
        if (tecu) isobus::CANNetworkManager::CANNetwork.deactivate_control_function(tecu);
        tecu.reset();
        tecuClaimed = false;
    };

    // The stack's own thread handles the bus. This loop watches the address claim, runs the task
    // controller's periodic work and passes section states each way.
    const auto started = Clock::now();
    auto claimedAt = started, lastStatusMessage = started, lastSnapshot = started;
    auto lastReopen = started;
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

        // --- The stack's SocketCAN driver closes its socket when the interface goes down (the CAN
        // watch restarting a stuck channel, or someone using "ip link"), and never opens it
        // again. Its receive thread keeps looking, so opening it here is all it takes.
        if (!driver->get_is_valid() && elapsed(lastReopen, 1000)) {
            lastReopen = Clock::now();
            driver->open();
            if (driver->get_is_valid()) LOG_NOTICE(Can, "ISOBUS: %s is back: reopened", interface_.c_str());
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

            // The app's side, ahead of the slower reading below: a setpoint goes out on the pass
            // that sees it. Setpoints are only sent in auto mode, so: switching off, all
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
        }

        // --- The status screen, once the task controller is up.
        if (terminal_ && server) {
            if (!vt)
                vt = std::make_unique<IsobusVt>(tc, [this](bool on) {
                    // From the screen's settings page: saved by the web thread, used at the next start.
                    aow::Settings s = shared_.settings();
                    s.isobus.tractorEcu = on;
                    shared_.setSettings(s);
                    shared_.requestSave();
                });
            IsobusVt::View view;
            view.status = status_;
            view.implementName = implementName;
            view.appConnected = shared_.isoAppLink.load(std::memory_order_relaxed);
            view.speedMmS = shared_.isoSpeedMmS.load(std::memory_order_relaxed);
            view.xteMm = shared_.isoXteMm.load(std::memory_order_relaxed);
            view.distanceMm = static_cast<uint32_t>(distanceMm);
            view.tractorEcuSetting = tractorEcu_;
            const uint32_t ip = shared_.ownAddresses[0].load(std::memory_order_relaxed);
            if (ip) {
                char text[16];
                std::snprintf(text, sizeof text, "%u.%u.%u.%u", ip & 255, (ip >> 8) & 255, (ip >> 16) & 255, ip >> 24);  // network order
                view.ip = text;
            }
            const auto now = vt->update(view) ? IsobusStatus::Terminal::Connected : IsobusStatus::Terminal::Waiting;
            if (now != status_.terminal) {
                status_.terminal = now;
                publish();
            }
        }

        // --- The tractor ECU role.
        if (tractorEcu_ && elapsed(lastTecuCheck, 200)) {
            lastTecuCheck = Clock::now();
            const bool other = busHasTractorEcu();
            if (other && status_.tecu != IsobusStatus::Tecu::Tractor) {
                if (tecu) LOG_WARNING(Can, "ISOBUS: a tractor ECU appeared on the bus: ours stops");
                else LOG_NOTICE(Can, "ISOBUS: the bus has a tractor ECU: not standing in for one");
                dropTecu();
                setTecu(IsobusStatus::Tecu::Tractor);
            } else if (!other && !tecu && status_.tecu == IsobusStatus::Tecu::Waiting && server &&
                       elapsed(claimedAt, 2000)) {
                // Two seconds on the bus and no tractor ECU has answered: stand in.
                tecu = isobus::CANNetworkManager::CANNetwork.create_internal_control_function(tecuName, 0, tecuAddress);
                tecuAt = Clock::now();
            } else if (tecu && !tecuClaimed && tecu->get_address_valid() && tecu->get_address() == tecuAddress) {
                tecuClaimed = true;
                tecuAt = Clock::now();
            } else if (tecu && !tecuClaimed && elapsed(tecuAt, 5000)) {
                LOG_ERROR(Can, "ISOBUS: could not claim address %u as the tractor ECU", tecuAddress);
                dropTecu();
                setTecu(IsobusStatus::Tecu::Failed);
            } else if (tecu && tecuClaimed && !tecu->get_address_valid()) {
                LOG_WARNING(Can, "ISOBUS: lost the tractor ECU's address %u", tecuAddress);
                dropTecu();
                setTecu(IsobusStatus::Tecu::Failed);
            } else if (tecuClaimed && !speedMessages && elapsed(tecuAt, 250)) {
                using F = isobus::ControlFunctionFunctionalities;
                tecuFunctionalities = std::make_unique<F>(tecu);
                tecuFunctionalities->set_functionality_is_supported(F::Functionalities::BasicTractorECUServer, 2, true);
                tecuFunctionalities->set_basic_tractor_ECU_server_option_state(F::BasicTractorECUOptions::Class1NoOptions, true);
                speedMessages = std::make_unique<isobus::SpeedMessagesInterface>(tecu, true, true, true, false);
                speedMessages->initialize();
                auto& wheel = speedMessages->wheelBasedSpeedTransmitData;
                wheel.set_implement_start_stop_operations_state(isobus::WheelBasedMachineSpeedData::ImplementStartStopOperations::NotAvailable);
                wheel.set_key_switch_state(isobus::WheelBasedMachineSpeedData::KeySwitchState::NotAvailable);
                wheel.set_operator_direction_reversed_state(isobus::WheelBasedMachineSpeedData::OperatorDirectionReversed::NotAvailable);
                speedMessages->machineSelectedSpeedTransmitData.set_speed_source(isobus::MachineSelectedSpeedData::SpeedSource::NavigationBasedSpeed);
                nmea = std::make_unique<isobus::NMEA2000MessageInterface>(tecu, true, false, false, false, false, false, false);
                nmea->initialize();
                lastDistance = Clock::now();
                LOG_NOTICE(Can, "ISOBUS: standing in as the tractor ECU at address %u (speed on the bus)", tecuAddress);
                setTecu(IsobusStatus::Tecu::Active);
            }
        }
        if (speedMessages) {
            // Speed, forward: the app's PGN 254 carries no direction. Distance is summed here.
            const std::int32_t mmS = shared_.isoSpeedMmS.load(std::memory_order_relaxed);
            const auto speed = static_cast<std::uint16_t>(mmS < 0 ? 0 : (mmS > 65000 ? 65000 : mmS));
            const auto now = Clock::now();
            distanceMm += speed * std::chrono::duration<double>(now - lastDistance).count();
            lastDistance = now;
            const auto distance = static_cast<std::uint32_t>(distanceMm);
            auto& ground = speedMessages->groundBasedSpeedTransmitData;
            auto& wheel = speedMessages->wheelBasedSpeedTransmitData;
            auto& selected = speedMessages->machineSelectedSpeedTransmitData;
            ground.set_machine_direction_of_travel(isobus::MachineDirection::Forward);
            wheel.set_machine_direction_of_travel(isobus::MachineDirection::Forward);
            selected.set_machine_direction_of_travel(isobus::MachineDirection::Forward);
            ground.set_machine_speed(speed);
            wheel.set_machine_speed(speed);
            selected.set_machine_speed(speed);
            ground.set_machine_distance(distance);
            wheel.set_machine_distance(distance);
            selected.set_machine_distance(distance);
            auto& cogSog = nmea->get_cog_sog_transmit_message();
            if (cogSog.set_speed_over_ground(static_cast<std::uint16_t>(speed / 10))) cogSog.set_sequence_id(nmeaSequence++);
            cogSog.set_course_over_ground(0);
            cogSog.set_course_over_ground_reference(
                isobus::NMEA2000Messages::CourseOverGroundSpeedOverGroundRapidUpdate::CourseOverGroundReference::NotApplicableOrNull);
            tecuFunctionalities->update();
            speedMessages->update();
            nmea->update();
            if (elapsed(lastJ1939Speed, 100)) {  // J1939 PGN 65256, as upstream: speed in km/h * 256
                lastJ1939Speed = now;
                const auto kmh256 = static_cast<std::uint16_t>((static_cast<std::uint32_t>(speed) * 576u + 312u) / 625u);
                const std::array<std::uint8_t, 8> data = {0xFF, 0xFF, static_cast<std::uint8_t>(kmh256 & 0xFF),
                                                          static_cast<std::uint8_t>(kmh256 >> 8), 0xFF, 0xFF, 0xFF, 0xFF};
                isobus::CANNetworkManager::CANNetwork.send_can_message(0xFEE8, data.data(), data.size(), tecu);
            }
            if (mmS != status_.speedMmS && elapsed(lastSpeedPublish, 500)) {
                lastSpeedPublish = now;
                status_.speedMmS = mmS;
                publish();
            }
        }

        // 10 ms, or at once when the control thread has new section bits.
        pollfd wake = {shared_.isoWakeFd(), POLLIN, 0};
        if (poll(&wake, 1, 10) > 0) {
            uint64_t n;
            (void)!read(wake.fd, &n, sizeof n);
        }
    }

    vt.reset();
    dropTecu();
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
