#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libkernel/Socket/include/SocketPoll.hpp"
#include "prx/libc/include/GuestFileDescriptors.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <csignal>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

extern "C" {
int APS5_VABI select_nid_postfix(int, void*, void*, void*, const void*);
int* APS5_VABI __error_nid_postfix();
int APS5_VABI pipe_nid_postfix(int*);
int APS5_VABI sceKernelClose(int);
std::int64_t APS5_VABI sceKernelRead(int, void*, std::size_t);
std::int64_t APS5_VABI sceKernelWrite(int, const void*, std::size_t);
}

using DescriptorSet = std::array<std::uint64_t, 16>;
using Timeval = std::array<std::int64_t, 2>;
static_assert(sizeof(DescriptorSet) == 128 && sizeof(Timeval) == 16);

static void Require(bool value, const char* message) {
    if (value) return;
    std::fprintf(stderr, "guest select test failed: %s\n", message);
    std::abort();
}

static void Set(DescriptorSet& set, int descriptor) {
    Require(descriptor >= 0 && descriptor < 1024, "descriptor fits guest fd_set");
    set[descriptor / 64] |= std::uint64_t{1} << (descriptor % 64);
}

static bool IsSet(const DescriptorSet& set, int descriptor) {
    return ((set[descriptor / 64] >> (descriptor % 64)) & 1u) != 0;
}

constexpr int VirtualDescriptor = 1000;
static std::atomic<short> virtualReadiness{0};
static std::atomic<int> virtualError{0};

static int PollVirtual(KernelSocketPoll::Entry* entries, int count, int) {
    if (const int error = virtualError.load()) return -error;
    int ready = 0;
    for (int index = 0; index < count; ++index) {
        auto& entry = entries[index];
        entry.revents = entry.descriptor == VirtualDescriptor ? virtualReadiness.load() : KernelSocketPoll::Unknown;
        if (entry.descriptor == VirtualDescriptor && entry.revents != 0) ++ready;
    }
    return ready;
}

#ifndef _WIN32
static void Interrupt(int) {}
#endif

