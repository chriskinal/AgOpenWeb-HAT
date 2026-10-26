// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors

#include "serial_hub.h"

#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "aow/log.h"

namespace aow {

namespace {

using Kind = SerialDetector::Kind;

constexpr uint64_t kInUseUs = 2'000'000;     // a receiver or IMU counts as sending within this
constexpr uint64_t kUsbScanUs = 1'000'000;
constexpr uint64_t kReopenUs = 1'000'000;
constexpr unsigned kOutDefaultBaud = 115200;  // an output on a port left on auto

std::string realDevice(const std::string& path) {
    char buf[PATH_MAX];
    return realpath(path.c_str(), buf) ? std::string(buf) : std::string();
}

const char* what(Kind k) {
    switch (k) {
    case Kind::Gnss: return "GNSS receiver";
    case Kind::Radio: return "RTK radio (RTCM)";
    case Kind::Imu: return "IMU";
    case Kind::Unknown: break;
    }
    return "nothing recognised";
}

}  // namespace

bool SerialHub::Channel::sending(uint64_t nowUs, uint64_t within) const {
    return port.fd() >= 0 && det.lastValidUs() && nowUs - det.lastValidUs() <= within;
}

SerialHub::SerialHub(const Config& c, GnssLink& gnss, imu::Imu& imu, hal::Clock& clock)
    : gnss_(gnss), imu_(imu), clock_(clock), usb_(c.usb) {
    const uint64_t now = clock_.nowUs();
    for (const std::string& p : {c.onboard, c.rs232[0], c.rs232[1], c.radio, c.imu, c.keya})
        if (!p.empty()) taken_.push_back(realDevice(p));

    if (!c.onboard.empty()) {
        Channel& o = add(Channel::Place::Onboard, "Onboard", c.onboard);
        o.fixed = Kind::Gnss;
        o.fixedBaud = c.onboardBaud;
        o.det.reset(c.onboardBaud, now);
        if (!open(o)) LOG_ERROR(Gnss, "Onboard receiver %s not opened: tried again every second", c.onboard.c_str());
        gnssIn_ = &o;  // from the start, so its first sentences count
    }
    for (int i = 0; i < 2; ++i) {
        if (c.rs232[i].empty()) continue;
        if (realDevice(c.rs232[i]) == realDevice(c.keya) && !c.keya.empty()) {
            LOG_INFO(Gnss, "Port %d: the Keya motor's", i + 1);
            continue;
        }
        char name[16];
        std::snprintf(name, sizeof name, "Port %d", i + 1);
        Channel& p = add(Channel::Place::Rs232, name, c.rs232[i]);
        p.index = i;
        open(p);
    }
    if (!c.radio.empty()) {
        Channel& r = add(Channel::Place::Fixed, c.radio, c.radio);
        r.fixed = Kind::Radio;
        r.fixedBaud = c.radioBaud;
        r.det.reset(c.radioBaud, now);
        if (open(r)) LOG_INFO(Gnss, "Radio RTCM on %s at %u baud", c.radio.c_str(), c.radioBaud);
    }
    if (!c.imu.empty()) {
        Channel& m = add(Channel::Place::Fixed, c.imu, c.imu);
        m.fixed = Kind::Imu;
        m.fixedBaud = 115200;
        m.det.reset(115200, now);
        if (open(m)) LOG_INFO(Imu, "IMU port %s open, waiting for a BNO085 or TM171", c.imu.c_str());
        else LOG_WARNING(Imu, "IMU port %s not there yet: opened when it appears", c.imu.c_str());
    }
    scanUsb();
}

SerialHub::Channel& SerialHub::add(Channel::Place place, const std::string& name, const std::string& path) {
    channels_.push_back(std::make_unique<Channel>());
    Channel& c = *channels_.back();
    c.place = place;
    c.name = name;
    c.path = path;
    c.det.reset(0, clock_.nowUs());
    return c;
}

bool SerialHub::open(Channel& c) {
    if (access(c.path.c_str(), F_OK) != 0) return false;
    return c.port.open(c.path.c_str(), c.output() ? outBaud(c) : c.det.baud());
}

unsigned SerialHub::outBaud(const Channel& c) const {
    return c.index >= 0 && ports_.rate[c.index] ? ports_.rate[c.index] : kOutDefaultBaud;
}

void SerialHub::apply(const Settings::Ports& p) {
    using P = Settings::Ports;
    ports_ = p;
    const uint64_t now = clock_.nowUs();
    for (auto& cp : channels_) {
        Channel& c = *cp;
        if (c.place != Channel::Place::Rs232 && c.place != Channel::Place::Usb) continue;
        const uint8_t here = c.place == Channel::Place::Usb ? P::kUsb : c.index == 0 ? P::kPort1 : P::kPort2;
        const bool nmea = p.nmeaOut == here && (here != P::kUsb || c.name == p.nmeaOutUsb);
        const bool rtcm = p.rtcmOut == here && (here != P::kUsb || c.name == p.rtcmOutUsb);
        const bool wasOutput = c.output();
        c.nmeaOut = nmea;
        c.rtcmOut = rtcm;
        if (c.output()) {
            const unsigned baud = outBaud(c);
            if (!wasOutput) LOG_INFO(Gnss, "%s: %s%s%s out at %u baud", c.name.c_str(), nmea ? "NMEA" : "", nmea && rtcm ? " and " : "",
                                     rtcm ? "RTCM" : "", baud);
            if (c.port.fd() >= 0) c.port.setBaud(baud);
            else open(c);
            continue;
        }
        // Listening: back from an output, or a new pinned rate (or auto), starts detection again.
        if (c.place == Channel::Place::Rs232) c.listenPinned = p.rate[c.index];
        const unsigned heldAt = c.det.autoBaud() ? 0 : c.det.baud();
        if (wasOutput || c.listenPinned != heldAt) {
            c.det.reset(c.listenPinned, now);
            if (c.port.fd() >= 0) c.port.setBaud(c.det.baud());
            else open(c);
        }
    }
    route();
}

void SerialHub::fds(std::vector<pollfd>& out) {
    polled_.clear();
    for (auto& cp : channels_) {
        Channel& c = *cp;
        if (c.port.fd() < 0 || c.output() || c.gone) continue;
        if (lent_ && c.place == Channel::Place::Onboard) continue;  // the UM98x page has it
        out.push_back({c.port.fd(), POLLIN, 0});
        polled_.push_back(&c);
    }
}

void SerialHub::handle(const pollfd* fds, size_t n) {
    for (size_t i = 0; i < n && i < polled_.size(); ++i) {
        Channel& c = *polled_[i];
        if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            LOG_WARNING(Gnss, "%s went away", c.name.c_str());
            c.port.close();
            c.nextOpenUs = clock_.nowUs() + kReopenUs;
            if (c.place == Channel::Place::Usb) c.gone = true;
            continue;
        }
        if (fds[i].revents) read(c);
    }
}

