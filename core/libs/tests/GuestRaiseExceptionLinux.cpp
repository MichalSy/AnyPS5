#include "SceTypes.hpp"
#include "prx/libkernel/KernelErrors.hpp"
#include "prx/libkernel/Pthread/include/Pthread.hpp"
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <thread>

struct GuestSignalSet { std::uint32_t bits[4]; };
struct GuestSignalAction {
    void (APS5_VABI *handler)(int);
    int flags;
    GuestSignalSet mask;
};

extern "C" {
int APS5_VABI sceKernelInstallExceptionHandler(int, void*);
int APS5_VABI sceKernelRemoveExceptionHandler(int);
int APS5_VABI sceKernelRaiseException(Pthread, int);
int APS5_VABI scePthreadCreate(Pthread*, const PthreadAttr*, PthreadEntry, void*, const char*);
int APS5_VABI scePthreadJoin(Pthread, void**);
int APS5_VABI scePthreadAttrInit(PthreadAttr*);
int APS5_VABI scePthreadAttrDestroy(PthreadAttr*);
int APS5_VABI scePthreadAttrSetdetachstate(PthreadAttr*, int);
Pthread APS5_VABI scePthreadSelf();
int APS5_VABI pthread_sigmask_nid_postfix(int, const GuestSignalSet*, GuestSignalSet*);
int APS5_VABI sigaction_nid_postfix(int, const GuestSignalAction*, GuestSignalAction*);
int* APS5_VABI __error_nid_postfix();
}

static void Require(bool value) { if (!value) std::abort(); }

template <typename Predicate>
static void Until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        Require(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}

static std::atomic<unsigned> calls{0};
static std::atomic<unsigned> failures{0};
static std::atomic<std::uintptr_t> handlerThread{0};
static std::atomic<bool> restoreRegisters{false};
static_assert(decltype(calls)::is_always_lock_free);
static_assert(decltype(failures)::is_always_lock_free);
static_assert(decltype(handlerThread)::is_always_lock_free);
static_assert(decltype(restoreRegisters)::is_always_lock_free);

static constexpr std::uint64_t RegisterSeed = 0x123456789abcdef0;
static constexpr std::uint64_t RegisterResult = 0x23456789abcdef01;
static constexpr std::array<unsigned char, 16> VectorSeed{
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00};
static constexpr std::array<unsigned char, 16> VectorResult{
    0xf1, 0xe2, 0xd3, 0xc4, 0xb5, 0xa6, 0x97, 0x88,
    0x79, 0x6a, 0x5b, 0x4c, 0x3d, 0x2e, 0x1f, 0x00};

struct RegisterProbe {
    std::atomic<std::uint32_t> ready{0};
    std::atomic<std::uint32_t> released{0};
    std::uintptr_t stackLow = 0;
    std::uintptr_t stackHigh = 0;
    std::uint64_t observedRegister = 0;
    std::array<unsigned char, 16> observedVector{};
};

static RegisterProbe* registerProbe = nullptr;
static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t));
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

static std::uint64_t Read64(const unsigned char* context, std::size_t offset) {
    std::uint64_t value;
    std::memcpy(&value, context + offset, sizeof(value));
    return value;
}

static void APS5_VABI Handler(int signum, void* rawContext) {
    unsigned errors = 0;
    if (signum != 30 || rawContext == nullptr) errors |= 1;
    if (rawContext != nullptr) {
        auto* context = static_cast<unsigned char*>(rawContext);
        if (Read64(context, 0x108) != 0x480) errors |= 2;
        if (Read64(context, 0x110) != 0x10002 || Read64(context, 0x118) != 0x20001) errors |= 4;
        const auto rsp = Read64(context, 0xf8);
        if (rsp == 0 || Read64(context, 0xe0) == 0) errors |= 8;
        if ((reinterpret_cast<std::uintptr_t>(rawContext) & 63) != 0) errors |= 16;
        if (restoreRegisters.load(std::memory_order_relaxed)) {
            auto& probe = *registerProbe;
            if (Read64(context, 0xa0) != RegisterSeed) errors |= 32;
            if (rsp < probe.stackLow || rsp >= probe.stackHigh) errors |= 64;
            if (std::memcmp(context + 0x1e0, VectorSeed.data(), VectorSeed.size()) != 0) errors |= 128;
            std::memcpy(context + 0xa0, &RegisterResult, sizeof(RegisterResult));
            std::memcpy(context + 0x1e0, VectorResult.data(), VectorResult.size());
            std::memset(context + 0x140 + 464, 0xa5, 48);
            probe.released.store(1, std::memory_order_release);
        }
    }
    failures.fetch_or(errors, std::memory_order_relaxed);
    handlerThread.store(static_cast<std::uintptr_t>(::pthread_self()), std::memory_order_relaxed);
    errno = EDOM;
    calls.fetch_add(1, std::memory_order_release);
}

