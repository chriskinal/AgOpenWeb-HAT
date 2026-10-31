// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors

#include "server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>

#include "aow/log.h"

namespace aow::web {

namespace {

constexpr size_t kMaxHeader = 16 * 1024;
constexpr uint64_t kHttpIdleMs = 10'000;

uint64_t nowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch()).count());
}

const char* reason(int code) {
    switch (code) {
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 302: return "Found";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "Unknown";
    }
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t");
    const size_t b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::string serialize(const Response& r) {
    char head[512];
    int n = std::snprintf(head, sizeof head,
                          "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                          "Cache-Control: no-store\r\nConnection: close\r\n",
                          r.code, reason(r.code), r.contentType.c_str(), r.body.size());
    std::string out(head, static_cast<size_t>(n));
    if (!r.location.empty()) out += "Location: " + r.location + "\r\n";
    out += "\r\n";
    out += r.body;
    return out;
}

}  // namespace

// --- Request / Response

std::string Request::header(const char* lowerName) const {
    for (const auto& h : headers)
        if (h.first == lowerName) return h.second;
    return "";
}

Response Response::json(std::string body, int code) {
    Response r;
    r.code = code;
    r.contentType = "application/json";
    r.body = std::move(body);
    return r;
}

Response Response::html(const char* page) {
    Response r;
    r.contentType = "text/html; charset=utf-8";
    r.body = page;
    return r;
}

Response Response::text(std::string body, int code) {
    Response r;
    r.code = code;
    r.body = std::move(body);
    return r;
}

Response Response::redirect(std::string to) {
    Response r;
    r.code = 302;
    r.location = std::move(to);
    return r;
}

// --- SHA-1 and base64, for the WebSocket handshake only

std::string sha1(const std::string& data) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string m = data;
    const uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
    m += static_cast<char>(0x80);
    while (m.size() % 64 != 56) m += '\0';
    for (int i = 7; i >= 0; --i) m += static_cast<char>((bits >> (i * 8)) & 0xFF);

    auto rol = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const auto* p = reinterpret_cast<const unsigned char*>(m.data() + off + i * 4);
            w[i] = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
        }
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    std::string out;
    for (uint32_t v : h)
        for (int i = 3; i >= 0; --i) out += static_cast<char>((v >> (i * 8)) & 0xFF);
    return out;
}

std::string base64(const std::string& data) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        const uint32_t v = uint32_t(uint8_t(data[i])) << 16 | uint32_t(uint8_t(data[i + 1])) << 8 | uint8_t(data[i + 2]);
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += t[v & 63];
    }
    if (i + 1 == data.size()) {
        const uint32_t v = uint32_t(uint8_t(data[i])) << 16;
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == data.size()) {
        const uint32_t v = uint32_t(uint8_t(data[i])) << 16 | uint32_t(uint8_t(data[i + 1])) << 8;
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += '=';
    }
    return out;
}

std::string webSocketAccept(const std::string& key) {
    return base64(sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
}

// --- Server

Server::~Server() {
    for (auto& c : conns_) ::close(c.first);
    for (auto& l : listeners_) ::close(l.fd);
}

uint16_t Server::listenHttp(uint16_t port) { return listen(port, Kind::Http); }
uint16_t Server::listenWebSocket(uint16_t port) { return listen(port, Kind::WebSocket); }

uint16_t Server::listen(uint16_t port, Kind kind) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0 || ::listen(fd, 16) < 0) {
        LOG_ERROR(Network, "web: cannot listen on port %u: %s", port, std::strerror(errno));
        ::close(fd);
        return 0;
    }
    socklen_t len = sizeof a;
    getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
    const uint16_t bound = ntohs(a.sin_port);
    listeners_.push_back({fd, bound, kind});
    return bound;
}

void Server::route(const std::string& path, Handler h) { routes_[path] = std::move(h); }

void Server::uploadRoute(const std::string& path, const std::string& file, size_t maxBytes, Handler h) {
    routes_[path] = std::move(h);
    uploads_[path] = Upload{file, maxBytes};
}

void Server::greet(uint16_t wsPort, std::function<std::string()> text) { greetings_[wsPort] = std::move(text); }

