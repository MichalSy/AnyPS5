#include "tests/DebugBridgeTestPeer.hpp"
#include <iostream>

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

static std::string CheckBridgePacket(BridgePacket& packet, std::uint64_t& sequence, std::uint16_t& sourcePort) {
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
        Require(text.find("\"capabilities\":[\"set_monitoring\"]") != std::string::npos, "Bridge did not advertise monitoring");
        Require(text.find("\"monitoring\":{\"enabled\":false,\"epoch\":0}") != std::string::npos, "Monitoring was enabled by default");
    }
    return type;
}

static void CheckActive(BridgeCollector& collector, bool registeredShutdown) {
    using Clock = std::chrono::steady_clock;
    BridgePacket packet;
    std::uint64_t sequence = 0;
    std::uint16_t sourcePort = 0;
    DebugBridgeStart_nid_no_patch();
    Require(collector.Receive(std::chrono::milliseconds(1000), packet), "Bridge did not send its initial hello");
    Require(CheckBridgePacket(packet, sequence, sourcePort) == "hello", "First bridge packet is not hello");
    DebugBridgeStart_nid_no_patch();
    unsigned hellos = 1;
    unsigned heartbeats = 0;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while ((hellos < 2 || heartbeats < 2) && Clock::now() < deadline) {
        if (!collector.Receive(std::chrono::milliseconds(250), packet)) continue;
        const auto type = CheckBridgePacket(packet, sequence, sourcePort);
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
        goodbye = CheckBridgePacket(packet, sequence, sourcePort) == "goodbye";
    }
    Require(goodbye, "Bridge did not send goodbye during shutdown");
    DebugBridgeStop_nid_no_patch();
    DebugBridgeStart_nid_no_patch();
    Require(!collector.Receive(std::chrono::milliseconds(1200), packet), "Stopped bridge continued or restarted its worker");
}