static void ForeignHandler(int) {}
static void APS5_VABI PosixHandler(int) {}

struct WorkerProbe {
    std::atomic<bool> ready{false};
    std::atomic<bool> stop{false};
    std::atomic<bool> done{false};
    std::uintptr_t native = 0;
    int observedErrno = 0;
    bool waiting = false;
    std::mutex mutex;
    std::condition_variable wake;
};

static void* APS5_VABI Worker(void* opaque) {
    auto& probe = *static_cast<WorkerProbe*>(opaque);
    probe.native = static_cast<std::uintptr_t>(::pthread_self());
    errno = ERANGE;
    if (probe.waiting) {
        std::unique_lock lock(probe.mutex);
        probe.ready.store(true, std::memory_order_release);
        probe.wake.wait(lock, [&] { return probe.stop.load(std::memory_order_acquire); });
    } else {
        probe.ready.store(true, std::memory_order_release);
        while (!probe.stop.load(std::memory_order_acquire)) {}
    }
    probe.observedErrno = errno;
    probe.done.store(true, std::memory_order_release);
    return nullptr;
}

static void Stop(WorkerProbe& probe) {
    if (probe.waiting) {
        std::lock_guard lock(probe.mutex);
        probe.stop.store(true, std::memory_order_release);
        probe.wake.notify_one();
    } else {
        probe.stop.store(true, std::memory_order_release);
    }
}

static void TestWorker(bool waiting, bool detached) {
    WorkerProbe probe;
    probe.waiting = waiting;
    PthreadAttr attr = nullptr;
    Pthread thread = nullptr;
    if (detached) {
        Require(scePthreadAttrInit(&attr) == 0);
        Require(scePthreadAttrSetdetachstate(&attr, 1) == 0);
    }
    Require(scePthreadCreate(&thread, attr == nullptr ? nullptr : &attr, Worker, &probe, "exception-worker") == 0);
    if (attr != nullptr) Require(scePthreadAttrDestroy(&attr) == 0);
    Until([&] { return probe.ready.load(std::memory_order_acquire); });
    if (waiting) { std::lock_guard lock(probe.mutex); }
    const auto before = calls.load(std::memory_order_acquire);
    Require(sceKernelRaiseException(thread, 30) == 0);
    Until([&] { return calls.load(std::memory_order_acquire) == before + 1; });
    Require(handlerThread.load(std::memory_order_relaxed) == probe.native);
    Stop(probe);
    if (detached) Until([&] { return probe.done.load(std::memory_order_acquire); });
    else {
        Until([&] { return thread->_finished.load(std::memory_order_acquire); });
        Require(sceKernelRaiseException(thread, 30) == SCE_KERNEL_ERROR_ESRCH);
        Require(scePthreadJoin(thread, nullptr) == 0);
    }
    Require(probe.observedErrno == ERANGE);
}

static void* APS5_VABI RegisterWorker(void* opaque) {
    auto& probe = *static_cast<RegisterProbe*>(opaque);
    pthread_attr_t attr;
    Require(::pthread_getattr_np(::pthread_self(), &attr) == 0);
    void* address = nullptr;
    std::size_t size = 0;
    Require(::pthread_attr_getstack(&attr, &address, &size) == 0);
    Require(::pthread_attr_destroy(&attr) == 0);
    probe.stackLow = reinterpret_cast<std::uintptr_t>(address);
    probe.stackHigh = probe.stackLow + size;
    asm volatile(
        "movq %[seed], %%r12\n\t"
        "movdqu (%[input]), %%xmm0\n\t"
        "movl $1, (%[ready])\n\t"
        "1:\n\t"
        "cmpl $0, (%[released])\n\t"
        "je 1b\n\t"
        "movq %%r12, (%[outputRegister])\n\t"
        "movdqu %%xmm0, (%[outputVector])\n\t"
        :
        : [seed] "r" (RegisterSeed), [input] "r" (VectorSeed.data()),
          [ready] "r" (&probe.ready), [released] "r" (&probe.released),
          [outputRegister] "r" (&probe.observedRegister), [outputVector] "r" (probe.observedVector.data())
        : "r12", "xmm0", "cc", "memory");
    return nullptr;
}

static void TestContextRestore() {
    RegisterProbe probe;
    registerProbe = &probe;
    restoreRegisters.store(true, std::memory_order_relaxed);
    Pthread thread = nullptr;
    Require(scePthreadCreate(&thread, nullptr, RegisterWorker, &probe, "exception-registers") == 0);
    Until([&] { return probe.ready.load(std::memory_order_acquire) != 0; });
    Require(sceKernelRaiseException(thread, 30) == 0);
    Require(scePthreadJoin(thread, nullptr) == 0);
    restoreRegisters.store(false, std::memory_order_relaxed);
    registerProbe = nullptr;
    Require(probe.observedRegister == RegisterResult);
    Require(probe.observedVector == VectorResult);
    Require(failures.load(std::memory_order_relaxed) == 0);
}

