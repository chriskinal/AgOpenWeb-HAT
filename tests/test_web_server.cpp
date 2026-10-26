// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors
//
// Drives aow::web::Server over real loopback sockets, in one thread.
#include "server.h"

#include <sys/socket.h>
#include <unistd.h>

#include <stdexcept>
#include <string>

#include "check.h"
#include "http_client.h"

using namespace aow::web;

static std::string maskedFrame(uint8_t opcode, const std::string& payload) {
    std::string f;
    f += static_cast<char>(0x80 | opcode);
    f += static_cast<char>(0x80 | payload.size());
    const char mask[4] = {0x11, 0x22, 0x33, 0x44};
    f.append(mask, 4);
    for (size_t i = 0; i < payload.size(); ++i) f += static_cast<char>(payload[i] ^ mask[i % 4]);
    return f;
}

int main() {
    // RFC 6455 section 1.3 worked example, and FIPS 180 "abc".
    CHECK(webSocketAccept("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    CHECK(base64(sha1("abc")) == "qZk+NkcGgWq6PiVxeFDCbJzQ2J0=");
    CHECK(base64("") == "" && base64("f") == "Zg==" && base64("fo") == "Zm8=" && base64("foo") == "Zm9v");

    Server s;
    const uint16_t http = s.listenHttp(0);
    const uint16_t ws = s.listenWebSocket(0);
    CHECK(http && ws && http != ws);

    std::string lastBody, lastMethod, lastQuery;
    s.route("/api/echo", [&](const Request& r) {
        lastMethod = r.method;
        lastBody = r.body;
        lastQuery = r.query;
        return Response::json("{\"ok\":true,\"len\":" + std::to_string(r.body.size()) + "}");
    });
    s.route("/", [](const Request&) { return Response::html("<h1>home</h1>"); });
    s.route("/boom", [](const Request&) -> Response { throw std::runtime_error("x"); });

    // GET a page.
    std::string r = request(s, http, "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(r.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
    CHECK(r.find("Content-Type: text/html") != std::string::npos);
    CHECK(r.find("Content-Length: 13\r\n") != std::string::npos);
    CHECK(r.size() >= 13 && r.compare(r.size() - 13, 13, "<h1>home</h1>") == 0);

    // POST with a body that arrives in two pieces, and a query string.
    {
        const std::string body(40000, 'b');
        const std::string head = "POST /api/echo?x=1 HTTP/1.1\r\nContent-Length: " +
                                 std::to_string(body.size()) + "\r\nContent-Type: application/json\r\n\r\n";
        const int fd = connectTo(http);
        send(fd, head.data(), head.size(), 0);
        send(fd, body.data(), 1000, 0);
        pump(s, fd, [&](const std::string&) { return false; } /* let the server see a partial body */);
        send(fd, body.data() + 1000, body.size() - 1000, 0);
        const std::string got = pump(s, fd, [](const std::string& g) { return g.find("}") != std::string::npos; });
        close(fd);
        CHECK(lastMethod == "POST" && lastQuery == "x=1" && lastBody.size() == body.size());
        CHECK(got.find("{\"ok\":true,\"len\":40000}") != std::string::npos);
    }

    // Unknown path, handler that throws, garbage request.
    CHECK(request(s, http, "GET /nope HTTP/1.1\r\n\r\n").rfind("HTTP/1.1 404", 0) == 0);
    CHECK(request(s, http, "GET /boom HTTP/1.1\r\n\r\n").rfind("HTTP/1.1 500", 0) == 0);
    CHECK(request(s, http, "garbage\r\n\r\n").rfind("HTTP/1.1 400", 0) == 0);

    // A body over the limit is refused.
    s.maxBody = 100;
    CHECK(request(s, http, "POST /api/echo HTTP/1.1\r\nContent-Length: 5000\r\n\r\n").rfind("HTTP/1.1 400", 0) == 0);

    // WebSocket: handshake, broadcast (text and a 300-byte binary), ping, close.
    {
        const int fd = connectTo(ws);
        const std::string hs = "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                               "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
        send(fd, hs.data(), hs.size(), 0);
        std::string got = pump(s, fd, [](const std::string& g) { return g.find("\r\n\r\n") != std::string::npos; });
        CHECK(got.rfind("HTTP/1.1 101", 0) == 0);
        CHECK(got.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);
        CHECK(s.clientCount(ws) == 1 && s.clientCount(http) == 0);

        s.broadcast(ws, "hello", 5, false);
        got = pump(s, fd, [](const std::string& g) { return g.size() >= 7; });
        CHECK(got == std::string("\x81\x05hello", 7));

        std::string bin(300, '\x5A');
        s.broadcast(ws, bin.data(), bin.size(), true);
        got = pump(s, fd, [](const std::string& g) { return g.size() >= 304; });
        CHECK(got.size() == 304 && uint8_t(got[0]) == 0x82 && uint8_t(got[1]) == 126);
        CHECK(uint8_t(got[2]) == 1 && uint8_t(got[3]) == 44 && got.substr(4) == bin);

        const std::string ping = maskedFrame(0x9, "hi");
        send(fd, ping.data(), ping.size(), 0);
        got = pump(s, fd, [](const std::string& g) { return g.size() >= 4; });
        CHECK(got == std::string("\x8A\x02hi", 4));

        const std::string bye = maskedFrame(0x8, std::string("\x03\xE8", 2));
        send(fd, bye.data(), bye.size(), 0);
        got = pump(s, fd, [](const std::string&) { return false; });
        CHECK(got.size() >= 2 && uint8_t(got[0]) == 0x88);
        close(fd);
        s.poll(5);
        CHECK(s.clientCount(ws) == 0);
    }

    // A plain HTTP request on the WebSocket port is refused.
    CHECK(request(s, ws, "GET / HTTP/1.1\r\n\r\n").rfind("HTTP/1.1 400", 0) == 0);

    return finish("web_server");
}