void SerialHub::read(Channel& c) {
    uint8_t buf[1024];
    int n;
    while ((n = c.port.read(buf, sizeof buf)) > 0) {
        const uint64_t now = clock_.nowUs();
        const Kind before = c.kind();
        c.det.feed(buf, static_cast<size_t>(n), now);
        if (c.kind() != before) announce(c, before);
        if (&c == gnssIn_) gnss_.feedReceiver(buf, static_cast<size_t>(n));
        else if (c.kind() == Kind::Radio && !lent_) gnss_.forwardRadio(buf, static_cast<size_t>(n));
        if (&c == imuIn_ || (!imuIn_ && c.kind() == Kind::Imu)) {
            imuIn_ = &c;
            imu_.feed(buf, static_cast<size_t>(n), now);
        }
    }
}

void SerialHub::announce(Channel& c, Kind before) {
    if (c.fixed != Kind::Unknown) return;  // taken as such: said at open
    if (c.kind() == Kind::Unknown)
        LOG_INFO(Gnss, "%s: no %s for %u s, listening again", c.name.c_str(), what(before),
                 static_cast<unsigned>(SerialDetector::kLostUs / 1'000'000));
    else
        LOG_INFO(Gnss, "%s: %s at %u baud", c.name.c_str(), what(c.kind()), c.det.baud());
    route();
}

