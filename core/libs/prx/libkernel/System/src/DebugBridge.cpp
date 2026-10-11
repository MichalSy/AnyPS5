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
#include "prx/libkernel/System/include/JsonParser.hpp"
#include "prx/libc/include/Shutdown.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
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
class DebugBridge;
std::atomic<DebugBridge*> activeBridge{nullptr};
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

constexpr auto unavailable = std::numeric_limits<std::uint64_t>::max();
constexpr std::array shaderNames{
    "translations_active", "translations_completed", "cached_variants_count", "source_code_bytes",
    "variant_spirv_bytes", "disk_cache_hits", "disk_cache_misses", "disk_cache_writes",
    "disk_cache_load_failures", "disk_cache_write_failures", "preparation_queue_pending", "preparation_active",
    "pipeline_builds_active", "pipeline_builds_completed", "pipeline_optimization_queue_pending", "pipeline_optimizations_active"
};
static_assert(shaderNames.size() == static_cast<std::size_t>(DebugBridgeShaderMetric::Count));
constexpr std::array memoryNames{
    "process_rss_bytes", "process_rss_timestamp_us", "sampled_cache_bytes", "sampled_cache_budget_bytes",
    "storage_cache_bytes", "storage_cache_budget_bytes", "gpu_heap_usage_bytes", "gpu_heap_budget_bytes", "gpu_heap_timestamp_us"
};

struct ResourceCounters {
    std::array<std::atomic<std::uint64_t>, shaderNames.size()> shaders{};
    std::atomic<std::uint64_t> sampledBytes{unavailable};
    std::atomic<std::uint64_t> sampledBudget{unavailable};
    std::atomic<std::uint64_t> storageBytes{unavailable};
    std::atomic<std::uint64_t> storageBudget{unavailable};
    std::atomic<std::uint64_t> heapUsage{unavailable};
    std::atomic<std::uint64_t> heapBudget{unavailable};
    std::atomic<std::uint64_t> heapTimestamp{unavailable};
};

constinit ResourceCounters resourceCounters;

struct ResourceSnapshot {
    std::array<std::uint64_t, shaderNames.size()> shaders{};
    std::array<std::uint64_t, memoryNames.size()> memory{};
    bool operator==(const ResourceSnapshot&) const = default;
};

std::uint64_t TimestampUs(std::chrono::steady_clock::time_point time) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(time.time_since_epoch()).count());
}

