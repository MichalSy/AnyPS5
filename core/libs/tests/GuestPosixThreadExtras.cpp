#include "SceTypes.hpp"
#include "prx/libkernel/Time/include/Time.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <thread>
#ifndef _WIN32
#include <pthread.h>
#include <time.h>
#endif

extern "C" {
Pthread APS5_VABI scePthreadSelf();
int APS5_VABI scePthreadCreate(Pthread*, const PthreadAttr*, PthreadEntry, void*, const char*);
int APS5_VABI scePthreadJoin(Pthread, void**);
int APS5_VABI scePthreadGetname(Pthread, char*);
void APS5_VABI pthread_set_name_np_nid_postfix(Pthread, const char*);
int APS5_VABI pthread_setcanceltype_nid_postfix(int, int*);
int APS5_VABI pthread_getcpuclockid_nid_postfix(Pthread, KernelClockid*);
int APS5_VABI sched_get_priority_max_nid_postfix(int);
int APS5_VABI sched_get_priority_min_nid_postfix(int);
int* APS5_VABI __error_nid_postfix();
}

static void Require(bool value) { if (!value) std::abort(); }

static void TestPriorityLimits() {
    *__error_nid_postfix() = 13;
    for (int policy : {1, 3}) {
        Require(sched_get_priority_max_nid_postfix(policy) == 256);
        Require(sched_get_priority_min_nid_postfix(policy) == 767);
    }
    Require(sched_get_priority_max_nid_postfix(2) == 103);
    Require(sched_get_priority_min_nid_postfix(2) == 0);
    Require(*__error_nid_postfix() == 13);
    for (int policy : {-1, 0, 4}) {
        Require(sched_get_priority_max_nid_postfix(policy) == -1);
        Require(*__error_nid_postfix() == 22);
        Require(sched_get_priority_min_nid_postfix(policy) == -1);
        Require(*__error_nid_postfix() == 22);
    }
}

static std::int64_t Nanos(KernelClockid clock) {
    KernelTimespec value{};
    Require(clock_gettime_nid_postfix(clock, &value) == 0);
    return value.tv_sec * 1000000000LL + value.tv_nsec;
}

static std::atomic<bool> stop{false};

struct ClockProbe {
    KernelClockid mainClock;
#ifndef _WIN32
    clockid_t nativeMainClock;
    std::atomic<KernelClockid> workerSelfClock{-1};
    std::atomic<bool> ready{false};
#endif
};

static void* APS5_VABI Burn(void* opaque) {
#ifndef _WIN32
    auto& probe = *static_cast<ClockProbe*>(opaque);
    KernelClockid selfClock = -1;
    Require(pthread_getcpuclockid_nid_postfix(scePthreadSelf(), &selfClock) == 0);
    timespec before{};
    timespec after{};
    Require(::clock_gettime(probe.nativeMainClock, &before) == 0);
    const auto sampled = Nanos(probe.mainClock);
    Require(::clock_gettime(probe.nativeMainClock, &after) == 0);
    Require(sampled >= before.tv_sec * 1000000000LL + before.tv_nsec);
    Require(sampled <= after.tv_sec * 1000000000LL + after.tv_nsec);
    probe.workerSelfClock.store(selfClock, std::memory_order_relaxed);
    probe.ready.store(true, std::memory_order_release);
#else
    (void)opaque;
#endif
    while (!stop.load(std::memory_order_acquire)) {}
    return nullptr;
}

int main() {
    TestPriorityLimits();
    const auto self = scePthreadSelf();
    pthread_set_name_np_nid_postfix(self, "quake-main");
    char name[32]{};
    Require(scePthreadGetname(self, name) == 0);
    Require(std::strcmp(name, "quake-main") == 0);
    int oldType = -1;
    Require(pthread_setcanceltype_nid_postfix(2, &oldType) == 45 && oldType == -1);
    Require(pthread_setcanceltype_nid_postfix(1, &oldType) == 22 && oldType == -1);
    Require(pthread_setcanceltype_nid_postfix(0, &oldType) == 0 && oldType == 0);

    KernelClockid clock = -1;
    Require(pthread_getcpuclockid_nid_postfix(nullptr, &clock) == 22);
    Require(clock == -1);
    Require(pthread_getcpuclockid_nid_postfix(self, nullptr) == 14);
    Require(pthread_getcpuclockid_nid_postfix(self, &clock) == 0);
    Require(Nanos(clock) >= 0);
    KernelTimespec resolution{};
    Require(clock_getres_nid_postfix(clock, &resolution) == 0);
    Require(resolution.tv_nsec > 0);

    ClockProbe probe{clock};
#ifndef _WIN32
    Require(::pthread_getcpuclockid(::pthread_self(), &probe.nativeMainClock) == 0);
#endif
    Pthread worker = nullptr;
    Require(scePthreadCreate(&worker, nullptr, Burn, &probe, "cpu-clock-worker") == 0);
#ifndef _WIN32
    KernelClockid workerClock = -1;
    Require(pthread_getcpuclockid_nid_postfix(worker, &workerClock) == 0);
    Require(workerClock != clock);
    const auto readyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!probe.ready.load(std::memory_order_acquire)) {
        Require(std::chrono::steady_clock::now() < readyDeadline);
        std::this_thread::yield();
    }
    Require(workerClock == probe.workerSelfClock.load(std::memory_order_relaxed));
    const auto before = Nanos(workerClock);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (Nanos(workerClock) - before < 1000000) {
        Require(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    Require(clock_getres_nid_postfix(workerClock, &resolution) == 0);
    Require(resolution.tv_nsec > 0);
    Require(clock_getres_nid_postfix(workerClock, nullptr) == 0);
#else
    KernelClockid workerClock = -1;
    Require(pthread_getcpuclockid_nid_postfix(worker, &workerClock) == 0);
    Require(workerClock != clock);
#endif
    stop.store(true, std::memory_order_release);
    Require(scePthreadJoin(worker, nullptr) == 0);
}
