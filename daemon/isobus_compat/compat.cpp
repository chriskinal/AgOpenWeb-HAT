// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>

#include "aow/log.h"
#include "logging_utils.hpp"
#include "settings.hpp"

namespace {

std::string dir = ".";
std::mutex fileMutex;  // ISOBUS threads only, never the control thread
constexpr std::uintmax_t kMaxLogBytes = 1024 * 1024;

void toFile(const std::string& line) {
    std::lock_guard<std::mutex> l(fileMutex);
    const std::string path = dir + "/tc.log";
    std::error_code ec;
    if (std::filesystem::file_size(path, ec) > kMaxLogBytes && !ec) std::filesystem::rename(path, path + ".1", ec);
    std::ofstream out(path, std::ios::app);
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm);
    out << stamp << ' ' << line << '\n';
}

// Collects what is written until std::endl flushes it, then hands the line on.
class LineBuf : public std::stringbuf {
public:
    int sync() override {
        std::string line = str();
        str("");
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (line.empty()) return 0;
        toFile(line);
        LOG_DEBUG(Can, "TC: %s", line.c_str());
        return 0;
    }
};

}  // namespace

std::ostream& log() {
    thread_local LineBuf buf;
    thread_local std::ostream stream(&buf);
    return stream;
}

std::ostream& log(const std::string& tag) { return log() << "[" << tag << "] "; }

// As upstream's: the file's directory is made if missing, and a failure to make it throws.
std::string Settings::get_filename_path(std::string fileName) {
    const std::filesystem::path full = std::filesystem::path(dir) / fileName;
    std::error_code ec;
    std::filesystem::create_directories(full.parent_path(), ec);
    if (ec) throw std::runtime_error("Failed to create directory " + full.parent_path().string() + ": " + ec.message());
    return full.string();
}
void Settings::set_directory(const std::string& directory) {
    dir = directory;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
}
const std::string& Settings::directory() { return dir; }
