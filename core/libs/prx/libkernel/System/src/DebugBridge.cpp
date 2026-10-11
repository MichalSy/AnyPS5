#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "prx/libkernel/System/include/DebugBridge.hpp"
#include "prx/libc/include/Shutdown.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace {

std::atomic_flag diagnosed = ATOMIC_FLAG_INIT;

void Diagnose(const char* message) noexcept {
    if (!diagnosed.test_and_set(std::memory_order_relaxed))
        std::fprintf(stderr, "[debug-bridge] disabled: %s\n", message);
}

bool ValidUtf8(std::string_view text) {
    for (std::size_t at = 0; at < text.size();) {
        const auto first = static_cast<unsigned char>(text[at++]);
        if (first < 0x80) continue;
        std::uint32_t code = 0;
        std::uint32_t minimum = 0;
        std::size_t following = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            code = first & 0x1f;
            minimum = 0x80;
            following = 1;
        } else if (first >= 0xe0 && first <= 0xef) {
            code = first & 0x0f;
            minimum = 0x800;
            following = 2;
        } else if (first >= 0xf0 && first <= 0xf4) {
            code = first & 0x07;
            minimum = 0x10000;
            following = 3;
        } else {
            return false;
        }
        if (following > text.size() - at) return false;
        for (std::size_t index = 0; index < following; ++index) {
            const auto next = static_cast<unsigned char>(text[at++]);
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}

std::string QuoteId(const char* text, const char* error) {
    const std::string_view value(text);
    if (value.empty() || value.size() > 128 || !ValidUtf8(value)) throw std::invalid_argument(error);
    constexpr char digits[] = "0123456789abcdef";
    std::string quoted = "\"";
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte == '\\' || byte == '"') {
            quoted += '\\';
            quoted += character;
        } else if (byte < 0x20) {
            quoted += "\\u00";
            quoted += digits[byte >> 4];
            quoted += digits[byte & 0x0f];
        } else {
            quoted += character;
        }
    }
    quoted += '"';
    return quoted;
}

std::uint16_t ParsePort(const char* text) {
    unsigned value = 0;
    if (*text == '\0') throw std::invalid_argument("FRAMERIFT_BRIDGE_PORT must be a decimal port from 1 to 65535");
    for (const auto* at = text; *at != '\0'; ++at) {
        if (*at < '0' || *at > '9' || value > (65535u - static_cast<unsigned>(*at - '0')) / 10u)
            throw std::invalid_argument("FRAMERIFT_BRIDGE_PORT must be a decimal port from 1 to 65535");
        value = value * 10u + static_cast<unsigned>(*at - '0');
    }
    if (value == 0) throw std::invalid_argument("FRAMERIFT_BRIDGE_PORT must be a decimal port from 1 to 65535");
    return static_cast<std::uint16_t>(value);
}

int SocketError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool DroppedDatagram(int error) {
#ifdef _WIN32
    return error == WSAEWOULDBLOCK || error == WSAENOBUFS || error == WSAEINTR
        || error == WSAECONNREFUSED || error == WSAECONNRESET
        || error == WSAENETUNREACH || error == WSAEHOSTUNREACH;
#else
    return error == EAGAIN || error == EWOULDBLOCK || error == ENOBUFS || error == EINTR
        || error == ECONNREFUSED || error == ECONNRESET || error == ENETUNREACH || error == EHOSTUNREACH;
#endif
}

class DatagramSocket {
public:
    DatagramSocket() = default;
    DatagramSocket(const DatagramSocket&) = delete;
    DatagramSocket& operator=(const DatagramSocket&) = delete;

    ~DatagramSocket() {
#ifdef _WIN32
        if (handle != INVALID_SOCKET) closesocket(handle);
        if (winsockStarted) WSACleanup();
#else
        if (handle >= 0) close(handle);
#endif
    }

    void Open() {
#ifdef _WIN32
        WSADATA data{};
        if (const auto error = WSAStartup(MAKEWORD(2, 2), &data); error != 0)
            throw std::system_error(error, std::system_category(), "starting Winsock");
        winsockStarted = true;
#endif
        handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
        if (handle == INVALID_SOCKET) throw std::system_error(SocketError(), std::system_category(), "creating UDP socket");
        u_long nonblocking = 1;
        if (ioctlsocket(handle, FIONBIO, &nonblocking) != 0)
            throw std::system_error(SocketError(), std::system_category(), "making UDP socket nonblocking");
#else
        if (handle < 0) throw std::system_error(SocketError(), std::system_category(), "creating UDP socket");
        if (fcntl(handle, F_SETFL, O_NONBLOCK) < 0 || fcntl(handle, F_SETFD, FD_CLOEXEC) < 0)
            throw std::system_error(SocketError(), std::system_category(), "making UDP socket nonblocking");
#endif
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(handle, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0)
            throw std::system_error(SocketError(), std::system_category(), "binding loopback UDP socket");
    }

