// Loopback client helpers for the web tests: the server under test runs in the same thread,
// so every wait pumps it.
#pragma once

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <string>

#include "server.h"

using aow::web::Server;

inline int connectTo(uint16_t port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) return -1;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

// Polls the server and reads the client until `done` says the reply is complete (or 2 s pass).
template <typename Done>
inline std::string pump(Server& s, int fd, Done done) {
    std::string got;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < until && !done(got)) {
        s.poll(5);
        char buf[65536];
        ssize_t n;
        while ((n = recv(fd, buf, sizeof buf, 0)) > 0) got.append(buf, static_cast<size_t>(n));
        if (n == 0) break;  // server closed
    }
    return got;
}

inline std::string request(Server& s, uint16_t port, const std::string& req) {
    const int fd = connectTo(port);
    send(fd, req.data(), req.size(), 0);
    std::string got = pump(s, fd, [](const std::string&) { return false; });  // until close
    close(fd);
    return got;
}