size_t Server::clientCount(uint16_t wsPort) const {
    size_t n = 0;
    for (const auto& c : conns_)
        if (c.second.port == wsPort && c.second.kind == Kind::WebSocket && c.second.open) ++n;
    return n;
}

std::string Server::frame(const void* data, size_t len, uint8_t opcode) {
    std::string f;
    f += static_cast<char>(0x80 | opcode);  // FIN, unmasked (server to client)
    if (len < 126) {
        f += static_cast<char>(len);
    } else if (len < 65536) {
        f += static_cast<char>(126);
        f += static_cast<char>(len >> 8);
        f += static_cast<char>(len & 0xFF);
    } else {
        f += static_cast<char>(127);
        for (int i = 7; i >= 0; --i) f += static_cast<char>((static_cast<uint64_t>(len) >> (i * 8)) & 0xFF);
    }
    f.append(static_cast<const char*>(data), len);
    return f;
}

void Server::broadcast(uint16_t wsPort, const void* data, size_t len, bool binary) {
    std::string f;
    std::vector<int> drop;
    for (auto& kv : conns_) {
        Conn& c = kv.second;
        if (c.port != wsPort || c.kind != Kind::WebSocket || !c.open) continue;
        if (c.out.size() > maxWsBacklog) {
            drop.push_back(c.fd);
            continue;
        }
        if (f.empty()) f = frame(data, len, binary ? 0x2 : 0x1);
        c.out += f;
    }
    for (int fd : drop) close(fd);
}

void Server::close(int fd) {
    const auto it = conns_.find(fd);
    if (it != conns_.end() && it->second.bodyFd >= 0) {  // lost mid-upload: drop the part file
        ::close(it->second.bodyFd);
        ::unlink(it->second.upload.bodyFile.c_str());
    }
    ::close(fd);
    conns_.erase(fd);
}

void Server::poll(int timeoutMs) {
    std::vector<pollfd> fds;
    for (const auto& l : listeners_) fds.push_back({l.fd, POLLIN, 0});
    for (const auto& kv : conns_)
        fds.push_back({kv.first, static_cast<short>(POLLIN | (kv.second.out.empty() ? 0 : POLLOUT)), 0});

    if (::poll(fds.data(), fds.size(), timeoutMs) <= 0) {
        // Nothing arrived; still time out idle HTTP connections below.
    }

    const uint64_t now = nowMs();
    for (size_t i = 0; i < fds.size(); ++i) {
        if (!fds[i].revents) continue;
        if (i < listeners_.size()) {
            accept(listeners_[i]);
            continue;
        }
        auto it = conns_.find(fds[i].fd);
        if (it == conns_.end()) continue;
        Conn& c = it->second;
        if (fds[i].revents & (POLLERR | POLLNVAL)) {
            close(c.fd);
            continue;
        }
        if (fds[i].revents & (POLLIN | POLLHUP)) {
            c.lastActivityMs = now;
            readFrom(c);
            if (!conns_.count(fds[i].fd)) continue;
        }
        if ((fds[i].revents & POLLOUT) || !c.out.empty()) writeTo(c);
    }

    // Connections accepted above can carry a later timestamp than `now`; don't let that wrap.
    std::vector<int> idle;
    for (const auto& kv : conns_)
        if (kv.second.kind == Kind::Http && now > kv.second.lastActivityMs &&
            now - kv.second.lastActivityMs > kHttpIdleMs)
            idle.push_back(kv.first);
    for (int fd : idle) close(fd);
}

void Server::accept(const Listener& l) {
    for (;;) {
        const int fd = ::accept(l.fd, nullptr, nullptr);
        if (fd < 0) return;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
        const int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        Conn c;
        c.fd = fd;
        c.port = l.port;
        c.kind = l.kind;
        c.lastActivityMs = nowMs();
        conns_.emplace(fd, std::move(c));
    }
}

