#include "prx/libc/include/general/VabiMacros.hpp"
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#ifndef _WIN32
#include <pthread.h>
#include <thread>
#endif

using Handler = void (APS5_VABI *)(int);
struct GuestSignalSet {
    std::uint32_t bits[4];
};
struct GuestSignalAction {
    Handler handler;
    std::int32_t flags;
    GuestSignalSet mask;
};
extern "C" {
Handler APS5_VABI signal_nid_postfix(int, Handler);
int APS5_VABI raise_nid_postfix(int);
int APS5_VABI sigprocmask_nid_postfix(int, const void*, void*);
int APS5_VABI sigemptyset_nid_postfix(GuestSignalSet*);
int APS5_VABI sigfillset_nid_postfix(GuestSignalSet*);
int APS5_VABI sigaddset_nid_postfix(GuestSignalSet*, int);
int APS5_VABI sigdelset_nid_postfix(GuestSignalSet*, int);
int APS5_VABI sigaction_nid_postfix(int, const GuestSignalAction*, GuestSignalAction*);
int APS5_VABI pthread_sigmask_nid_postfix(int, const GuestSignalSet*, GuestSignalSet*);
int* APS5_VABI __error_nid_postfix();
int APS5_VABI _is_signal_return_nid_postfix(std::uint64_t);
}
volatile std::sig_atomic_t received = 0;
volatile std::uintptr_t handlerReturn = 0;
void APS5_VABI Callback(int value) {
    received = value;
    handlerReturn = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
}
static void Require(bool value) { if (!value) std::abort(); }

static void TestSets() {
    GuestSignalSet set{};
    Require(sigfillset_nid_postfix(&set) == 0);
    for (auto word : set.bits) Require(word == UINT32_MAX);
    Require(sigemptyset_nid_postfix(&set) == 0);
    for (auto word : set.bits) Require(word == 0);
    for (int signal : {1, 32, 33, 128}) Require(sigaddset_nid_postfix(&set, signal) == 0);
    Require(set.bits[0] == 0x80000001u && set.bits[1] == 1);
    Require(set.bits[2] == 0 && set.bits[3] == 0x80000000u);
    Require(sigdelset_nid_postfix(&set, 32) == 0 && set.bits[0] == 1);
    Require(sigdelset_nid_postfix(&set, 128) == 0 && set.bits[3] == 0);
    Require(sigaddset_nid_postfix(&set, 0) == -1 && *__error_nid_postfix() == 22);
    Require(sigdelset_nid_postfix(&set, 129) == -1 && *__error_nid_postfix() == 22);
    Require(sigemptyset_nid_postfix(nullptr) == -1 && *__error_nid_postfix() == 14);
    Require(sigfillset_nid_postfix(nullptr) == -1 && *__error_nid_postfix() == 14);
    Require(sigaddset_nid_postfix(nullptr, 1) == -1 && *__error_nid_postfix() == 14);
    Require(sigdelset_nid_postfix(nullptr, 1) == -1 && *__error_nid_postfix() == 14);
}