int main() {
    KernelSetSocketPoller_nid_no_patch(nullptr);
    const Timeval pollNow{0, 0};
    const Timeval shortWait{0, 30000};
    const auto start = std::chrono::steady_clock::now();
    *__error_nid_postfix() = 13;
    Require(select_nid_postfix(0, nullptr, nullptr, nullptr, shortWait.data()) == 0, "empty set waits for timeout");
    Require(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(20), "timeout does not return early");
    Require(shortWait == Timeval{0, 30000} && *__error_nid_postfix() == 13, "successful wait preserves timeval and errno");
    DescriptorSet reads{};
    Set(reads, 1023);
    const auto original = reads;
    const Timeval badMicroseconds{0, 1000000};
    const Timeval negativeSeconds{-1, 0};
    const Timeval negativeMicroseconds{0, -1};
    const Timeval excessiveSeconds{std::numeric_limits<std::int64_t>::max(), 0};
    for (const auto& timeout : {badMicroseconds, negativeSeconds, negativeMicroseconds, excessiveSeconds}) {
        Require(select_nid_postfix(1024, reads.data(), nullptr, nullptr, timeout.data()) == -1 &&
            *__error_nid_postfix() == 22 && reads == original, "invalid timeout preserves descriptor set");
    }
    Require(select_nid_postfix(-1, nullptr, nullptr, nullptr, pollNow.data()) == -1 && *__error_nid_postfix() == 22,
        "negative descriptor count is invalid");
    Require(select_nid_postfix(0x10000001, nullptr, nullptr, nullptr, pollNow.data()) == -1 && *__error_nid_postfix() == 22,
        "high virtual GuestSockets cannot be represented by fd_set");
    reads = {};
    Set(reads, 100);
    Require(select_nid_postfix(1, reads.data(), nullptr, nullptr, pollNow.data()) == 0 && IsSet(reads, 100),
        "bits beyond nfds remain untouched");

    int pipe[2];
#ifdef _WIN32
    Require(pipe_nid_postfix(pipe) == 0, "create guest pipe");
    reads = {};
    Set(reads, pipe[0]);
    const auto nativeReads = reads;
    Require(select_nid_postfix(pipe[0] + 1, reads.data(), nullptr, nullptr, pollNow.data()) == -1 &&
        *__error_nid_postfix() == 45 && reads == nativeReads, "Windows CRT descriptors are explicitly unsupported");
    Require(sceKernelClose(pipe[0]) == 0 && sceKernelClose(pipe[1]) == 0, "close guest pipe");
#else
    Require(pipe_nid_postfix(pipe) == 0, "create guest pipe");
    reads = {};
    Set(reads, pipe[0]);
    Require(select_nid_postfix(pipe[0] + 1, reads.data(), nullptr, nullptr, pollNow.data()) == 0 &&
        !IsSet(reads, pipe[0]), "empty pipe is not readable");
    const char message = 's';
    std::thread writer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        Require(sceKernelWrite(pipe[1], &message, 1) == 1, "write guest pipe");
    });
    reads = {};
    Set(reads, pipe[0]);
    const Timeval second{1, 0};
    const int result = select_nid_postfix(pipe[0] + 1, reads.data(), nullptr, nullptr, second.data());
    writer.join();
    Require(result == 1 && IsSet(reads, pipe[0]) && second == Timeval{1, 0}, "pipe becomes readable during wait");
    char received = 0;
    Require(sceKernelRead(pipe[0], &received, 1) == 1 && received == message, "read guest pipe");

    int sockets[2];
    Require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "create native socket pair");
    Require(::write(sockets[1], &message, 1) == 1, "write native socket");
    auto socketLease = GuestFiles::AdoptOwned_nid_no_patch(sockets[0], 2);
    Require(static_cast<bool>(socketLease), "adopt owned native socket privately");
    sockets[0] = GuestFiles::LogicalDescriptor_nid_no_patch(socketLease);
    socketLease.reset();
    reads = {};
    DescriptorSet writes{};
    DescriptorSet exceptions{};
    Set(reads, sockets[0]);
    Set(writes, sockets[0]);
    Set(exceptions, sockets[0]);
    Require(select_nid_postfix(sockets[0] + 1, reads.data(), writes.data(), exceptions.data(), pollNow.data()) == 2 &&
        IsSet(reads, sockets[0]) && IsSet(writes, sockets[0]) && !IsSet(exceptions, sockets[0]),
        "return counts readiness bits and ordinary data is not exceptional");
    Require(sceKernelClose(sockets[0]) == 0 && ::close(sockets[1]) == 0, "close owned socket pair");
    reads = {};
    Set(reads, sockets[0]);
    const auto closedReads = reads;
    Require(select_nid_postfix(sockets[0] + 1, reads.data(), nullptr, nullptr, pollNow.data()) == -1 &&
        *__error_nid_postfix() == 9 && reads == closedReads, "invalid native descriptor reports EBADF without changing sets");
    reads = {};
    Set(reads, 1023);
    const auto highReads = reads;
    Require(select_nid_postfix(1024, reads.data(), nullptr, nullptr, pollNow.data()) == -1 &&
        *__error_nid_postfix() == 9 && reads == highReads, "unowned high guest descriptor is not silently ignored");

    int privatePipe[2];
    Require(::pipe(privatePipe) == 0, "create private high-descriptor fixture");
    const int highNative = ::fcntl(privatePipe[0], F_DUPFD_CLOEXEC, 1024);
    Require(highNative >= 1024, "native descriptor exceeds guest fd_set range");
    Require(::close(privatePipe[0]) == 0, "close original private pipe reader");
    auto highLease = GuestFiles::AdoptOwned_nid_no_patch(highNative, 0);
    Require(static_cast<bool>(highLease), "adopt private high native descriptor");
    const int highGuest = GuestFiles::LogicalDescriptor_nid_no_patch(highLease);
    Require(highGuest < 1024 && highGuest != highNative, "logical descriptor remains representable independently");
    highLease.reset();
    Require(::write(privatePipe[1], &message, 1) == 1, "prepare high native descriptor readability");
    reads = {};
    Set(reads, highGuest);
    Require(select_nid_postfix(highGuest + 1, reads.data(), nullptr, nullptr, pollNow.data()) == 1 &&
        IsSet(reads, highGuest), "high native descriptor readiness maps to the guest bit");
    Require(sceKernelRead(highGuest, &received, 1) == 1 && received == message, "read actual high native pipe payload");
    Require(::close(privatePipe[1]) == 0, "close private writer");
    DescriptorSet highExceptions{};
    Set(highExceptions, highGuest);
    const auto ignoredHangupStart = std::chrono::steady_clock::now();
    Require(select_nid_postfix(highGuest + 1, nullptr, nullptr, highExceptions.data(), shortWait.data()) == 0 &&
        !IsSet(highExceptions, highGuest), "native EOF is not an exceptional-data event");
    Require(std::chrono::steady_clock::now() - ignoredHangupStart >= std::chrono::milliseconds(20),
        "unrequested native hangup still waits for timeout");
    Require(sceKernelClose(highGuest) == 0, "close high native guest descriptor");

    struct sigaction previous{};
    struct sigaction action{};
    action.sa_handler = Interrupt;
    Require(::sigemptyset(&action.sa_mask) == 0 && ::sigaction(SIGUSR1, &action, &previous) == 0, "install interrupt handler");
    const pthread_t waitingThread = pthread_self();
    std::thread interrupter([waitingThread] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        Require(pthread_kill(waitingThread, SIGUSR1) == 0, "interrupt select wait");
    });
    reads = {};
    Set(reads, pipe[0]);
    const auto interruptedReads = reads;
    const int interrupted = select_nid_postfix(pipe[0] + 1, reads.data(), nullptr, nullptr, second.data());
    const int interruptionError = *__error_nid_postfix();
    interrupter.join();
    Require(::sigaction(SIGUSR1, &previous, nullptr) == 0, "restore interrupt handler");
    Require(interrupted == -1 && interruptionError == 4 && reads == interruptedReads && second == Timeval{1, 0},
        "EINTR preserves descriptor sets and guest timeval");