struct MaskProbe {
    std::atomic<bool> ready{false};
    std::atomic<bool> unblock{false};
    int observedErrno = 0;
};

static void* APS5_VABI MaskWorker(void* opaque) {
    auto& probe = *static_cast<MaskProbe*>(opaque);
    GuestSignalSet signal{{1u << 29, 0, 0, 0}};
    GuestSignalSet previous{};
    Require(pthread_sigmask_nid_postfix(1, &signal, &previous) == 0);
    errno = E2BIG;
    probe.ready.store(true, std::memory_order_release);
    while (!probe.unblock.load(std::memory_order_acquire)) {}
    Require(pthread_sigmask_nid_postfix(3, &previous, nullptr) == 0);
    probe.observedErrno = errno;
    return nullptr;
}

static void TestBlockedDelivery() {
    MaskProbe probe;
    Pthread thread = nullptr;
    Require(scePthreadCreate(&thread, nullptr, MaskWorker, &probe, "exception-mask") == 0);
    Until([&] { return probe.ready.load(std::memory_order_acquire); });
    const auto before = calls.load(std::memory_order_acquire);
    Require(sceKernelRaiseException(thread, 30) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Require(calls.load(std::memory_order_acquire) == before);
    probe.unblock.store(true, std::memory_order_release);
    Require(scePthreadJoin(thread, nullptr) == 0);
    Require(calls.load(std::memory_order_acquire) == before + 1);
    Require(probe.observedErrno == E2BIG);
}

static void TestAdoptedThread() {
    std::atomic<Pthread> adopted{nullptr};
    WorkerProbe probe;
    std::thread thread([&] {
        adopted.store(scePthreadSelf(), std::memory_order_release);
        Worker(&probe);
    });
    Until([&] { return probe.ready.load(std::memory_order_acquire); });
    const auto before = calls.load(std::memory_order_acquire);
    Require(sceKernelRaiseException(adopted.load(std::memory_order_acquire), 30) == 0);
    Until([&] { return calls.load(std::memory_order_acquire) == before + 1; });
    Require(handlerThread.load(std::memory_order_relaxed) == probe.native);
    Stop(probe);
    thread.join();
    Require(probe.observedErrno == ERANGE);
}

int main() {
    const auto self = scePthreadSelf();
    Require(sceKernelRaiseException(self, 30) == SCE_KERNEL_ERROR_EINVAL);
    Require(sceKernelRaiseException(nullptr, 30) == SCE_KERNEL_ERROR_ESRCH);
    Require(sceKernelRaiseException(self, 31) == SCE_KERNEL_ERROR_EINVAL);
    struct sigaction original{};
    Require(::sigaction(SIGUSR1, nullptr, &original) == 0);
    struct sigaction foreign{};
    foreign.sa_handler = ForeignHandler;
    ::sigemptyset(&foreign.sa_mask);
    Require(::sigaction(SIGUSR1, &foreign, nullptr) == 0);
    Require(sceKernelInstallExceptionHandler(30, reinterpret_cast<void*>(&Handler)) == SCE_KERNEL_ERROR_EOPNOTSUPP);
    struct sigaction checked{};
    Require(::sigaction(SIGUSR1, nullptr, &checked) == 0 && checked.sa_handler == ForeignHandler);
    Require(::sigaction(SIGUSR1, &original, nullptr) == 0);
    Require(sceKernelInstallExceptionHandler(30, reinterpret_cast<void*>(&Handler)) == 0);
    Require(sceKernelInstallExceptionHandler(30, reinterpret_cast<void*>(&Handler)) == SCE_KERNEL_ERROR_EAGAIN);
    GuestSignalAction posix{PosixHandler, 0, {}};
    Require(sigaction_nid_postfix(30, &posix, nullptr) == -1 && *__error_nid_postfix() == 45);
    errno = ENOEXEC;
    Require(sceKernelRaiseException(self, 30) == 0);
    Require(calls.load(std::memory_order_acquire) == 1);
    Require(handlerThread.load(std::memory_order_relaxed) == static_cast<std::uintptr_t>(::pthread_self()));
    Require(errno == ENOEXEC);
    TestWorker(false, false);
    TestWorker(true, false);
    TestWorker(false, true);
    TestAdoptedThread();
    TestBlockedDelivery();
    TestContextRestore();
    Require(failures.load(std::memory_order_relaxed) == 0);
    Require(sceKernelRemoveExceptionHandler(30) == SCE_KERNEL_ERROR_EOPNOTSUPP);
    Require(::sigaction(SIGUSR1, &original, nullptr) == 0);
    Require(sceKernelRaiseException(self, 30) == SCE_KERNEL_ERROR_EOPNOTSUPP);
}