#ifndef _WIN32
static void NativeCallback(int) {}
static void NativeInfoCallback(int, siginfo_t*, void*) {}
static void TestPosixSignals() {
    GuestSignalSet originalMask{};
    Require(pthread_sigmask_nid_postfix(0, nullptr, &originalMask) == 0);
    GuestSignalSet userSignal{};
    Require(sigaddset_nid_postfix(&userSignal, 30) == 0);
    Require(pthread_sigmask_nid_postfix(2, &userSignal, nullptr) == 0);
    GuestSignalAction action{Callback, 0x12, {}};
    Require(sigaddset_nid_postfix(&action.mask, 31) == 0);
    GuestSignalAction originalAction{};
    Require(sigaction_nid_postfix(30, &action, &originalAction) == 0);
    GuestSignalAction queried{};
    Require(sigaction_nid_postfix(30, nullptr, &queried) == 0);
    Require(queried.handler == Callback && queried.flags == 0x12);
    Require(queried.mask.bits[0] == (1u << 30));
    received = 0;
    Require(std::raise(SIGUSR1) == 0 && received == 30);

    *__error_nid_postfix() = 13;
    Require(pthread_sigmask_nid_postfix(0, &userSignal, nullptr) == 22);
    Require(*__error_nid_postfix() == 13);
    Require(pthread_sigmask_nid_postfix(0, nullptr, &queried.mask) == 0);
    Require(*__error_nid_postfix() == 13);
    Require(sigprocmask_nid_postfix(0, &userSignal, nullptr) == -1);
    Require(*__error_nid_postfix() == 22);
    Require(pthread_sigmask_nid_postfix(1, &userSignal, nullptr) == 0);
    received = 0;
    std::thread worker([&] {
        GuestSignalSet inherited{};
        Require(pthread_sigmask_nid_postfix(0, nullptr, &inherited) == 0);
        Require((inherited.bits[0] & (1u << 29)) != 0);
        Require(pthread_sigmask_nid_postfix(2, &userSignal, nullptr) == 0);
        Require(raise_nid_postfix(30) == 0 && received == 30);
    });
    worker.join();
    GuestSignalSet current{};
    Require(pthread_sigmask_nid_postfix(0, nullptr, &current) == 0);
    Require((current.bits[0] & (1u << 29)) != 0);
    received = 0;
    Require(raise_nid_postfix(30) == 0 && received == 0);
    Require(pthread_sigmask_nid_postfix(2, &userSignal, nullptr) == 0 && received == 30);

    GuestSignalSet full{};
    Require(sigfillset_nid_postfix(&full) == 0);
    GuestSignalSet beforeFull{};
    Require(pthread_sigmask_nid_postfix(3, &full, &beforeFull) == 0);
    Require(pthread_sigmask_nid_postfix(0, nullptr, &current) == 0);
    Require((current.bits[0] & (1u << 29)) != 0);
    Require((current.bits[0] & (1u << 8)) == 0);
    Require((current.bits[0] & (1u << 16)) == 0);
    Require(current.bits[1] == 0 && current.bits[2] == 0 && current.bits[3] == 0);
    Require(pthread_sigmask_nid_postfix(3, &beforeFull, nullptr) == 0);

    sigset_t hostOriginal{};
    sigset_t hostExtra{};
    Require(::sigemptyset(&hostExtra) == 0);
    Require(::sigaddset(&hostExtra, SIGRTMIN + 1) == 0);
    Require(::pthread_sigmask(SIG_BLOCK, &hostExtra, &hostOriginal) == 0);
    GuestSignalSet empty{};
    Require(pthread_sigmask_nid_postfix(3, &empty, nullptr) == 0);
    sigset_t hostCurrent{};
    Require(::pthread_sigmask(SIG_SETMASK, nullptr, &hostCurrent) == 0);
    Require(::sigismember(&hostCurrent, SIGRTMIN + 1) == 1);
    Require(::pthread_sigmask(SIG_SETMASK, &hostOriginal, nullptr) == 0);

    GuestSignalAction unsupported = action;
    for (int flag : {0x01, 0x40}) {
        unsupported.flags = flag;
        Require(sigaction_nid_postfix(30, &unsupported, nullptr) == -1);
        Require(*__error_nid_postfix() == 45);
    }
    unsupported.flags = 0x80;
    Require(sigaction_nid_postfix(30, &unsupported, nullptr) == -1 && *__error_nid_postfix() == 22);
    Require(sigaction_nid_postfix(7, nullptr, &queried) == -1 && *__error_nid_postfix() == 45);
    Require(sigaction_nid_postfix(9, &action, nullptr) == -1 && *__error_nid_postfix() == 22);
    Require(sigaction_nid_postfix(17, &action, nullptr) == -1 && *__error_nid_postfix() == 22);

    struct sigaction nativeAction{};
    struct sigaction nativeOriginal{};
    nativeAction.sa_handler = NativeCallback;
    Require(::sigemptyset(&nativeAction.sa_mask) == 0);
    Require(::sigaction(SIGUSR2, &nativeAction, &nativeOriginal) == 0);
    GuestSignalAction guestDefault{};
    Require(sigaction_nid_postfix(31, nullptr, &queried) == -1 && *__error_nid_postfix() == 45);
    Require(sigaction_nid_postfix(31, &guestDefault, nullptr) == -1 && *__error_nid_postfix() == 45);
    struct sigaction nativeQueried{};
    Require(::sigaction(SIGUSR2, nullptr, &nativeQueried) == 0);
    Require(nativeQueried.sa_handler == NativeCallback);
    Require(::sigaction(SIGUSR2, &nativeOriginal, nullptr) == 0);
    nativeAction.sa_sigaction = NativeInfoCallback;
    nativeAction.sa_flags = SA_SIGINFO | SA_ONSTACK;
    Require(::sigaction(SIGTRAP, &nativeAction, &nativeOriginal) == 0);
    Require(sigaction_nid_postfix(5, &guestDefault, nullptr) == -1 && *__error_nid_postfix() == 45);
    Require(::sigaction(SIGTRAP, nullptr, &nativeQueried) == 0);
    Require(nativeQueried.sa_sigaction == NativeInfoCallback && (nativeQueried.sa_flags & SA_SIGINFO));
    Require(::sigaction(SIGTRAP, &nativeOriginal, nullptr) == 0);

    action.flags = 0x04;
    Require(sigaction_nid_postfix(30, &action, nullptr) == 0);
    received = 0;
    Require(raise_nid_postfix(30) == 0 && received == 30);
    Require(sigaction_nid_postfix(30, nullptr, &queried) == 0 && queried.handler == nullptr);
    Require(sigaction_nid_postfix(30, &originalAction, nullptr) == 0);
    Require(pthread_sigmask_nid_postfix(3, &originalMask, nullptr) == 0);
}
#endif