void SerialHub::tick() {
    const uint64_t now = clock_.nowUs();
    bool changed = false;
    for (auto& cp : channels_) {
        Channel& c = *cp;
        if (c.port.fd() < 0) {
            if (c.gone || now < c.nextOpenUs) continue;
            c.nextOpenUs = now + kReopenUs;
            if (open(c)) changed = true;
            continue;
        }
        if (c.output()) continue;
        const Kind before = c.kind();
        if (c.det.tick(now)) c.port.setBaud(c.det.baud());
        if (c.kind() != before) announce(c, before);
    }
    // Unplugged USB devices go.
    const size_t was = channels_.size();
    for (auto& cp : channels_)
        if (cp->gone && (cp.get() == gnssIn_ || cp.get() == imuIn_)) {
            if (cp.get() == gnssIn_) gnssIn_ = nullptr;
            if (cp.get() == imuIn_) imuIn_ = nullptr;
        }
    channels_.erase(std::remove_if(channels_.begin(), channels_.end(), [](const auto& cp) { return cp->gone; }),
                    channels_.end());
    if (channels_.size() != was) changed = true;
    if (now >= nextUsbScanUs_) {
        nextUsbScanUs_ = now + kUsbScanUs;
        const size_t before = channels_.size();
        scanUsb();
        if (channels_.size() != before) changed = true;
    }
    route();
    if (changed) apply(ports_);
}

void SerialHub::scanUsb() {
    if (!usb_) return;
    const std::string dir = "/dev/serial/by-id";
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> names;
    while (const dirent* e = readdir(d))
        if (e->d_name[0] != '.') names.push_back(e->d_name);
    closedir(d);
    std::sort(names.begin(), names.end());
    for (const std::string& name : names) {
        const std::string path = dir + "/" + name;
        const std::string real = realDevice(path);
        const bool acm = real.rfind("/dev/ttyACM", 0) == 0;
        if (!acm && real.rfind("/dev/ttyUSB", 0) != 0) continue;
        if (std::find(taken_.begin(), taken_.end(), real) != taken_.end()) continue;
        if (std::any_of(channels_.begin(), channels_.end(), [&](const auto& c) { return c->path == path; })) continue;
        Channel& u = add(Channel::Place::Usb, name, path);
        // A USB CDC device (ttyACM) ignores the rate; a USB to serial adapter (ttyUSB) needs it.
        u.listenPinned = acm ? 115200 : 0;
        u.det.reset(u.listenPinned, clock_.nowUs());
        LOG_INFO(Gnss, "USB %s: plugged in, listening", name.c_str());
        apply(ports_);  // an output may be waiting for it
        if (!u.output()) open(u);
    }
}

