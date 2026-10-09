#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libc/include/GuestFileDescriptors.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libkernel/Socket/include/SocketPoll.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <new>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#endif

namespace {

constexpr int GuestEbadf = 9;
constexpr int GuestEnomem = 12;
constexpr int GuestEfault = 14;
constexpr int GuestEinval = 22;
constexpr int GuestEopnotsupp = 45;
constexpr int SelectDescriptorLimit = 1024;

struct GuestTimeval {
    std::int64_t seconds;
    std::int64_t microseconds;
};
static_assert(sizeof(GuestTimeval) == 16);

std::atomic<KernelSocketPoll::Poller> g_socketPoller{nullptr};

bool DescriptorSet(const void* set, int descriptor) {
    return set != nullptr && ((static_cast<const std::uint64_t*>(set)[descriptor / 64] >> (descriptor % 64)) & 1u) != 0;
}

void ClearDescriptor(void* set, int descriptor) {
    if (set != nullptr) static_cast<std::uint64_t*>(set)[descriptor / 64] &= ~(std::uint64_t{1} << (descriptor % 64));
}

int ReadyCount(const std::vector<KernelSocketPoll::Entry>& entries) {
    int ready = 0;
    for (const auto& entry : entries) {
        if ((entry.events & KernelSocketPoll::Readable) &&
            (entry.revents & (KernelSocketPoll::Readable | KernelSocketPoll::HangUp | KernelSocketPoll::Error))) ++ready;
        if ((entry.events & KernelSocketPoll::Writable) &&
            (entry.revents & (KernelSocketPoll::Writable | KernelSocketPoll::Error))) ++ready;
        if ((entry.events & KernelSocketPoll::Urgent) && (entry.revents & KernelSocketPoll::Urgent)) ++ready;
    }
    return ready;
}

void FilterDescriptors(const std::vector<KernelSocketPoll::Entry>& entries, void* readfds, void* writefds, void* exceptfds) {
    for (const auto& entry : entries) {
        if ((entry.revents & (KernelSocketPoll::Readable | KernelSocketPoll::HangUp | KernelSocketPoll::Error)) == 0)
            ClearDescriptor(readfds, entry.descriptor);
        if ((entry.revents & (KernelSocketPoll::Writable | KernelSocketPoll::Error)) == 0)
            ClearDescriptor(writefds, entry.descriptor);
        if ((entry.revents & KernelSocketPoll::Urgent) == 0) ClearDescriptor(exceptfds, entry.descriptor);
    }
}

int PollVirtual(KernelSocketPoll::Poller poller, std::vector<KernelSocketPoll::Entry>& entries, int timeoutMilliseconds) {
    if (entries.empty()) return 0;
    for (auto& entry : entries) entry.revents = 0;
    const int result = poller(entries.data(), static_cast<int>(entries.size()), timeoutMilliseconds);
    if (result < 0) return result;
    for (const auto& entry : entries) {
        if (entry.revents & KernelSocketPoll::Unknown) return -GuestEbadf;
    }
    return result;
}

int PollNative(std::vector<KernelSocketPoll::Entry>& entries, const std::vector<int>& descriptors, int timeoutMilliseconds) {
#ifdef _WIN32
    if (!entries.empty()) return -GuestEopnotsupp;
    if (timeoutMilliseconds > 0) std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMilliseconds));
    return 0;