std::uint64_t ProcessRss() noexcept {
#ifdef __linux__
    static const auto pageBytes = ::sysconf(_SC_PAGESIZE);
    if (pageBytes <= 0) return unavailable;
    auto* file = std::fopen("/proc/self/statm", "r");
    if (!file) return unavailable;
    unsigned long long virtualPages = 0;
    unsigned long long residentPages = 0;
    const auto fields = std::fscanf(file, "%llu %llu", &virtualPages, &residentPages);
    std::fclose(file);
    if (fields != 2 || residentPages > unavailable / static_cast<std::uint64_t>(pageBytes)) return unavailable;
    return residentPages * static_cast<std::uint64_t>(pageBytes);
#else
    return unavailable;
#endif
}

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

    bool Receive(std::uint16_t port, std::string& packet) {
        std::array<char, 16385> buffer{};
        sockaddr_in source{};
#ifdef _WIN32
        int addressSize = sizeof(source);
        const auto received = ::recvfrom(handle, buffer.data(), static_cast<int>(buffer.size()), 0,
            reinterpret_cast<sockaddr*>(&source), &addressSize);
#else
        socklen_t addressSize = sizeof(source);
        const auto received = ::recvfrom(handle, buffer.data(), buffer.size(), 0,
            reinterpret_cast<sockaddr*>(&source), &addressSize);
#endif
        packet.clear();
        if (received < 0) {
            const auto error = SocketError();
#ifdef _WIN32
            if (error == WSAEMSGSIZE) return true;
#endif
            if (DroppedDatagram(error)) return false;
            throw std::system_error(error, std::system_category(), "receiving UDP packet");
        }
        if (received == 0 || received > 16384 || addressSize != sizeof(source)
            || source.sin_family != AF_INET || source.sin_addr.s_addr != htonl(INADDR_LOOPBACK)
            || ntohs(source.sin_port) != port) return true;
        packet.assign(buffer.data(), static_cast<std::size_t>(received));
        return true;
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
        sessionText = session;
        gameId = QuoteId(game, "FRAMERIFT_GAME_ID must contain 1 to 128 bytes of valid UTF-8");
        LibcRegisterShutdown_nid_postfix(DebugBridgeStop_nid_no_patch);
        worker = std::thread([this] { run(); });
        activeBridge.store(this, std::memory_order_release);
    }

    void Stop() noexcept {
        try {
            std::lock_guard lock(lifecycleMutex);
            stopped = true;
            activeBridge.store(nullptr, std::memory_order_release);
            monitoring.fetch_and(~std::uint64_t{1}, std::memory_order_acq_rel);
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

    void Publish(const DebugBridgeFrame& frame) noexcept {
        const auto state = monitoring.load(std::memory_order_acquire);
        if (!(state & 1) || stopping.load(std::memory_order_relaxed)) return;
        const auto epoch = state >> 1;
        if (producerEpoch != epoch) {
            producerEpoch = epoch;
            previousTimestamp = 0;
            droppedSamples = 0;
        }
        if (previousHandle != frame.outputHandle || previousGeneration != frame.outputGeneration) previousTimestamp = 0;
        Sample sample{frame, epoch, droppedSamples, 0, false};
        if (previousTimestamp != 0 && frame.timestampUs > previousTimestamp) {
            sample.intervalUs = frame.timestampUs - previousTimestamp;
            sample.hasInterval = true;
        }
        previousHandle = frame.outputHandle;
        previousGeneration = frame.outputGeneration;
        previousTimestamp = frame.timestampUs;
        const auto write = queueWrite.load(std::memory_order_relaxed);
        if (write - queueRead.load(std::memory_order_acquire) == samples.size()) {
            if (droppedSamples < std::numeric_limits<std::uint64_t>::max()) ++droppedSamples;
            return;
        }
        samples[write % samples.size()] = sample;
        queueWrite.store(write + 1, std::memory_order_release);
    }

private:
    struct Sample {
        DebugBridgeFrame frame;
        std::uint64_t epoch = 0;
        std::uint64_t dropped = 0;
        std::uint64_t intervalUs = 0;
        bool hasInterval = false;
    };

    struct Command {
        std::string id;
        std::string outcome;
        bool completed = false;
    };

    std::string envelope(const char* type) {
        if (sequence == static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            throw std::runtime_error("bridge sequence exhausted");
        return std::string("{\"protocol\":\"framerift.bridge\",\"version\":1,\"type\":\"") + type
            + "\",\"session_id\":" + sessionId + ",\"sequence\":" + std::to_string(++sequence);
    }

    std::string monitoringJson() const {
        const auto state = monitoring.load(std::memory_order_acquire);
        return std::string("{\"enabled\":") + ((state & 1) ? "true" : "false") + ",\"epoch\":" + std::to_string(state >> 1) + '}';
    }

    std::string packet(const char* type) {
        auto result = envelope(type);
        if (std::strcmp(type, "hello") == 0)
            result += ",\"game_id\":" + gameId + ",\"capabilities\":[\"set_monitoring\",\"resource_status\"]";
        if (std::strcmp(type, "hello") == 0 || std::strcmp(type, "heartbeat") == 0)
            result += ",\"monitoring\":" + monitoringJson();
        return result + '}';
    }

    std::string event(const Sample& sample) {
        const auto& frame = sample.frame;
        return envelope("event") + ",\"name\":\"frame_timing\",\"frame\":" + std::to_string(frame.frame)
            + ",\"data\":{\"output_handle\":" + std::to_string(frame.outputHandle)
            + ",\"output_generation\":" + std::to_string(frame.outputGeneration)
            + ",\"completed_flip_count\":" + std::to_string(frame.completedFlipCount)
            + ",\"timestamp_us\":" + std::to_string(frame.timestampUs)
            + ",\"flip_interval_us\":" + (sample.hasInterval ? std::to_string(sample.intervalUs) : "null")
            + ",\"monitoring_epoch\":" + std::to_string(sample.epoch)
            + ",\"dropped_samples\":" + std::to_string(sample.dropped) + "}}";
    }

    void resources(DatagramSocket& socket, std::chrono::steady_clock::time_point now) {
        const auto state = monitoring.load(std::memory_order_acquire);
        if (!(state & 1)) {
            hasResourceSnapshot = false;
            return;
        }
        const auto epoch = state >> 1;
        if (now >= nextRssAt) {
            processRss = ProcessRss();
            processRssTimestamp = processRss == unavailable ? unavailable : TimestampUs(now);
            nextRssAt = now + std::chrono::seconds(1);
        }
        ResourceSnapshot snapshot;
        for (std::size_t index = 0; index < snapshot.shaders.size(); ++index)
            snapshot.shaders[index] = resourceCounters.shaders[index].load(std::memory_order_relaxed);
        const auto heapTimestamp = resourceCounters.heapTimestamp.load(std::memory_order_acquire);
        snapshot.memory = {
            processRss, processRssTimestamp,
            resourceCounters.sampledBytes.load(std::memory_order_relaxed), resourceCounters.sampledBudget.load(std::memory_order_relaxed),
            resourceCounters.storageBytes.load(std::memory_order_relaxed), resourceCounters.storageBudget.load(std::memory_order_relaxed),
            heapTimestamp == unavailable ? unavailable : resourceCounters.heapUsage.load(std::memory_order_relaxed),
            heapTimestamp == unavailable ? unavailable : resourceCounters.heapBudget.load(std::memory_order_relaxed), heapTimestamp
        };
        const bool initial = !hasResourceSnapshot || resourceEpoch != epoch;
        if (!initial && (now - resourceSentAt < std::chrono::milliseconds(250)
            || (snapshot == lastResourceSnapshot && now - resourceSentAt < std::chrono::seconds(5)))) return;
        auto packet = envelope("event") + ",\"name\":\"resource_status\",\"data\":{\"timestamp_us\":"
            + std::to_string(TimestampUs(std::chrono::steady_clock::now())) + ",\"monitoring_epoch\":" + std::to_string(epoch) + ",\"shaders\":{";
        for (std::size_t index = 0; index < snapshot.shaders.size(); ++index) {
            if (index != 0) packet += ',';
            packet += std::string("\"") + shaderNames[index] + "\":" + std::to_string(snapshot.shaders[index]);
        }
        packet += "},\"memory\":{";
        for (std::size_t index = 0; index < snapshot.memory.size(); ++index) {
            if (index != 0) packet += ',';
            packet += std::string("\"") + memoryNames[index] + "\":"
                + (snapshot.memory[index] == unavailable ? "null" : std::to_string(snapshot.memory[index]));
        }
        packet += "}}}";
        socket.Send(port, packet);
        resourceSentAt = now;
        resourceEpoch = epoch;
        lastResourceSnapshot = snapshot;
        hasResourceSnapshot = true;
    }

    static const JsonValue& member(const JsonValue& object, const char* name, JsonType type) {
        const auto* value = findObjectMember(object, name);
        if (!value || value->type != type) throw std::invalid_argument("invalid bridge command member");
        return *value;
    }

    static std::uint64_t unsignedValue(const JsonValue& value) {
        if (value.type != JsonType::Number) throw std::invalid_argument("invalid bridge command integer");
        std::uint64_t result = 0;
        const auto& text = value.numberText;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()
            || result > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            throw std::invalid_argument("invalid bridge command integer");
        return result;
    }

    void command(DatagramSocket& socket, const std::string& text) {
        JsonValue root;
        std::string id;
        std::string name;
        std::uint64_t commandSequence = 0;
        try {
            if (text.empty() || !ValidUtf8(text)) return;
            root = parseJsonDocument(text, 8, true);
            if (member(root, "protocol", JsonType::String).stringValue != "framerift.bridge"
                || unsignedValue(member(root, "version", JsonType::Number)) != 1
                || member(root, "type", JsonType::String).stringValue != "command"
                || member(root, "session_id", JsonType::String).stringValue != sessionText) return;
            commandSequence = unsignedValue(member(root, "sequence", JsonType::Number));
            id = member(root, "command_id", JsonType::String).stringValue;
            if (id.empty() || id.size() > 128 || id.find('\0') != std::string::npos) return;
            name = member(root, "name", JsonType::String).stringValue;
            member(root, "args", JsonType::Object);
        } catch (const std::exception&) {
            return;
        }
        const auto quotedId = QuoteId(id.c_str(), "invalid command identity");
        socket.Send(port, envelope("command_received") + ",\"command_id\":" + quotedId + '}');
        for (const auto& previous : commands) {
            if (previous.id == id) {
                socket.Send(port, envelope(previous.completed ? "command_completed" : "command_failed")
                    + ",\"command_id\":" + quotedId + previous.outcome + '}');
                return;
            }
        }
        std::string outcome;
        bool completed = false;
        if (commandSequence <= lastCommandSequence) {
            outcome = ",\"error\":\"Command sequence is stale or its history expired\"";
        } else if (name != "set_monitoring") {
            outcome = ",\"error\":\"Unsupported command\"";
        } else {
            const auto& args = member(root, "args", JsonType::Object);
            const auto* enabled = findObjectMember(args, "enabled");
            if (args.objectValue.size() != 1 || !enabled || enabled->type != JsonType::Boolean) {
                outcome = ",\"error\":\"set_monitoring requires exactly one boolean enabled argument\"";
            } else {
                const auto previous = monitoring.load(std::memory_order_acquire);
                auto next = previous;
                if (static_cast<bool>(previous & 1) != enabled->boolValue) {
                    if ((previous >> 1) == (std::numeric_limits<std::uint64_t>::max() >> 1))
                        throw std::runtime_error("monitoring epoch exhausted");
                    next = (((previous >> 1) + 1) << 1) | static_cast<std::uint64_t>(enabled->boolValue);
                    monitoring.store(next, std::memory_order_release);
                }
                outcome = ",\"result\":" + monitoringJson();
                completed = true;
            }
        }
        lastCommandSequence = std::max(lastCommandSequence, commandSequence);
        commands[commandWrite++ % commands.size()] = {id, outcome, completed};
        socket.Send(port, envelope(completed ? "command_completed" : "command_failed")
            + ",\"command_id\":" + quotedId + outcome + '}');
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
            while (!stopping.load(std::memory_order_acquire)) {
                std::string incoming;
                for (unsigned count = 0; count < 32 && socket.Receive(port, incoming); ++count) command(socket, incoming);
                for (unsigned count = 0; count < 64; ++count) {
                    const auto read = queueRead.load(std::memory_order_relaxed);
                    if (read == queueWrite.load(std::memory_order_acquire)) break;
                    const auto sample = samples[read % samples.size()];
                    queueRead.store(read + 1, std::memory_order_release);
                    const auto state = monitoring.load(std::memory_order_acquire);
                    if ((state & 1) && sample.epoch == (state >> 1)) socket.Send(port, event(sample));
                }
                if (stopping.load(std::memory_order_acquire)) break;
                const auto now = Clock::now();
                resources(socket, now);
                if (now >= helloAt) {
                    socket.Send(port, packet("hello"));
                    helloAt = now + std::chrono::seconds(3);
                }
                if (now >= heartbeatAt) {
                    socket.Send(port, packet("heartbeat"));
                    heartbeatAt = now + std::chrono::seconds(1);
                }
                std::unique_lock lock(waitMutex);
                changed.wait_until(lock, std::min({heartbeatAt, helloAt, now + std::chrono::milliseconds(25)}), [this] {
                    return stopping.load(std::memory_order_acquire)
                        || queueRead.load(std::memory_order_relaxed) != queueWrite.load(std::memory_order_acquire);
                });
            }
            socket.Send(port, packet("goodbye"));
        } catch (const std::exception& error) {
            Diagnose(error.what());
        } catch (...) {
            Diagnose("bridge worker failed");
        }
        monitoring.fetch_and(~std::uint64_t{1}, std::memory_order_acq_rel);
    }

    std::mutex lifecycleMutex;
    std::mutex waitMutex;
    std::condition_variable changed;
    std::thread worker;
    std::atomic<bool> stopping{false};
    std::atomic<std::uint64_t> monitoring{0};
    std::array<Sample, 256> samples{};
    std::atomic<std::uint64_t> queueRead{0};
    std::atomic<std::uint64_t> queueWrite{0};
    std::uint64_t producerEpoch = 0;
    std::uint64_t previousTimestamp = 0;
    std::uint32_t previousHandle = 0;
    std::uint64_t previousGeneration = 0;
    std::uint64_t droppedSamples = 0;
    std::array<Command, 128> commands{};
    std::size_t commandWrite = 0;
    std::uint64_t lastCommandSequence = 0;
    ResourceSnapshot lastResourceSnapshot;
    std::chrono::steady_clock::time_point resourceSentAt{};
    std::chrono::steady_clock::time_point nextRssAt{};
    std::uint64_t resourceEpoch = 0;
    std::uint64_t processRss = unavailable;
    std::uint64_t processRssTimestamp = unavailable;
    bool hasResourceSnapshot = false;
    bool started = false;
    bool stopped = false;
    std::uint16_t port = 0;
    std::uint64_t sequence = 0;
    std::string sessionId;
    std::string sessionText;
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

extern "C" void DebugBridgePublishFrame_nid_no_patch(const DebugBridgeFrame& frame) noexcept {
    if (auto* bridge = activeBridge.load(std::memory_order_acquire)) bridge->Publish(frame);
}

extern "C" void DebugBridgeAddShaderMetric_nid_no_patch(DebugBridgeShaderMetric metric, std::int64_t delta) noexcept {
    const auto index = static_cast<std::size_t>(metric);
    if (index < resourceCounters.shaders.size())
        resourceCounters.shaders[index].fetch_add(static_cast<std::uint64_t>(delta), std::memory_order_relaxed);
}

extern "C" void DebugBridgePublishSampledCache_nid_no_patch(std::uint64_t bytes, std::uint64_t budgetBytes) noexcept {
    resourceCounters.sampledBytes.store(bytes, std::memory_order_relaxed);
    if (budgetBytes != 0) resourceCounters.sampledBudget.store(budgetBytes, std::memory_order_relaxed);
}

extern "C" void DebugBridgePublishStorageCache_nid_no_patch(std::uint64_t bytes, std::uint64_t budgetBytes) noexcept {
    resourceCounters.storageBytes.store(bytes, std::memory_order_relaxed);
    if (budgetBytes != 0) resourceCounters.storageBudget.store(budgetBytes, std::memory_order_relaxed);
}

extern "C" void DebugBridgePublishGpuHeap_nid_no_patch(std::uint64_t usage, std::uint64_t budget, std::uint64_t timestampUs) noexcept {
    resourceCounters.heapUsage.store(usage, std::memory_order_relaxed);
    resourceCounters.heapBudget.store(budget, std::memory_order_relaxed);
    resourceCounters.heapTimestamp.store(timestampUs, std::memory_order_release);
}