static void CheckMonitoring(BridgeCollector& collector) {
    DebugBridgeStart_nid_no_patch();
    BridgePacket hello;
    Require(collector.Receive(std::chrono::milliseconds(1000), hello), "Monitoring bridge did not start");
    auto root = parseJsonDocument(hello.text);
    Require(Member(root, "type", JsonType::String).stringValue == "hello", "First monitoring packet is not hello");
    const auto& capabilities = Member(root, "capabilities", JsonType::Array).arrayValue;
    Require(capabilities.size() == 1 && capabilities[0].stringValue == "set_monitoring", "Wrong monitoring capabilities");
    CheckMonitoringState(Member(root, "monitoring", JsonType::Object), false, 0);
    DebugBridgePublishFrame_nid_no_patch({1, 1, 1, 1, 1000});
    NoEvents(collector, std::chrono::milliseconds(120));

    BridgeCollector rogue;
    rogue.Send(hello.source, Command(1, "rogue", R"({"enabled":true})"));
    collector.Send(hello.source, Command(1, "wrong-session", R"({"enabled":true})", "another-session"));
    collector.Send(hello.source, Command(1, "duplicate-key", R"({"enabled":true,"enabled":false})"));
    collector.Send(hello.source, Command(1, "nested", R"({"enabled":true,"ignored":[[[[[[[[[[]]]]]]]]]]})"));
    collector.Send(hello.source, Command(1, "oversized", R"({"enabled":true,"ignored":")" + std::string(17000, 'x') + "\"}"));
    collector.Send(hello.source, "{bad json");
    BridgePacket rejected;
    const auto rejectingUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(120);
    while (std::chrono::steady_clock::now() < rejectingUntil) {
        if (!collector.Receive(std::chrono::milliseconds(20), rejected)) continue;
        const auto type = Member(parseJsonDocument(rejected.text), "type", JsonType::String).stringValue;
        Require(type == "hello" || type == "heartbeat", "Malformed or untrusted command was acknowledged");
    }
    root = Await(collector, "heartbeat");
    CheckMonitoringState(Member(root, "monitoring", JsonType::Object), false, 0);

    collector.Send(hello.source, Command(1, "enable", R"({"enabled":true})"));
    Await(collector, "command_received", "enable");
    root = Await(collector, "command_completed", "enable");
    CheckMonitoringState(Member(root, "result", JsonType::Object), true, 1);
    DebugBridgePublishFrame_nid_no_patch({523, 1, 7, 20, 100000});
    root = Await(collector, "event");
    Require(Unsigned(root, "frame") == 523 && Member(root, "name", JsonType::String).stringValue == "frame_timing", "Wrong frame correlation");
    auto data = Member(root, "data", JsonType::Object);
    Require(Unsigned(data, "output_handle") == 1 && Unsigned(data, "output_generation") == 7
        && Unsigned(data, "completed_flip_count") == 20 && Unsigned(data, "timestamp_us") == 100000
        && Unsigned(data, "monitoring_epoch") == 1 && Unsigned(data, "dropped_samples") == 0, "Wrong first sample data");
    Member(data, "flip_interval_us", JsonType::Null);
    DebugBridgePublishFrame_nid_no_patch({524, 1, 7, 21, 116667});
    data = Member(Await(collector, "event"), "data", JsonType::Object);
    Require(Unsigned(data, "flip_interval_us") == 16667, "Wrong successful flip interval");

    collector.Send(hello.source, Command(1, "\\u0065nable", R"({"enabled":false})"));
    Await(collector, "command_received", "enable");
    root = Await(collector, "command_completed", "enable");
    CheckMonitoringState(Member(root, "result", JsonType::Object), true, 1);
    collector.Send(hello.source, Command(2, "enable-again", R"({"enabled":true})"));
    Await(collector, "command_received", "enable-again");
    root = Await(collector, "command_completed", "enable-again");
    CheckMonitoringState(Member(root, "result", JsonType::Object), true, 1);
    DebugBridgePublishFrame_nid_no_patch({525, 1, 7, 22, 133334});
    data = Member(Await(collector, "event"), "data", JsonType::Object);
    Require(Unsigned(data, "flip_interval_us") == 16667, "Duplicate command reset the source interval");
    DebugBridgePublishFrame_nid_no_patch({526, 1, 8, 1, 150001});
    data = Member(Await(collector, "event"), "data", JsonType::Object);
    Member(data, "flip_interval_us", JsonType::Null);
    Require(Unsigned(data, "output_generation") == 8, "Output reopen did not reset stream identity");

    collector.Send(hello.source, Command(3, "bad-args", R"({"enabled":"false"})"));
    Await(collector, "command_received", "bad-args");
    root = Await(collector, "command_failed", "bad-args");
    Require(!Member(root, "error", JsonType::String).stringValue.empty(), "Bad argument failure has no error");
    collector.Send(hello.source, Command(4, "unsupported", "{}", "framerift-debug-bridge-test", "capture_frame"));
    Await(collector, "command_received", "unsupported");
    Await(collector, "command_failed", "unsupported");
    root = Await(collector, "heartbeat");
    CheckMonitoringState(Member(root, "monitoring", JsonType::Object), true, 1);

    const auto burstAt = std::chrono::steady_clock::now();
    for (std::uint64_t index = 1; index <= 100000; ++index)
        DebugBridgePublishFrame_nid_no_patch({526 + index, 1, 8, 1 + index, 150001 + index});
    Require(std::chrono::steady_clock::now() - burstAt < std::chrono::seconds(1), "Frame producer blocked behind UDP delivery");
    BridgePacket draining;
    while (collector.Receive(std::chrono::milliseconds(100), draining)) {}
    DebugBridgePublishFrame_nid_no_patch({200000, 1, 8, 200000, 500000});
    root = Await(collector, "event");
    Require(Unsigned(root, "frame") == 200000, "Bounded source queue did not drain");
    data = Member(root, "data", JsonType::Object);
    Require(Unsigned(data, "dropped_samples") > 0, "Full producer queue did not report dropped samples");

    collector.Send(hello.source, Command(5, "disable", R"({"enabled":false})"));
    Await(collector, "command_received", "disable");
    root = Await(collector, "command_completed", "disable");
    CheckMonitoringState(Member(root, "result", JsonType::Object), false, 2);
    DebugBridgePublishFrame_nid_no_patch({200001, 1, 8, 200001, 516667});
    NoEvents(collector, std::chrono::milliseconds(120));
    collector.Send(hello.source, Command(6, "reenable", R"({"enabled":true})"));
    Await(collector, "command_received", "reenable");
    root = Await(collector, "command_completed", "reenable");
    CheckMonitoringState(Member(root, "result", JsonType::Object), true, 3);
    DebugBridgePublishFrame_nid_no_patch({200002, 1, 8, 200002, 533334});
    data = Member(Await(collector, "event"), "data", JsonType::Object);
    Member(data, "flip_interval_us", JsonType::Null);
    Require(Unsigned(data, "monitoring_epoch") == 3 && Unsigned(data, "dropped_samples") == 0, "Reactivation retained old source statistics");
    collector.Send(hello.source, Command(2, "stale", R"({"enabled":false})"));
    Await(collector, "command_received", "stale");
    Await(collector, "command_failed", "stale");
    root = Await(collector, "heartbeat");
    CheckMonitoringState(Member(root, "monitoring", JsonType::Object), true, 3);
    DebugBridgeStop_nid_no_patch();
}

int main(int argc, char** argv) {
    try {
        const std::string_view mode = argc > 1 ? argv[1] : "active";
        Require(argc <= 2, "Expected one bridge test mode");
        Require(mode == "monitoring" || mode == "active" || mode == "shutdown" || mode == "disabled" || mode == "invalid-port" || mode == "invalid-host" || mode == "absent",
            "Unknown bridge test mode");
        BridgeCollector collector;
        for (const auto* name : {"APS5_DEBUG_BRIDGE", "FRAMERIFT_BRIDGE_HOST", "FRAMERIFT_BRIDGE_PORT",
            "FRAMERIFT_BRIDGE_SESSION", "FRAMERIFT_GAME_ID"}) UnsetEnvironment(name);
        if (mode != "absent") {
            SetEnvironment("FRAMERIFT_BRIDGE_HOST", mode == "invalid-host" ? "0.0.0.0" : "127.0.0.1");
            SetEnvironment("FRAMERIFT_BRIDGE_PORT", std::to_string(collector.Port()) + (mode == "invalid-port" ? "trailing" : ""));
            SetEnvironment("FRAMERIFT_BRIDGE_SESSION", "framerift-debug-bridge-test");
            SetEnvironment("FRAMERIFT_GAME_ID", "PPSA90000");
        }
        if (mode == "disabled") SetEnvironment("APS5_DEBUG_BRIDGE", "0");
        if (mode == "monitoring") CheckMonitoring(collector);
        else if (mode == "active" || mode == "shutdown") CheckActive(collector, mode == "shutdown");
        else {
            DebugBridgeStart_nid_no_patch();
            DebugBridgeStart_nid_no_patch();
            BridgePacket packet;
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