void Server::readFrom(Conn& c) {
    char buf[16384];
    for (;;) {
        const ssize_t n = ::recv(c.fd, buf, sizeof buf, 0);
        if (n > 0) {
            if (c.bodyFd >= 0) {
                writeBody(c, buf, static_cast<size_t>(n));
                continue;
            }
            c.in.append(buf, static_cast<size_t>(n));
            if (c.in.size() > kMaxHeader + maxBody) {
                close(c.fd);
                return;
            }
            continue;
        }
        if (n == 0) {  // peer closed
            close(c.fd);
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        close(c.fd);
        return;
    }
    if (c.kind == Kind::Http) handleHttp(c);
    else if (!c.open) handleWebSocketHandshake(c);
    else handleWebSocketFrames(c);
}

void Server::writeTo(Conn& c) {
    while (!c.out.empty()) {
#ifdef MSG_NOSIGNAL
        const ssize_t n = ::send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);
#else
        const ssize_t n = ::send(c.fd, c.out.data(), c.out.size(), 0);
#endif
        if (n > 0) {
            c.out.erase(0, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n < 0 && errno == EINTR) continue;
        close(c.fd);
        return;
    }
    if (c.closeAfterWrite) close(c.fd);
}

bool Server::parseRequest(Conn& c, Request& r, bool& complete) {
    complete = false;
    const size_t end = c.in.find("\r\n\r\n");
    if (end == std::string::npos) return c.in.size() <= kMaxHeader;

    const size_t lineEnd = c.in.find("\r\n");
    const std::string line = c.in.substr(0, lineEnd);
    const size_t sp1 = line.find(' ');
    const size_t sp2 = line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
    r.method = line.substr(0, sp1);
    std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const size_t q = target.find('?');
    r.path = target.substr(0, q);
    if (q != std::string::npos) r.query = target.substr(q + 1);

    size_t pos = lineEnd + 2;
    while (pos < end) {
        const size_t e = c.in.find("\r\n", pos);
        const std::string h = c.in.substr(pos, e - pos);
        const size_t colon = h.find(':');
        if (colon != std::string::npos) r.headers.emplace_back(lower(trim(h.substr(0, colon))), trim(h.substr(colon + 1)));
        pos = e + 2;
    }

    const std::string cl = r.header("content-length");
    const size_t bodyLen = cl.empty() ? 0 : std::strtoul(cl.c_str(), nullptr, 10);
    const auto up = uploads_.find(r.path);
    if (up != uploads_.end() && r.method == "POST" && c.kind == Kind::Http) {
        // The headers are enough: the body goes to the file from here on (handleHttp).
        c.upload = r;
        c.upload.bodyFile = up->second.file;
        c.bodyLeft = bodyLen;
        c.in.erase(0, end + 4);
        complete = true;
        return true;
    }
    if (bodyLen > maxBody) return false;
    if (c.in.size() < end + 4 + bodyLen) return true;  // body still arriving
    r.body = c.in.substr(end + 4, bodyLen);
    c.in.erase(0, end + 4 + bodyLen);
    complete = true;
    return true;
}

void Server::writeBody(Conn& c, const char* data, size_t len) {
    if (len > c.bodyLeft) len = c.bodyLeft;  // anything after the body is ignored
    c.bodyLeft -= len;
    while (len > 0 && !c.bodyErrno) {
        const ssize_t n = ::write(c.bodyFd, data, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            c.bodyErrno = n < 0 ? errno : ENOSPC;
            break;
        }
        data += n;
        len -= static_cast<size_t>(n);
    }
}

void Server::finishUpload(Conn& c) {
    ::close(c.bodyFd);
    c.bodyFd = -1;
    Request r = std::move(c.upload);
    Response resp;
    if (c.bodyErrno) {
        ::unlink(r.bodyFile.c_str());
        LOG_ERROR(Network, "web: upload to %s: %s", r.bodyFile.c_str(), std::strerror(c.bodyErrno));
        resp = Response::text(std::string("The upload could not be stored: ") + std::strerror(c.bodyErrno), 500);
    } else {
        try {
            resp = routes_[r.path](r);
        } catch (const std::exception& e) {
            LOG_ERROR(Network, "web: %s %s: %s", r.method.c_str(), r.path.c_str(), e.what());
            resp = Response::text("Internal Server Error", 500);
        }
    }
    c.out = serialize(resp);
    c.closeAfterWrite = true;
}

void Server::handleHttp(Conn& c) {
    if (c.closeAfterWrite) return;  // already answered; ignore anything more
    if (c.bodyFd >= 0) {
        if (c.bodyLeft == 0) finishUpload(c);
        return;
    }
    Request r;
    bool complete;
    Response resp;
    if (!parseRequest(c, r, complete)) {
        resp = Response::text("Bad Request", 400);
    } else if (!complete) {
        return;
    } else if (!c.upload.bodyFile.empty()) {
        const Upload& up = uploads_[c.upload.path];
        if (c.bodyLeft > up.maxBytes) {
            c.upload = Request{};
            resp = Response::text("The file is too large.", 413);
        } else if ((c.bodyFd = ::open(up.file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600)) < 0) {
            LOG_ERROR(Network, "web: upload to %s: %s", up.file.c_str(), std::strerror(errno));
            resp = Response::text(std::string("The upload could not be stored: ") + std::strerror(errno), 500);
            c.upload = Request{};
        } else {
            // What came with the headers, then the rest as it arrives (readFrom).
            const std::string first = std::move(c.in);
            c.in.clear();
            writeBody(c, first.data(), first.size());
            if (c.bodyLeft == 0) finishUpload(c);
            return;
        }
    } else {
        auto it = routes_.find(r.path);
        if (it == routes_.end()) {
            resp = Response::text("Not Found", 404);
        } else {
            try {
                resp = it->second(r);
            } catch (const std::exception& e) {
                LOG_ERROR(Network, "web: %s %s: %s", r.method.c_str(), r.path.c_str(), e.what());
                resp = Response::text("Internal Server Error", 500);
            }
        }
    }
    c.out = serialize(resp);
    c.closeAfterWrite = true;
}

void Server::handleWebSocketHandshake(Conn& c) {
    Request r;
    bool complete;
    if (!parseRequest(c, r, complete)) {
        close(c.fd);
        return;
    }
    if (!complete) return;
    const std::string key = r.header("sec-websocket-key");
    if (lower(r.header("upgrade")) != "websocket" || key.empty()) {
        c.out = serialize(Response::text("Expected a WebSocket upgrade", 400));
        c.closeAfterWrite = true;
        return;
    }
    c.out = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + webSocketAccept(key) + "\r\n\r\n";
    c.open = true;
    const auto greeting = greetings_.find(c.port);
    if (greeting != greetings_.end()) {
        const std::string text = greeting->second();
        c.out += frame(text.data(), text.size(), 0x1);
    }
    handleWebSocketFrames(c);  // in case a frame came with the handshake
}

void Server::handleWebSocketFrames(Conn& c) {
    // Client frames are masked. Only close and ping matter: the pages never send data.
    while (c.in.size() >= 2) {
        const auto* p = reinterpret_cast<const uint8_t*>(c.in.data());
        const uint8_t opcode = p[0] & 0x0F;
        const bool masked = p[1] & 0x80;
        uint64_t len = p[1] & 0x7F;
        size_t hdr = 2;
        if (len == 126) {
            if (c.in.size() < 4) return;
            len = uint64_t(p[2]) << 8 | p[3];
            hdr = 4;
        } else if (len == 127) {
            if (c.in.size() < 10) return;
            len = 0;
            for (int i = 0; i < 8; ++i) len = len << 8 | p[2 + i];
            hdr = 10;
        }
        if (len > 65536) {
            close(c.fd);
            return;
        }
        const size_t maskLen = masked ? 4 : 0;
        if (c.in.size() < hdr + maskLen + len) return;
        std::string payload = c.in.substr(hdr + maskLen, static_cast<size_t>(len));
        if (masked)
            for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= c.in[hdr + i % 4];
        c.in.erase(0, hdr + maskLen + static_cast<size_t>(len));

        if (opcode == 0x8) {  // close: echo it and hang up
            c.out += frame(payload.data(), payload.size() < 2 ? payload.size() : 2, 0x8);
            c.closeAfterWrite = true;
            return;
        }
        if (opcode == 0x9) c.out += frame(payload.data(), payload.size(), 0xA);  // ping -> pong
    }
}

}  // namespace aow::web
