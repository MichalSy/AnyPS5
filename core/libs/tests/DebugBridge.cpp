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

struct Packet {
    std::string text;
    sockaddr_in source{};
};

class Collector {
public:
    Collector() {
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

    ~Collector() {
#ifdef _WIN32
        if (socket != INVALID_SOCKET) ::closesocket(socket);
        WSACleanup();
#else
        if (socket >= 0) ::close(socket);
#endif
    }

    Collector(const Collector&) = delete;
    Collector& operator=(const Collector&) = delete;

    std::uint16_t Port() const { return port; }

    bool Receive(std::chrono::milliseconds duration, Packet& packet) {
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

private:
#ifdef _WIN32
    SOCKET socket = INVALID_SOCKET;
#else
    int socket = -1;
#endif
    std::uint16_t port = 0;
};

static std::string StringField(const std::string& text, const char* name) {
    const std::string key = std::string("\"") + name + "\":\"";
    const auto position = text.find(key);
    Require(position != std::string::npos && text.find(key, position + key.size()) == std::string::npos,
        "Missing or duplicate bridge string field");
    const auto begin = position + key.size();
    const auto end = text.find('"', begin);
    Require(end != std::string::npos, "Unterminated bridge string field");
    return text.substr(begin, end - begin);
}

static std::uint64_t IntegerField(const std::string& text, const char* name) {
    const std::string key = std::string("\"") + name + "\":";
    const auto position = text.find(key);
    Require(position != std::string::npos && text.find(key, position + key.size()) == std::string::npos,
        "Missing or duplicate bridge integer field");
    const auto begin = position + key.size();
    const auto end = text.find_first_of(",}", begin);
    Require(end != std::string::npos, "Unterminated bridge integer field");
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data() + begin, text.data() + end, value);
    Require(parsed.ec == std::errc{} && parsed.ptr == text.data() + end, "Invalid bridge integer field");
    return value;
}

static std::string CheckPacket(Packet& packet, std::uint64_t& sequence, std::uint16_t& sourcePort) {
    packet.text.erase(std::remove_if(packet.text.begin(), packet.text.end(), [](unsigned char value) {
        return std::isspace(value) != 0;
    }), packet.text.end());
    const auto& text = packet.text;
    Require(!text.empty() && text.front() == '{' && text.back() == '}', "Bridge packet is not a JSON object");
    Require(StringField(text, "protocol") == "framerift.bridge", "Wrong bridge protocol");
    Require(IntegerField(text, "version") == 1, "Wrong bridge protocol version");
    Require(StringField(text, "session_id") == "framerift-debug-bridge-test", "Wrong bridge session identity");
    Require(packet.source.sin_family == AF_INET && packet.source.sin_addr.s_addr == htonl(INADDR_LOOPBACK),
        "Bridge source is not IPv4 loopback");
    const auto packetPort = ntohs(packet.source.sin_port);
    Require(packetPort != 0 && (!sourcePort || sourcePort == packetPort), "Bridge changed its source socket");
    sourcePort = packetPort;
    const auto next = IntegerField(text, "sequence");
    Require(next == sequence + 1, "Bridge sequence reset, duplicated or skipped");
    sequence = next;
    Require(text.find("\"frame\":") == std::string::npos, "Connection bridge fabricated a frame counter");
    const auto type = StringField(text, "type");
    Require(type == "hello" || type == "heartbeat" || type == "goodbye", "Connection bridge emitted an unexpected event");
    if (type == "hello") {
        Require(StringField(text, "game_id") == "PPSA90000", "Wrong bridge game identity");
        Require(text.find("\"capabilities\":[]") != std::string::npos, "Connection bridge advertised execution capabilities");
    }
    return type;
}

static void CheckActive(Collector& collector, bool registeredShutdown) {
    using Clock = std::chrono::steady_clock;
    Packet packet;
    std::uint64_t sequence = 0;
    std::uint16_t sourcePort = 0;
    DebugBridgeStart_nid_no_patch();
    Require(collector.Receive(std::chrono::milliseconds(1000), packet), "Bridge did not send its initial hello");
    Require(CheckPacket(packet, sequence, sourcePort) == "hello", "First bridge packet is not hello");
    DebugBridgeStart_nid_no_patch();
    unsigned hellos = 1;
    unsigned heartbeats = 0;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while ((hellos < 2 || heartbeats < 2) && Clock::now() < deadline) {
        if (!collector.Receive(std::chrono::milliseconds(250), packet)) continue;
        const auto type = CheckPacket(packet, sequence, sourcePort);
        Require(type != "goodbye", "Bridge stopped without an explicit stop request");
        if (type == "hello") ++hellos;
        else ++heartbeats;
    }
    Require(hellos >= 2 && heartbeats >= 2, "Bridge did not renew hello and send heartbeats");
    const auto stopping = Clock::now();
    if (registeredShutdown) LibcRunShutdown_nid_postfix();
    else DebugBridgeStop_nid_no_patch();
    Require(Clock::now() - stopping < std::chrono::seconds(1), "Bridge shutdown waited for its next heartbeat");
    bool goodbye = false;
    const auto goodbyeDeadline = Clock::now() + std::chrono::seconds(1);
    while (!goodbye && Clock::now() < goodbyeDeadline) {
        if (!collector.Receive(std::chrono::milliseconds(100), packet)) continue;
        goodbye = CheckPacket(packet, sequence, sourcePort) == "goodbye";
    }
    Require(goodbye, "Bridge did not send goodbye during shutdown");
    DebugBridgeStop_nid_no_patch();
    DebugBridgeStart_nid_no_patch();
    Require(!collector.Receive(std::chrono::milliseconds(1200), packet), "Stopped bridge continued or restarted its worker");
}

int main(int argc, char** argv) {
    try {
        const std::string_view mode = argc > 1 ? argv[1] : "active";
        Require(argc <= 2, "Expected one bridge test mode");
        Require(mode == "active" || mode == "shutdown" || mode == "disabled" || mode == "invalid-port" || mode == "invalid-host" || mode == "absent",
            "Unknown bridge test mode");
        Collector collector;
        for (const auto* name : {"APS5_DEBUG_BRIDGE", "FRAMERIFT_BRIDGE_HOST", "FRAMERIFT_BRIDGE_PORT",
            "FRAMERIFT_BRIDGE_SESSION", "FRAMERIFT_GAME_ID"}) UnsetEnvironment(name);
        if (mode != "absent") {
            SetEnvironment("FRAMERIFT_BRIDGE_HOST", mode == "invalid-host" ? "0.0.0.0" : "127.0.0.1");
            SetEnvironment("FRAMERIFT_BRIDGE_PORT", std::to_string(collector.Port()) + (mode == "invalid-port" ? "trailing" : ""));
            SetEnvironment("FRAMERIFT_BRIDGE_SESSION", "framerift-debug-bridge-test");
            SetEnvironment("FRAMERIFT_GAME_ID", "PPSA90000");
        }
        if (mode == "disabled") SetEnvironment("APS5_DEBUG_BRIDGE", "0");
        if (mode == "active" || mode == "shutdown") CheckActive(collector, mode == "shutdown");
        else {
            DebugBridgeStart_nid_no_patch();
            DebugBridgeStart_nid_no_patch();
            Packet packet;
            Require(!collector.Receive(std::chrono::milliseconds(1200), packet), "Inactive bridge configuration sent UDP traffic");
            DebugBridgeStop_nid_no_patch();
            Require(!collector.Receive(std::chrono::milliseconds(50), packet), "Inactive bridge sent goodbye during stop");
        }
    } catch (const std::exception& error) {
        DebugBridgeStop_nid_no_patch();
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
