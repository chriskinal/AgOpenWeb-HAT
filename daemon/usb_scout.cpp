#include "usb_scout.h"

#include <dirent.h>

#include <algorithm>
#include <climits>
#include <cstdlib>

#include "aow/log.h"

namespace aow {

namespace {
constexpr uint64_t kListenUs = 3'000'000;  // per device
constexpr uint64_t kRelistUs = 1'000'000;
constexpr int kSentences = 3;              // good NMEA sentences to call it a receiver
}  // namespace

std::string realDevice(const std::string& path) {
    char buf[PATH_MAX];
    return realpath(path.c_str(), buf) ? std::string(buf) : std::string();
}

void UsbScout::want(Kind k, bool on) {
    if (k == Kind::Imu) wantImu_ = on;
    if (k == Kind::Gnss) wantGnss_ = on;
}

void UsbScout::exclude(const std::string& path) {
    const std::string r = realDevice(path);
    if (!r.empty() && !isExcluded(r)) excluded_.push_back({path, r});
}

void UsbScout::release(const std::string& path) {
    // By name: an unplugged device no longer points anywhere.
    excluded_.erase(std::remove_if(excluded_.begin(), excluded_.end(), [&](const Excluded& e) { return e.name == path; }),
                    excluded_.end());
}

bool UsbScout::isExcluded(const std::string& real) const {
    return std::any_of(excluded_.begin(), excluded_.end(), [&](const Excluded& e) { return e.real == real; });
}

void UsbScout::next(uint64_t nowUs) {
    if (nextCandidate_ >= candidates_.size()) {
        if (listed_ && nowUs - listedUs_ < kRelistUs) return;
        listed_ = true;
        listedUs_ = nowUs;
        candidates_.clear();
        nextCandidate_ = 0;
        const std::string dir = "/dev/serial/by-id";
        DIR* d = opendir(dir.c_str());
        if (!d) return;
        std::vector<std::string> names;
        while (const dirent* e = readdir(d))
            if (e->d_name[0] != '.') names.push_back(dir + "/" + e->d_name);
        closedir(d);
        std::sort(names.begin(), names.end());
        for (const auto& p : names) {
            const std::string r = realDevice(p);
            if (r.empty() || isExcluded(r)) continue;
            if (r.rfind("/dev/ttyACM", 0) == 0) {
                candidates_.push_back({p, 115200});
            } else if (r.rfind("/dev/ttyUSB", 0) == 0) {
                candidates_.push_back({p, 115200});
                candidates_.push_back({p, 460800});
            }
        }
        if (candidates_.empty()) return;
    }
    current_ = candidates_[nextCandidate_++];
    if (!port_.open(current_.path.c_str(), current_.baud)) return;
    openedUs_ = nowUs;
    imu_ = imu::Imu();
    nmea_ = nmea::SentenceReader();
    sentences_ = 0;
}

UsbScout::Kind UsbScout::tick(uint64_t nowUs) {
    if (!wantImu_ && !wantGnss_) {
        port_.close();
        return Kind::None;
    }
    if (port_.fd() < 0) {
        next(nowUs);
        return Kind::None;
    }
    uint8_t buf[512];
    int n;
    while ((n = port_.read(buf, sizeof buf)) > 0) {
        imu_.feed(buf, static_cast<size_t>(n), nowUs);
        for (int i = 0; i < n; ++i)
            if (nmea_.feed(static_cast<char>(buf[i])) && nmea_.sentence()[0] == '$') ++sentences_;
    }
    Kind k = Kind::None;
    if (wantImu_ && imu_.type() != imu::Type::None) k = Kind::Imu;
    else if (wantGnss_ && sentences_ >= kSentences) k = Kind::Gnss;
    if (k != Kind::None) {
        port_.close();
        found_ = current_.path;
        foundBaud_ = current_.baud;
        exclude(found_);
        nextCandidate_ = candidates_.size();  // list again next time: the rest may have changed
        return k;
    }
    if (nowUs - openedUs_ > kListenUs) port_.close();  // not what we want: the next one
    return Kind::None;
}

}  // namespace aow