void SerialHub::route() {
    const uint64_t now = clock_.nowUs();
    // The receiver: onboard while it sends, else the first other one sending; kept otherwise.
    Channel* onboard = nullptr;
    Channel* other = nullptr;
    for (auto& cp : channels_) {
        Channel& c = *cp;
        if (c.output() || c.gone || c.kind() != Kind::Gnss) continue;
        if (c.place == Channel::Place::Onboard) onboard = &c;
        else if (!other && c.sending(now, kInUseUs)) other = &c;
    }
    Channel* want = gnssIn_;
    if (onboard && onboard->sending(now, kInUseUs)) want = onboard;
    else if (!(gnssIn_ && gnssIn_->sending(now, kInUseUs)) && other) want = other;
    if (gnssIn_ && (gnssIn_->gone || gnssIn_->output())) want = other ? other : onboard;
    if (want != gnssIn_) {
        gnssIn_ = want;
        if (want) LOG_NOTICE(Gnss, "GNSS receiver in use: %s", want->name.c_str());
    }
    gnss_.setReceiver(gnssIn_ && !lent_ ? &gnssIn_->port : nullptr);

    // The IMU: the one feeding it while it sends, else the first other one sending.
    if (imuIn_ && (imuIn_->gone || imuIn_->kind() != Kind::Imu)) imuIn_ = nullptr;
    if (!imuIn_ || !imuIn_->sending(now, kInUseUs))
        for (auto& cp : channels_)
            if (cp.get() != imuIn_ && !cp->gone && !cp->output() && cp->kind() == Kind::Imu && cp->sending(now, kInUseUs)) {
                imuIn_ = cp.get();
                break;
            }

    // Outputs.
    Channel* nmea = nullptr;
    Channel* rtcm = nullptr;
    for (auto& cp : channels_) {
        if (cp->port.fd() < 0 || cp->gone) continue;
        if (cp->nmeaOut && !nmea) nmea = cp.get();
        if (cp->rtcmOut && !rtcm) rtcm = cp.get();
    }
    gnss_.setNmeaOut(nmea ? &nmea->port : nullptr, ports_.nmeaSentences, ports_.nmeaRateHz);
    gnss_.setRtcmOut(rtcm ? &rtcm->port : nullptr);
}

hal::SerialPort* SerialHub::onboardPort() {
    for (auto& cp : channels_)
        if (cp->place == Channel::Place::Onboard) return &cp->port;
    return nullptr;
}

const std::string& SerialHub::gnssName() const {
    static const std::string none;
    return gnssIn_ ? gnssIn_->name : none;
}

const std::string& SerialHub::imuName() const {
    static const std::string none;
    return imuIn_ ? imuIn_->name : none;
}

void SerialHub::status(Status& st) const {
    const uint64_t now = clock_.nowUs();
    st.serialCount = 0;
    for (const auto& cp : channels_) {
        if (st.serialCount >= Status::kSerialChannels) break;
        const Channel& c = *cp;
        Status::Serial& v = st.serial[st.serialCount++];
        std::snprintf(v.name, sizeof v.name, "%s", c.name.c_str());
        v.place = c.place == Channel::Place::Onboard ? "onboard"
                : c.place == Channel::Place::Usb     ? "usb"
                : c.place == Channel::Place::Fixed   ? "fixed"
                : c.index == 0                     ? "port1"
                                                   : "port2";
        v.kind = kindName(c.kind());
        v.open = c.port.fd() >= 0;
        v.nmeaOut = c.nmeaOut;
        v.rtcmOut = c.rtcmOut;
        v.inUse = &c == gnssIn_ || &c == imuIn_;
        v.baud = c.output() ? outBaud(c) : c.det.baud();
        v.autoBaud = !c.output() && c.det.autoBaud() && c.fixed == Kind::Unknown;
        const PortMonitor& m = c.det.monitor();
        v.bytesIn = m.bytes();
        v.msSinceIn = m.lastUs() ? static_cast<uint32_t>((now - m.lastUs()) / 1000) : UINT32_MAX;
        v.lineCount = m.lineCount();
        for (int l = 0; l < m.lineCount(); ++l) std::memcpy(v.lines[l], m.line(l), sizeof v.lines[l]);
        v.rtcmCount = m.rtcmTypes(now, v.rtcm, PortMonitor::kRtcmTypes);
    }
    std::snprintf(st.gnssPort, sizeof st.gnssPort, "%s", gnssIn_ && gnssIn_->port.fd() >= 0 ? gnssIn_->path.c_str() : "");
    std::snprintf(st.imuPort, sizeof st.imuPort, "%s", imuIn_ && imuIn_->port.fd() >= 0 ? imuIn_->path.c_str() : "");
}

}  // namespace aow
