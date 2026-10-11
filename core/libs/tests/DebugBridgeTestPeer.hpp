#ifndef CORE_LIBS_TESTS_DEBUGBRIDGETESTPEER_HPP
#define CORE_LIBS_TESTS_DEBUGBRIDGETESTPEER_HPP

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include "prx/libc/include/Shutdown.hpp"
#include "prx/libkernel/System/include/DebugBridge.hpp"
#include "prx/libkernel/System/include/JsonParser.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

static void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static void SetEnvironment(const char* name, const std::string& value) {
#ifdef _WIN32
    Require(_putenv_s(name, value.c_str()) == 0, "Cannot set test environment");
#else
    Require(::setenv(name, value.c_str(), 1) == 0, "Cannot set test environment");
#endif
}

static void UnsetEnvironment(const char* name) {
#ifdef _WIN32
    Require(_putenv_s(name, "") == 0, "Cannot clear test environment");
#else
    Require(::unsetenv(name) == 0, "Cannot clear test environment");
#endif
}

struct BridgePacket {
    std::string text;
    sockaddr_in source{};
};

class BridgeCollector {
public:
    BridgeCollector() {
#ifdef _WIN32
        WSADATA data{};
        Require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "Cannot initialize test sockets");
#endif
        socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
        Require(socket != INVALID_SOCKET, "Cannot open test socket");
#else
        Require(socket >= 0 && socket < FD_SETSIZE, "Cannot open test socket");
#endif
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        Require(::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0, "Cannot bind test socket");
#ifdef _WIN32
        int size = sizeof(address);
#else
        socklen_t size = sizeof(address);
#endif
        Require(::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size) == 0, "Cannot read test port");
        port = ntohs(address.sin_port);
        Require(port != 0, "Test socket has no bound port");
    }

    ~BridgeCollector() {
#ifdef _WIN32
        if (socket != INVALID_SOCKET) ::closesocket(socket);
        WSACleanup();
#else
        if (socket >= 0) ::close(socket);
#endif
    }

    BridgeCollector(const BridgeCollector&) = delete;
    BridgeCollector& operator=(const BridgeCollector&) = delete;

    std::uint16_t Port() const { return port; }

    bool Receive(std::chrono::milliseconds duration, BridgePacket& packet) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(socket, &readable);
        timeval timeout{};
        timeout.tv_sec = static_cast<long>(duration.count() / 1000);
        timeout.tv_usec = static_cast<long>((duration.count() % 1000) * 1000);
#ifdef _WIN32
        const int ready = ::select(0, &readable, nullptr, nullptr, &timeout);
#else
        const int ready = ::select(socket + 1, &readable, nullptr, nullptr, &timeout);
#endif
        Require(ready >= 0, "Cannot wait for bridge packet");
        if (!ready) return false;
        std::array<char, 16385> buffer{};
#ifdef _WIN32
        int size = sizeof(packet.source);
#else
        socklen_t size = sizeof(packet.source);
#endif
        const auto count = ::recvfrom(socket, buffer.data(), static_cast<int>(buffer.size()), 0,
            reinterpret_cast<sockaddr*>(&packet.source), &size);
        Require(count > 0 && count <= 16384, "Invalid bridge datagram size");
        Require(size == sizeof(packet.source), "Unexpected bridge source address");
        packet.text.assign(buffer.data(), static_cast<std::size_t>(count));
        return true;
    }

    void Send(const sockaddr_in& destination, const std::string& packet) {
        const auto sent = ::sendto(socket, packet.data(), static_cast<int>(packet.size()), 0,
            reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
        Require(sent == static_cast<int>(packet.size()), "Cannot send test command");
    }

private:
#ifdef _WIN32
    SOCKET socket = INVALID_SOCKET;
#else
    int socket = -1;
#endif
    std::uint16_t port = 0;
};

static const JsonValue& Member(const JsonValue& object, const char* name, JsonType type) {
    const auto* value = findObjectMember(object, name);
    Require(value && value->type == type, "Unexpected JSON member type");
    return *value;
}

static std::uint64_t Unsigned(const JsonValue& object, const char* name) {
    const auto& value = Member(object, name, JsonType::Number).numberText;
    std::uint64_t number = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
    Require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size(), "Invalid JSON unsigned integer");
    return number;
}

static JsonValue Await(BridgeCollector& collector, const char* type, const std::string& commandId = {}) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    BridgePacket packet;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!collector.Receive(std::chrono::milliseconds(100), packet)) continue;
        auto root = parseJsonDocument(packet.text);
        if (Member(root, "type", JsonType::String).stringValue != type) continue;
        if (!commandId.empty() && Member(root, "command_id", JsonType::String).stringValue != commandId) continue;
        return root;
    }
    throw std::runtime_error(std::string("Missing bridge packet: ") + type);
}

static std::string Command(std::uint64_t sequence, const std::string& id, const std::string& args,
    const std::string& session = "framerift-debug-bridge-test", const std::string& name = "set_monitoring") {
    return "{\"protocol\":\"framerift.bridge\",\"version\":1,\"type\":\"command\",\"session_id\":\"" + session
        + "\",\"sequence\":" + std::to_string(sequence) + ",\"command_id\":\"" + id + "\",\"name\":\"" + name + "\",\"args\":" + args + '}';
}

static void CheckMonitoringState(const JsonValue& state, bool enabled, std::uint64_t epoch) {
    Require(Member(state, "enabled", JsonType::Boolean).boolValue == enabled, "Wrong confirmed monitoring state");
    Require(Unsigned(state, "epoch") == epoch, "Monitoring epoch changed unexpectedly");
}

static void NoEvents(BridgeCollector& collector, std::chrono::milliseconds duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    BridgePacket packet;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!collector.Receive(std::chrono::milliseconds(20), packet)) continue;
        const auto root = parseJsonDocument(packet.text);
        Require(Member(root, "type", JsonType::String).stringValue != "event", "Disabled monitoring emitted telemetry");
    }
}


#endif
