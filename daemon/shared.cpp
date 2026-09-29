#include "shared.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

namespace aow {

bool SettingsFile::load(Settings& s, std::string& error) const {
    std::ifstream in(path);
    if (!in) {
        if (errno == ENOENT) return true;  // first run: defaults
        error = path + ": " + std::strerror(errno);
        return false;
    }
    std::stringstream buf;
    buf << in.rdbuf();
    std::string why;
    if (!fromJson(buf.str(), s, &why)) {
        error = path + ": " + why;
        return false;
    }
    return true;
}

bool SettingsFile::save(const Settings& s, std::string& error) const {
    const std::string tmp = path + ".tmp";
    const std::string json = toJson(s);

    // Create the directory on first save (/etc/aow-hat).
    const size_t slash = path.rfind('/');
    if (slash != std::string::npos && slash > 0) mkdir(path.substr(0, slash).c_str(), 0755);

    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        error = tmp + ": " + std::strerror(errno);
        return false;
    }
    const bool ok = ::write(fd, json.data(), json.size()) == static_cast<ssize_t>(json.size()) && fsync(fd) == 0;
    const int savedErrno = errno;
    ::close(fd);
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
        error = path + ": " + std::strerror(ok ? errno : savedErrno);
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

}  // namespace aow