#endif

    KernelSetSocketPoller_nid_no_patch(&PollVirtual);
    reads = {};
    Set(reads, VirtualDescriptor);
    Require(select_nid_postfix(VirtualDescriptor + 1, reads.data(), nullptr, nullptr, pollNow.data()) == 0 &&
        !IsSet(reads, VirtualDescriptor), "virtual socket timeout clears unreadable bit");
    virtualReadiness = KernelSocketPoll::Readable | KernelSocketPoll::Writable;
    reads = {};
    DescriptorSet virtualWrites{};
    Set(reads, VirtualDescriptor);
    Set(virtualWrites, VirtualDescriptor);
    Require(select_nid_postfix(VirtualDescriptor + 1, reads.data(), virtualWrites.data(), nullptr, pollNow.data()) == 2 &&
        IsSet(reads, VirtualDescriptor) && IsSet(virtualWrites, VirtualDescriptor), "virtual socket readiness counts all sets");
    virtualError = 4;
    const auto virtualOriginal = reads;
    Require(select_nid_postfix(VirtualDescriptor + 1, reads.data(), nullptr, nullptr, pollNow.data()) == -1 &&
        *__error_nid_postfix() == 4 && reads == virtualOriginal, "virtual poller errors preserve descriptor sets");
    virtualError = 0;
    virtualReadiness = KernelSocketPoll::HangUp;
    DescriptorSet virtualExceptions{};
    Set(virtualExceptions, VirtualDescriptor);
    const auto hangupStart = std::chrono::steady_clock::now();
    Require(select_nid_postfix(VirtualDescriptor + 1, nullptr, nullptr, virtualExceptions.data(), shortWait.data()) == 0 &&
        !IsSet(virtualExceptions, VirtualDescriptor), "hangup is not an exceptional-data event");
    Require(std::chrono::steady_clock::now() - hangupStart >= std::chrono::milliseconds(20), "unrequested hangup still observes timeout");
#ifndef _WIN32
    virtualReadiness = KernelSocketPoll::Readable | KernelSocketPoll::Writable;
    Require(sceKernelWrite(pipe[1], &message, 1) == 1, "prepare mixed descriptor sets");
    reads = {};
    virtualWrites = {};
    Set(reads, pipe[0]);
    Set(reads, VirtualDescriptor);
    Set(virtualWrites, VirtualDescriptor);
    Require(select_nid_postfix(VirtualDescriptor + 1, reads.data(), virtualWrites.data(), nullptr, pollNow.data()) == 3 &&
        IsSet(reads, pipe[0]) && IsSet(reads, VirtualDescriptor) && IsSet(virtualWrites, VirtualDescriptor),
        "native and virtual descriptor readiness is combined");
    Require(sceKernelRead(pipe[0], &received, 1) == 1, "drain mixed readiness pipe");
    Require(sceKernelClose(pipe[1]) == 0, "close guest pipe writer");
    KernelSetSocketPoller_nid_no_patch(nullptr);
    reads = {};
    Set(reads, pipe[0]);
    Require(select_nid_postfix(pipe[0] + 1, reads.data(), nullptr, nullptr, pollNow.data()) == 1 &&
        IsSet(reads, pipe[0]), "native EOF is readable");
    Require(sceKernelClose(pipe[0]) == 0, "close guest pipe reader");
#endif
    KernelSetSocketPoller_nid_no_patch(nullptr);
}