    void Send(std::uint16_t port, const std::string& packet) {
        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        destination.sin_port = htons(port);
#ifdef _WIN32
        const auto sent = ::sendto(handle, packet.data(), static_cast<int>(packet.size()), 0,
            reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
#else
        const auto sent = ::sendto(handle, packet.data(), packet.size(), 0,
            reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
#endif
        if (sent < 0) {
            const auto error = SocketError();
            if (!DroppedDatagram(error)) throw std::system_error(error, std::system_category(), "sending UDP packet");
        } else if (static_cast<std::size_t>(sent) != packet.size()) {
            throw std::runtime_error("incomplete UDP datagram send");
        }
    }

private:
#ifdef _WIN32
    SOCKET handle = INVALID_SOCKET;
    bool winsockStarted = false;
#else
    int handle = -1;
#endif
};

class DebugBridge {
public:
    ~DebugBridge() { Stop(); }

    void Start() {
        std::lock_guard lock(lifecycleMutex);
        if (started || stopped) return;
        started = true;
        const auto* enabled = std::getenv("APS5_DEBUG_BRIDGE");
        if (enabled && std::strcmp(enabled, "0") == 0) return;
        const auto* host = std::getenv("FRAMERIFT_BRIDGE_HOST");
        const auto* portValue = std::getenv("FRAMERIFT_BRIDGE_PORT");
        const auto* session = std::getenv("FRAMERIFT_BRIDGE_SESSION");
        const auto* game = std::getenv("FRAMERIFT_GAME_ID");
        if (!host && !portValue && !session && !game) return;
        if (!host || !portValue || !session || !game)
            throw std::invalid_argument("FRAMERIFT bridge requires HOST, PORT, SESSION and GAME_ID environment variables");
        if (std::strcmp(host, "127.0.0.1") != 0)
            throw std::invalid_argument("FRAMERIFT_BRIDGE_HOST must be 127.0.0.1");
        port = ParsePort(portValue);
        sessionId = QuoteId(session, "FRAMERIFT_BRIDGE_SESSION must contain 1 to 128 bytes of valid UTF-8");
        gameId = QuoteId(game, "FRAMERIFT_GAME_ID must contain 1 to 128 bytes of valid UTF-8");
        LibcRegisterShutdown_nid_postfix(DebugBridgeStop_nid_no_patch);
        worker = std::thread([this] { run(); });
    }

    void Stop() noexcept {
        try {
            std::lock_guard lock(lifecycleMutex);
            stopped = true;
            {
                std::lock_guard waitLock(waitMutex);
                stopping.store(true, std::memory_order_release);
            }
            changed.notify_all();
            if (worker.joinable()) worker.join();
        } catch (const std::exception& error) {
            Diagnose(error.what());
        } catch (...) {
            Diagnose("stopping bridge failed");
        }
    }

private:
    std::string packet(const char* type) {
        if (sequence == static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            throw std::runtime_error("bridge sequence exhausted");
        auto result = std::string("{\"protocol\":\"framerift.bridge\",\"version\":1,\"type\":\"") + type
            + "\",\"session_id\":" + sessionId + ",\"sequence\":" + std::to_string(++sequence);
        if (std::strcmp(type, "hello") == 0) result += ",\"game_id\":" + gameId + ",\"capabilities\":[]";
        result += '}';
        return result;
    }

    void run() noexcept {
        try {
            DatagramSocket socket;
            socket.Open();
            if (stopping.load(std::memory_order_acquire)) return;
            socket.Send(port, packet("hello"));
            using Clock = std::chrono::steady_clock;
            auto heartbeatAt = Clock::now() + std::chrono::seconds(1);
            auto helloAt = Clock::now() + std::chrono::seconds(3);
            for (;;) {
                {
                    std::unique_lock lock(waitMutex);
                    changed.wait_until(lock, std::min(heartbeatAt, helloAt), [this] {
                        return stopping.load(std::memory_order_acquire);
                    });
                }
                if (stopping.load(std::memory_order_acquire)) break;
                const auto now = Clock::now();
                if (now >= helloAt) {
                    socket.Send(port, packet("hello"));
                    helloAt = now + std::chrono::seconds(3);
                }
                if (now >= heartbeatAt) {
                    socket.Send(port, packet("heartbeat"));
                    heartbeatAt = now + std::chrono::seconds(1);
                }
            }
            socket.Send(port, packet("goodbye"));
        } catch (const std::exception& error) {
            Diagnose(error.what());
        } catch (...) {
            Diagnose("bridge worker failed");
        }
    }

    std::mutex lifecycleMutex;
    std::mutex waitMutex;
    std::condition_variable changed;
    std::thread worker;
    std::atomic<bool> stopping{false};
    bool started = false;
    bool stopped = false;
    std::uint16_t port = 0;
    std::uint64_t sequence = 0;
    std::string sessionId;
    std::string gameId;
};

DebugBridge& GetBridge() {
#ifdef _WIN32
    static auto* bridge = new DebugBridge;
    return *bridge;
#else
    static DebugBridge bridge;
    return bridge;
#endif
}

}

extern "C" void DebugBridgeStart_nid_no_patch() noexcept {
    try {
        GetBridge().Start();
    } catch (const std::exception& error) {
        Diagnose(error.what());
    } catch (...) {
        Diagnose("starting bridge failed");
    }
}

extern "C" void DebugBridgeStop_nid_no_patch() noexcept {
    try {
        GetBridge().Stop();
    } catch (const std::exception& error) {
        Diagnose(error.what());
    } catch (...) {
        Diagnose("stopping bridge failed");
    }
}