int main() {
    TestSets();
#ifndef _WIN32
    GuestSignalSet initialMask{};
    GuestSignalSet emptyMask{};
    Require(pthread_sigmask_nid_postfix(3, &emptyMask, &initialMask) == 0);
#endif
    const auto invalid = reinterpret_cast<Handler>(static_cast<std::uintptr_t>(-1));
    const auto ignore = reinterpret_cast<Handler>(std::uintptr_t{1});
    Require(signal_nid_postfix(9, Callback) == invalid);
    Require(*__error_nid_postfix() == 22);
    Require(signal_nid_postfix(15, Callback) != invalid);
    Require(raise_nid_postfix(15) == 0 && received == 15);
    Require(handlerReturn != 0 && _is_signal_return_nid_postfix(handlerReturn) == 0);
    Require(_is_signal_return_nid_postfix(reinterpret_cast<std::uintptr_t>(Callback)) == 0);
    Require(_is_signal_return_nid_postfix(0) == 0);
    received = 0;
    Require(raise_nid_postfix(15) == 0 && received == 15);
    Require(signal_nid_postfix(15, ignore) != invalid);
    received = 0;
    Require(raise_nid_postfix(15) == 0 && received == 0);
    Require(signal_nid_postfix(15, nullptr) == ignore);
    Require(raise_nid_postfix(100) == -1 && *__error_nid_postfix() == 22);
    GuestSignalSet blocked{{0x20, 0, 0, 0}};
    GuestSignalSet previous{{}};
    Require(sigprocmask_nid_postfix(3, &blocked, &previous) == 0);
    Require(previous.bits[0] == 0);
    Require(sigprocmask_nid_postfix(1, nullptr, &previous) == 0);
    Require(previous.bits[0] == 0x20);
    Require(sigprocmask_nid_postfix(2, &blocked, nullptr) == 0);
    Require(sigprocmask_nid_postfix(1, nullptr, &previous) == 0);
    Require(previous.bits[0] == 0);
#ifndef _WIN32
    TestPosixSignals();
    Require(pthread_sigmask_nid_postfix(3, &initialMask, nullptr) == 0);
#else
    *__error_nid_postfix() = 13;
    Require(pthread_sigmask_nid_postfix(1, &blocked, nullptr) == 45);
    Require(*__error_nid_postfix() == 13);
    Require(sigaction_nid_postfix(15, nullptr, nullptr) == -1 && *__error_nid_postfix() == 45);
#endif
}
