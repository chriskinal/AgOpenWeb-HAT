// A small HTTP/1.1 + WebSocket server, the transport under the firmware's web pages
// (the firmware's SimpleHTTPServer + SimpleWebSocket, rebuilt on POSIX sockets).
//
// Single-threaded and event-driven: whoever owns it calls poll() in a loop and may call
// broadcast() between polls. Every connection is non-blocking, so a slow browser cannot hold up
// the telemetry stream. HTTP connections close after one response, as in the firmware.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace aow::web {

struct Request {
    std::string method, path, query, body;
    std::vector<std::pair<std::string, std::string>> headers;  // names lower-cased
    std::string header(const char* lowerName) const;
};

struct Response {
    int code = 200;
    std::string contentType = "text/plain";
    std::string body;
    std::string location;  // for redirects

    static Response json(std::string body, int code = 200);
    static Response html(const char* page);
    static Response text(std::string body, int code = 200);
    static Response redirect(std::string to);
};

using Handler = std::function<Response(const Request&)>;

class Server {
public:
    Server() = default;
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Each returns the port actually bound (pass 0 for any free port), or 0 on failure.
    uint16_t listenHttp(uint16_t port);
    uint16_t listenWebSocket(uint16_t port);

    // Exact-path routes; the handler checks the method. Unknown paths get 404.
    void route(const std::string& path, Handler h);

    // Sends one message to every client of the WebSocket listener on this port.
    void broadcast(uint16_t wsPort, const void* data, size_t len, bool binary);
    size_t clientCount(uint16_t wsPort) const;

    // Waits up to timeoutMs for activity and handles it.
    void poll(int timeoutMs);

    size_t maxBody = 512 * 1024;         // largest request body accepted
    size_t maxWsBacklog = 1024 * 1024;   // a WebSocket client further behind than this is dropped

private:
    enum class Kind : uint8_t { Http, WebSocket };
    struct Listener { int fd; uint16_t port; Kind kind; };
    struct Conn {
        int fd;
        uint16_t port;
        Kind kind;
        bool open = false;          // WebSocket handshake done
        bool closeAfterWrite = false;
        std::string in, out;
        uint64_t lastActivityMs = 0;
    };

    uint16_t listen(uint16_t port, Kind kind);
    void accept(const Listener& l);
    void readFrom(Conn& c);
    void writeTo(Conn& c);
    void handleHttp(Conn& c);
    void handleWebSocketHandshake(Conn& c);
    void handleWebSocketFrames(Conn& c);
    bool parseRequest(Conn& c, Request& r, bool& complete);
    static std::string frame(const void* data, size_t len, uint8_t opcode);
    void close(int fd);

    std::vector<Listener> listeners_;
    std::map<int, Conn> conns_;
    std::map<std::string, Handler> routes_;
};

// Exposed for tests.
std::string sha1(const std::string& data);  // 20 raw bytes
std::string base64(const std::string& data);
std::string webSocketAccept(const std::string& key);

}  // namespace aow::web
