#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
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
#include <sys/select.h>
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

int PollNative(std::vector<KernelSocketPoll::Entry>& entries, int timeoutMilliseconds) {
#ifdef _WIN32
    if (!entries.empty()) return -GuestEopnotsupp;
    if (timeoutMilliseconds > 0) std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMilliseconds));
    return 0;
#else
    fd_set readfds;
    fd_set writefds;
    fd_set exceptfds;
    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    FD_ZERO(&exceptfds);
    int nfds = 0;
    for (auto& entry : entries) {
        entry.revents = 0;
        if (::fcntl(entry.descriptor, F_GETFD) < 0) return errno == EBADF ? -GuestEbadf : -5;
        if (entry.events & KernelSocketPoll::Readable) FD_SET(entry.descriptor, &readfds);
        if (entry.events & KernelSocketPoll::Writable) FD_SET(entry.descriptor, &writefds);
        if (entry.events & KernelSocketPoll::Urgent) FD_SET(entry.descriptor, &exceptfds);
        nfds = std::max(nfds, entry.descriptor + 1);
    }
    timeval timeout{timeoutMilliseconds / 1000, (timeoutMilliseconds % 1000) * 1000};
    const int result = ::select(nfds, &readfds, &writefds, &exceptfds, &timeout);
    if (result < 0) {
        switch (errno) {
            case EINTR: return -4;
            case EBADF: return -GuestEbadf;
            case ENOMEM: return -GuestEnomem;
            case EFAULT: return -GuestEfault;
            case EINVAL: return -GuestEinval;
            case EAGAIN: return -35;
            default: return -5;
        }
    }
    for (auto& entry : entries) {
        if (FD_ISSET(entry.descriptor, &readfds)) entry.revents |= KernelSocketPoll::Readable;
        if (FD_ISSET(entry.descriptor, &writefds)) entry.revents |= KernelSocketPoll::Writable;
        if (FD_ISSET(entry.descriptor, &exceptfds)) entry.revents |= KernelSocketPoll::Urgent;
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
    for (;;) {
        const int virtualResult = PollVirtual(poller, virtualEntries, 0);
        if (virtualResult < 0) return fail(-virtualResult);
        const int nativeResult = PollNative(nativeEntries, 0);
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
                const int pauseResult = PollNative(nativeEntries, remaining);
                if (pauseResult < 0) return fail(-pauseResult);
            }
        } else {
            const int result = PollNative(nativeEntries, waitMilliseconds);
            if (result < 0) return fail(-result);
        }
    }
} catch (const std::bad_alloc&) {
    *__error_nid_postfix() = GuestEnomem;
    return -1;
}

}
