// The stack's headers come first: can_stack_logger.hpp defines LOG_ERROR and friends, which are
// the daemon's logging macros too. Drop the stack's before aow/log.h defines ours.
#if AOW_ISOBUS
#include "isobus/hardware_integration/can_hardware_interface.hpp"
#include "isobus/hardware_integration/socket_can_interface.hpp"
#include "isobus/isobus/can_NAME.hpp"
#include "isobus/isobus/can_internal_control_function.hpp"
#include "isobus/isobus/can_network_manager.hpp"
#include "isobus/isobus/can_stack_logger.hpp"
#include "isobus/isobus/isobus_preferred_addresses.hpp"
#undef LOG_CRITICAL
#undef LOG_ERROR
#undef LOG_WARNING
#undef LOG_INFO
#undef LOG_DEBUG
#endif

#include "isobus.h"

#include <pthread.h>
#include <sched.h>

#include <chrono>
#include <cstdio>
#include <fstream>

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

void Isobus::publish(IsobusStatus::State state, uint8_t address) {
    IsobusStatus s;
    s.busActive = framesSent() != sentAtStart_;
    s.state = state;
    s.channel = channel_;
    s.address = address;
    s.name = name_;
    std::snprintf(s.interface, sizeof s.interface, "%s", interface_.c_str());
    shared_.setIsobus(s);
}

void Isobus::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

#if !AOW_ISOBUS

void Isobus::start(const std::string& interface, uint8_t channel) {
    interface_ = interface;
    channel_ = channel;
    LOG_ERROR(Can, "ISOBUS is enabled in the settings but this build has no ISOBUS stack");
    publish(IsobusStatus::State::NotBuilt);
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

}  // namespace

void Isobus::start(const std::string& interface, uint8_t channel) {
    interface_ = interface;
    channel_ = channel;
    sentAtStart_ = framesSent();
    stop_ = false;
    publish(IsobusStatus::State::Starting);
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

    auto driver = std::make_shared<isobus::SocketCANInterface>(interface_);
    isobus::CANHardwareInterface::set_number_of_can_channels(1);
    isobus::CANHardwareInterface::assign_can_channel_frame_handler(0, driver);
    if (!isobus::CANHardwareInterface::start() || !driver->get_is_valid()) {
        LOG_ERROR(Can, "ISOBUS: could not open %s", interface_.c_str());
        isobus::CANHardwareInterface::stop();
        publish(IsobusStatus::State::Failed);
        return;
    }

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
    name_ = name.get_full_name();
    publish(IsobusStatus::State::Starting);

    auto tc = isobus::CANNetworkManager::CANNetwork.create_internal_control_function(
        name, 0, isobus::preferred_addresses::IndustryGroup2::TaskController_MappingComputer);
    LOG_INFO(Can, "ISOBUS on CAN%u (%s), NAME %016llX, claiming an address", channel_, interface_.c_str(),
             static_cast<unsigned long long>(name_));

    // The stack's own thread does the work. Here: watch the claim and publish what changes. An
    // address can be lost later to a control function with a higher-priority NAME.
    using clock = std::chrono::steady_clock;
    const auto started = clock::now();
    bool claimed = false, reportedFailure = false, busActive = false, reportedAlone = false;
    uint8_t address = 0xFE;
    while (!stop_) {
        // An unacknowledged claim is retried by the controller for as long as it takes, so this
        // turns true when the first other device appears.
        const bool active = framesSent() != sentAtStart_;
        if (active != busActive) {
            busActive = active;
            if (active && reportedAlone) LOG_INFO(Can, "ISOBUS: another device is on the bus now");
            if (claimed) publish(IsobusStatus::State::Claimed, address);
        } else if (!active && claimed && !reportedAlone && clock::now() - started > std::chrono::seconds(5)) {
            LOG_WARNING(Can, "ISOBUS: nothing on %s acknowledged the address claim: no other device on the bus",
                        interface_.c_str());
            reportedAlone = true;
        }
        const bool valid = tc->get_address_valid();
        const uint8_t now = valid ? tc->get_address() : 0xFE;
        if (valid && (!claimed || now != address)) {
            LOG_INFO(Can, "ISOBUS: claimed address %u", now);
            publish(IsobusStatus::State::Claimed, now);
            reportedFailure = false;
        } else if (!valid && claimed) {
            LOG_WARNING(Can, "ISOBUS: lost address %u, claiming another", address);
            publish(IsobusStatus::State::Starting);
        } else if (!valid && !claimed && !reportedFailure && clock::now() - started > std::chrono::seconds(5)) {
            LOG_ERROR(Can, "ISOBUS: no address claimed on %s (is the bus connected and at 250 kbit/s?)",
                      interface_.c_str());
            publish(IsobusStatus::State::Failed);
            reportedFailure = true;
        }
        claimed = valid;
        address = now;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    isobus::CANNetworkManager::CANNetwork.deactivate_control_function(tc);
    isobus::CANHardwareInterface::stop();
    isobus::CANStackLogger::set_can_stack_logger_sink(nullptr);
}

#endif

}  // namespace aow