#else
    std::vector<pollfd> native;
    native.reserve(entries.size());
    for (std::size_t index = 0; index < entries.size(); ++index) {
        auto& entry = entries[index];
        entry.revents = 0;
        short events = 0;
        if (entry.events & KernelSocketPoll::Readable) events |= POLLIN;
        if (entry.events & KernelSocketPoll::Writable) events |= POLLOUT;
        if (entry.events & KernelSocketPoll::Urgent) events |= POLLPRI;
        native.push_back({descriptors[index], events, 0});
    }
    const auto started = std::chrono::steady_clock::now();
    const auto errorResult = [] { return -GuestFiles::GuestFileNativeError_nid_no_patch(errno); };
    const int result = ::poll(native.data(), static_cast<nfds_t>(native.size()), timeoutMilliseconds);
    if (result < 0) return errorResult();
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (native[index].revents & POLLNVAL) return -GuestEbadf;
        if (native[index].revents & POLLIN) entries[index].revents |= KernelSocketPoll::Readable;
        if (native[index].revents & POLLOUT) entries[index].revents |= KernelSocketPoll::Writable;
        if (native[index].revents & POLLPRI) entries[index].revents |= KernelSocketPoll::Urgent;
        if (native[index].revents & POLLHUP) entries[index].revents |= KernelSocketPoll::HangUp;
        if (native[index].revents & POLLERR) entries[index].revents |= KernelSocketPoll::Error;
    }
    if (result > 0 && ReadyCount(entries) == 0 && timeoutMilliseconds > 0) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        const auto remaining = static_cast<int>(std::max<std::int64_t>(0, timeoutMilliseconds - elapsed));
        if (remaining > 0 && ::poll(nullptr, 0, remaining) < 0) return errorResult();
    }
    return result;
#endif
}

}

