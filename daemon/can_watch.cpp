#include "can_watch.h"

#include <pthread.h>
#include <sched.h>

#include <ArduinoJson.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>

#include "aow/can_stall.h"
#include "aow/log.h"

namespace aow {

namespace {

// Runs a command and returns what it printed. "" if it could not be run.
std::string output(const std::string& command) {
    std::string out;
    // tc lives in /usr/sbin, which a user's PATH may lack (systemd's has it).
    FILE* p = popen(("PATH=\"$PATH:/usr/sbin:/sbin\" " + command).c_str(), "r");
    if (!p) return out;
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    pclose(p);
    return out;
}

uint64_t txPackets(const std::string& interface) {
    std::ifstream in("/sys/class/net/" + interface + "/statistics/tx_packets");
    uint64_t n = 0;
    in >> n;
    return n;
}

// Frames waiting in the interface's queue, from "tc". False if tc gives no answer.
bool framesQueued(const std::string& interface) {
    JsonDocument doc;
    if (deserializeJson(doc, output("tc -s -j qdisc show dev " + interface + " 2>/dev/null"))) return false;
    for (JsonVariantConst q : doc.as<JsonArrayConst>())
        if ((q["qlen"] | 0) > 0) return true;
    return false;
}

// The controller's state and transmit error counter, from "ip". False if ip gives no answer.
bool controllerState(const std::string& interface, bool& errorActive, uint16_t& txErrors) {
    JsonDocument doc;
    if (deserializeJson(doc, output("ip -details -json link show " + interface + " 2>/dev/null"))) return false;
    JsonVariantConst can = doc[0]["linkinfo"]["info_data"];
    if (can.isNull()) return false;
    errorActive = std::string(can["state"] | "") == "ERROR-ACTIVE";
    txErrors = can["berr_counter"]["tx"] | 0;
    return true;
}

}  // namespace

void CanWatch::add(int index, const std::string& interface, uint32_t bitrate) {
    // Real controllers only: vcan has no device, and nothing to recover.
    if (!std::ifstream("/sys/class/net/" + interface + "/device/uevent")) return;
    channels_.push_back({index, interface, bitrate});
}

void CanWatch::start() {
    if (channels_.empty() || thread_.joinable()) return;
    stop_ = false;
    thread_ = std::thread([this] { run(); });
}

void CanWatch::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void CanWatch::run() {
    sched_param normal{};
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &normal);  // the process starts SCHED_FIFO
    pthread_setname_np(pthread_self(), "aow-canwatch");

    std::vector<CanStallDetector> detectors(channels_.size());
    std::vector<uint64_t> lastTx(channels_.size(), 0);
    std::vector<bool> haveTx(channels_.size(), false);
    while (!stop_) {
        for (size_t i = 0; i < channels_.size() && !stop_; ++i) {
            const Channel& c = channels_[i];
            CanSample s;
            s.txPackets = txPackets(c.interface);
            // tc and ip are only asked when the cheap sign is there: nothing sent since the
            // last look. On a channel that is sending, that is never.
            const bool moved = !haveTx[i] || s.txPackets != lastTx[i];
            lastTx[i] = s.txPackets;
            haveTx[i] = true;
            if (!moved && framesQueued(c.interface)) {
                s.queued = true;
                if (!controllerState(c.interface, s.errorActive, s.txErrors)) s.queued = false;
            }
            if (!detectors[i].update(s)) continue;

            LOG_WARNING(Can, "CAN%d (%s) has frames queued and is sending none, with no bus error: restarting the interface",
                        c.index + 1, c.interface.c_str());
            char cmd[200];
            std::snprintf(cmd, sizeof cmd,
                          "ip link set %s down && ip link set %s type can bitrate %u restart-ms 100 && ip link set %s up",
                          c.interface.c_str(), c.interface.c_str(), c.bitrate, c.interface.c_str());
            if (std::system(cmd) != 0) LOG_ERROR(Can, "CAN%d (%s): the restart failed", c.index + 1, c.interface.c_str());
            shared_.canRestarts[c.index].fetch_add(1);
        }
        for (int i = 0; i < 10 && !stop_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

}  // namespace aow