extern "C" {

const char* APS5_VABI __inet_ntop_nid_postfix(int family, const void* source, char* destination, std::uint32_t capacity);
int APS5_VABI __inet_pton_nid_postfix(int family, const char* text, void* destination);

const char* APS5_VABI inet_ntop_nid_postfix(int af, const void* src, char* dst, uint32_t size) {
    return __inet_ntop_nid_postfix(af, src, dst, size);
}

int APS5_VABI inet_pton_nid_postfix(int af, const char* src, void* dst) {
    return __inet_pton_nid_postfix(af, src, dst);
}

int* APS5_VABI __error_nid_postfix();

void KernelSetSocketPoller_nid_no_patch(KernelSocketPoll::Poller poller) {
    g_socketPoller.store(poller);
}

int APS5_VABI select_nid_postfix(int nfds, void* readfds, void* writefds, void* exceptfds, const void* timeout) try {
    const int savedErrno = *__error_nid_postfix();
    const auto fail = [](int error) { *__error_nid_postfix() = error; return -1; };
    const auto* limit = static_cast<const GuestTimeval*>(timeout);
    if (nfds < 0 || nfds > SelectDescriptorLimit || (limit != nullptr && (limit->seconds < 0 || limit->microseconds < 0 || limit->microseconds >= 1000000))) {
        return fail(GuestEinval);
    }
    const auto start = std::chrono::steady_clock::now();
    auto deadline = std::chrono::steady_clock::time_point::max();
    if (limit != nullptr) {
        const auto maximum = std::chrono::duration_cast<std::chrono::microseconds>(deadline - start).count();
        if (limit->seconds > maximum / 1000000 ||
            (limit->seconds == maximum / 1000000 && limit->microseconds > maximum % 1000000)) return fail(GuestEinval);
        deadline = start + std::chrono::microseconds(limit->seconds * 1000000 + limit->microseconds);
    }
    std::vector<KernelSocketPoll::Entry> entries;
    for (int descriptor = 0; descriptor < nfds; ++descriptor) {
        short events = 0;
        if (DescriptorSet(readfds, descriptor)) events |= KernelSocketPoll::Readable;
        if (DescriptorSet(writefds, descriptor)) events |= KernelSocketPoll::Writable;
        if (DescriptorSet(exceptfds, descriptor)) events |= KernelSocketPoll::Urgent;
        if (events != 0) entries.push_back({descriptor, events, 0});
    }
    const auto poller = g_socketPoller.load();
    if (poller != nullptr && !entries.empty()) {
        const int result = poller(entries.data(), static_cast<int>(entries.size()), 0);
        if (result < 0) return fail(-result);
    } else {
        for (auto& entry : entries) entry.revents = KernelSocketPoll::Unknown;
    }
    std::vector<KernelSocketPoll::Entry> virtualEntries;
    std::vector<KernelSocketPoll::Entry> nativeEntries;
    for (const auto& entry : entries) {
        if (entry.revents & KernelSocketPoll::Unknown) nativeEntries.push_back(entry);
        else virtualEntries.push_back(entry);
    }
    std::vector<GuestFiles::Lease> nativeLeases;
    std::vector<int> nativeDescriptors;
    nativeLeases.reserve(nativeEntries.size());
    nativeDescriptors.reserve(nativeEntries.size());
    for (const auto& entry : nativeEntries) {
        auto lease = GuestFiles::GuestFileAcquire_nid_no_patch(entry.descriptor);
        if (!lease) return fail(GuestEbadf);
        nativeDescriptors.push_back(GuestFiles::GuestFileNativeDescriptor_nid_no_patch(lease));
        nativeLeases.push_back(std::move(lease));
    }
    for (;;) {
        const int virtualResult = PollVirtual(poller, virtualEntries, 0);
        if (virtualResult < 0) return fail(-virtualResult);
        const int nativeResult = PollNative(nativeEntries, nativeDescriptors, 0);
        if (nativeResult < 0) return fail(-nativeResult);
        const int ready = ReadyCount(virtualEntries) + ReadyCount(nativeEntries);
        if (ready > 0 || (limit != nullptr && std::chrono::steady_clock::now() >= deadline)) {
            const std::size_t bytes = static_cast<std::size_t>((nfds + 63) / 64) * sizeof(std::uint64_t);
            std::uint64_t reads[16]{};
            std::uint64_t writes[16]{};
            std::uint64_t exceptions[16]{};
            if (readfds != nullptr) std::memcpy(reads, readfds, bytes);
            if (writefds != nullptr) std::memcpy(writes, writefds, bytes);
            if (exceptfds != nullptr) std::memcpy(exceptions, exceptfds, bytes);
            FilterDescriptors(virtualEntries, reads, writes, exceptions);
            FilterDescriptors(nativeEntries, reads, writes, exceptions);
            {
                const GuestArena::HostWrite readDestination(readfds, readfds != nullptr ? bytes : 0);
                const GuestArena::HostWrite writeDestination(writefds, writefds != nullptr ? bytes : 0);
                const GuestArena::HostWrite exceptDestination(exceptfds, exceptfds != nullptr ? bytes : 0);
                if (!readDestination.Open() || !writeDestination.Open() || !exceptDestination.Open()) return fail(GuestEfault);
                if (readfds != nullptr) std::memcpy(readfds, reads, bytes);
                if (writefds != nullptr) std::memcpy(writefds, writes, bytes);
                if (exceptfds != nullptr) std::memcpy(exceptfds, exceptions, bytes);
            }
            *__error_nid_postfix() = savedErrno;
            return ready;
        }
        int waitMilliseconds = 100;
        if (limit != nullptr) {
            const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - std::chrono::steady_clock::now()).count();
            waitMilliseconds = static_cast<int>(std::clamp<std::int64_t>((remaining + 999) / 1000, 0, 100));
        }
        if (!virtualEntries.empty() && nativeEntries.empty()) {
            const auto waitStart = std::chrono::steady_clock::now();
            const int result = PollVirtual(poller, virtualEntries, waitMilliseconds);
            if (result < 0) return fail(-result);
            if (ReadyCount(virtualEntries) == 0) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - waitStart).count();
                const int remaining = static_cast<int>(std::max<std::int64_t>(0, waitMilliseconds - elapsed));
                const int pauseResult = PollNative(nativeEntries, nativeDescriptors, remaining);
                if (pauseResult < 0) return fail(-pauseResult);
            }
        } else {
            const int result = PollNative(nativeEntries, nativeDescriptors, waitMilliseconds);
            if (result < 0) return fail(-result);
        }
    }
} catch (const std::bad_alloc&) {
    *__error_nid_postfix() = GuestEnomem;
    return -1;
}

}
