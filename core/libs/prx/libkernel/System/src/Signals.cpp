#include "prx/libc/include/general/VabiMacros.hpp"
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <csignal>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#ifndef _WIN32
#include <pthread.h>
#endif

extern "C" int* APS5_VABI __error_nid_postfix();
extern "C" int APS5_VABI getpid_nid_postfix(void);

using GuestHandler = void (APS5_VABI *)(int);
struct GuestSignalSet {
    std::uint32_t bits[4];
};
struct GuestSignalAction {
    GuestHandler handler;
    std::int32_t flags;
    GuestSignalSet mask;
};
static_assert(sizeof(GuestSignalSet) == 16);
static_assert(sizeof(GuestSignalAction) == 32);
static_assert(offsetof(GuestSignalAction, flags) == 8);
static_assert(offsetof(GuestSignalAction, mask) == 12);

extern "C" int APS5_VABI _sigprocmask_nid_postfix(int how, const GuestSignalSet* set, GuestSignalSet* previousSet);

namespace {
constexpr int guestInvalid = 22;
constexpr int guestFault = 14;
constexpr int guestNotSupported = 45;
constexpr int guestRestart = 0x02;
constexpr int guestResetHand = 0x04;
constexpr int guestNoChildStop = 0x08;
constexpr int guestNoDefer = 0x10;
constexpr int guestNoChildWait = 0x20;
constexpr int guestOnStack = 0x01;
constexpr int guestSigInfo = 0x40;
constexpr int guestActionFlags = 0x7f;

std::atomic<GuestHandler> handlers[129]{};
static_assert(std::atomic<GuestHandler>::is_always_lock_free);
#ifdef _WIN32
GuestSignalAction dispositions[129]{};
std::atomic<std::uint32_t> blockedMask{0};
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
#endif
std::mutex registration;

int Fail(int error) {
    *__error_nid_postfix() = error;
    return -1;
}
bool ValidSetSignal(int guest) { return guest >= 1 && guest <= 128; }
bool SetContains(const GuestSignalSet& set, int guest) {
    const unsigned index = static_cast<unsigned>(guest - 1);
    return (set.bits[index / 32] & (std::uint32_t{1} << (index % 32))) != 0;
}
void SetAdd(GuestSignalSet& set, int guest) {
    const unsigned index = static_cast<unsigned>(guest - 1);
    set.bits[index / 32] |= std::uint32_t{1} << (index % 32);
}
int NativeSignal(int guest) {
    switch (guest) {
        case 2: return SIGINT;
        case 4: return SIGILL;
        case 6: return SIGABRT;
        case 8: return SIGFPE;
        case 11: return SIGSEGV;
        case 15: return SIGTERM;
#ifndef _WIN32
        case 1: return SIGHUP;
        case 3: return SIGQUIT;
        case 5: return SIGTRAP;
        case 9: return SIGKILL;
        case 10: return SIGBUS;
        case 12: return SIGSYS;
        case 13: return SIGPIPE;
        case 14: return SIGALRM;
        case 16: return SIGURG;
        case 17: return SIGSTOP;
        case 18: return SIGTSTP;
        case 19: return SIGCONT;
        case 20: return SIGCHLD;
        case 21: return SIGTTIN;
        case 22: return SIGTTOU;
        case 23: return SIGIO;
        case 24: return SIGXCPU;
        case 25: return SIGXFSZ;
        case 26: return SIGVTALRM;
        case 27: return SIGPROF;
        case 28: return SIGWINCH;
        case 30: return SIGUSR1;
        case 31: return SIGUSR2;
#endif
        default: return 0;
    }
}
void Dispatch(int native) {
    int guest = 0;
    for (int candidate = 1; candidate < 32; ++candidate)
        if (NativeSignal(candidate) == native) { guest = candidate; break; }
    if (!guest) return;
#ifdef _WIN32
    std::signal(native, Dispatch);
    if ((blockedMask.load() & (std::uint32_t{1} << (guest - 1))) != 0) return;
#endif
    const auto callback = handlers[guest].load();
    if (reinterpret_cast<std::uintptr_t>(callback) > 1) callback(guest);
}

#ifdef _WIN32
bool Delivered(const GuestSignalAction& action) {
    return reinterpret_cast<std::uintptr_t>(action.handler) > 1 && !(action.flags & guestSigInfo);
}
bool Install(int guest, const GuestSignalAction& action) {
    const int native = NativeSignal(guest);
    if (!native) return true;
    const auto previous = handlers[guest].exchange(Delivered(action) ? action.handler : nullptr);
    const auto address = reinterpret_cast<std::uintptr_t>(action.handler);
    auto hostHandler = address == 1 ? SIG_IGN : Delivered(action) ? Dispatch : SIG_DFL;
    if (std::signal(native, hostHandler) == SIG_ERR) {
        handlers[guest].store(previous);
        return false;
    }
    return true;
}
#endif

#ifndef _WIN32
int GuestError(int native) {
    switch (native) {
        case EINVAL: return guestInvalid;
        case EFAULT: return guestFault;
        case EPERM: return 1;
        case ENOMEM: return 12;
        default: return guestNotSupported;
    }
}
void ToNativeSet(const GuestSignalSet& guest, sigset_t& native) {
    ::sigemptyset(&native);
    for (int signal = 1; signal < 32; ++signal)
        if (const int host = NativeSignal(signal); host && SetContains(guest, signal))
            ::sigaddset(&native, host);
}
GuestSignalSet FromNativeSet(const sigset_t& native) {
    GuestSignalSet guest{};
    for (int signal = 1; signal < 32; ++signal)
        if (const int host = NativeSignal(signal); host && ::sigismember(&native, host) == 1)
            SetAdd(guest, signal);
    return guest;
}
int ChangeMask(int how, const GuestSignalSet* set, GuestSignalSet* previous) {
    if (set && (how < 1 || how > 3)) return guestInvalid;
    sigset_t before{};
    if (const int error = ::pthread_sigmask(SIG_SETMASK, nullptr, &before))
        return GuestError(error);
    if (set) {
        sigset_t after = before;
        for (int signal = 1; signal < 32; ++signal) {
            const int host = NativeSignal(signal);
            if (!host) continue;
            const bool included = SetContains(*set, signal);
            if (included && how != 2) ::sigaddset(&after, host);
            else if ((included && how == 2) || how == 3) ::sigdelset(&after, host);
        }
        if (const int error = ::pthread_sigmask(SIG_SETMASK, &after, nullptr))
            return GuestError(error);
    }
    if (previous) *previous = FromNativeSet(before);
    return 0;
}
class RegistrationMask {
public:
    RegistrationMask() {
        sigset_t all{};
        ::sigfillset(&all);
        error = ::pthread_sigmask(SIG_BLOCK, &all, &previous);
    }
    ~RegistrationMask() {
        if (!error) ::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    }
    int error = 0;
private:
    sigset_t previous{};
};
int ToNativeFlags(int flags) {
    int native = 0;
    if (flags & guestRestart) native |= SA_RESTART;
    if (flags & guestResetHand) native |= SA_RESETHAND;
    if (flags & guestNoChildStop) native |= SA_NOCLDSTOP;
    if (flags & guestNoDefer) native |= SA_NODEFER;
    if (flags & guestNoChildWait) native |= SA_NOCLDWAIT;
    return native;
}
int FromNativeFlags(int flags) {
    int guest = 0;
    if (flags & SA_RESTART) guest |= guestRestart;
    if (flags & SA_RESETHAND) guest |= guestResetHand;
    if (flags & SA_NOCLDSTOP) guest |= guestNoChildStop;
    if (flags & SA_NODEFER) guest |= guestNoDefer;
    if (flags & SA_NOCLDWAIT) guest |= guestNoChildWait;
    if (flags & SA_ONSTACK) guest |= guestOnStack;
    if (flags & SA_SIGINFO) guest |= guestSigInfo;
    return guest;
}
int ChangeAction(int guest, const GuestSignalAction* action, GuestSignalAction* previous) {
    if (!ValidSetSignal(guest)) return guestInvalid;
    const int native = NativeSignal(guest);
    if (!native) return guestNotSupported;
    if (action) {
        if (guest == 9 || guest == 17 || (action->flags & ~guestActionFlags)) return guestInvalid;
        if (action->flags & (guestOnStack | guestSigInfo)) return guestNotSupported;
        if (reinterpret_cast<std::uintptr_t>(action->handler) == static_cast<std::uintptr_t>(-1))
            return guestInvalid;
    }
    RegistrationMask blocked;
    if (blocked.error) return GuestError(blocked.error);
    std::lock_guard lock(registration);
    struct sigaction oldNative{};
    if (::sigaction(native, nullptr, &oldNative) != 0) return GuestError(errno);
    GuestSignalAction oldGuest{};
    if (oldNative.sa_handler == SIG_DFL) oldGuest.handler = nullptr;
    else if (oldNative.sa_handler == SIG_IGN)
        oldGuest.handler = reinterpret_cast<GuestHandler>(std::uintptr_t{1});
    else if (!(oldNative.sa_flags & SA_SIGINFO) && oldNative.sa_handler == Dispatch)
        oldGuest.handler = handlers[guest].load();
    else return guestNotSupported;
    oldGuest.flags = FromNativeFlags(oldNative.sa_flags);
    oldGuest.mask = FromNativeSet(oldNative.sa_mask);
    if (action) {
        struct sigaction next{};
        const auto address = reinterpret_cast<std::uintptr_t>(action->handler);
        next.sa_handler = address == 0 ? SIG_DFL : address == 1 ? SIG_IGN : Dispatch;
        next.sa_flags = ToNativeFlags(action->flags);
        ToNativeSet(action->mask, next.sa_mask);
        const auto oldHandler = handlers[guest].exchange(action->handler);
        if (::sigaction(native, &next, nullptr) != 0) {
            const int error = GuestError(errno);
            handlers[guest].store(oldHandler);
            return error;
        }
    }
    if (previous) *previous = oldGuest;
    return 0;
}
#endif
}

#ifndef _WIN32
extern "C" std::mutex* NativeSignalRegistration_nid_no_patch() {
    return &registration;
}
extern "C" void CopyGuestSignalMask_nid_no_patch(const void* nativeMask, std::uint32_t* guestBits) {
    const auto guest = FromNativeSet(*static_cast<const sigset_t*>(nativeMask));
    for (unsigned index = 0; index < 4; ++index) guestBits[index] = guest.bits[index];
}
#endif

struct GuestStack {
    void* sp;
    std::size_t size;
    int flags;
};
static_assert(sizeof(GuestStack) == 24 && offsetof(GuestStack, flags) == 16);
namespace {
constexpr int SsDisable = 4;
constexpr std::size_t MinSignalStackSize = 2048;
thread_local GuestStack alternateStack{nullptr, 0, SsDisable};
}

extern "C" {
int APS5_VABI sigaction_nid_postfix(int guest, const GuestSignalAction* action, GuestSignalAction* previous) {
#ifdef _WIN32
    if (!ValidSetSignal(guest)) return Fail(guestInvalid);
    if (action && reinterpret_cast<std::uintptr_t>(action->handler) == static_cast<std::uintptr_t>(-1))
        return Fail(guestInvalid);
    if (action && (guest == 9 || guest == 17) && action->handler) return Fail(guestInvalid);
    std::lock_guard lock(registration);
    const auto old = dispositions[guest];
    if (action) {
        if (!Install(guest, *action)) return Fail(guestInvalid);
        dispositions[guest] = *action;
    }
    if (previous) *previous = old;
    return 0;
#else
    const int savedError = *__error_nid_postfix();
    const int error = ChangeAction(guest, action, previous);
    if (error) return Fail(error);
    *__error_nid_postfix() = savedError;
    return 0;
#endif
}
int APS5_VABI pthread_sigmask_nid_postfix(int how, const GuestSignalSet* set, GuestSignalSet* previous) {
    const int savedError = *__error_nid_postfix();
#ifdef _WIN32
    const int error = set && (how < 1 || how > 3) ? guestInvalid : _sigprocmask_nid_postfix(how, set, previous);
#else
    const int error = ChangeMask(how, set, previous);
#endif
    *__error_nid_postfix() = savedError;
    return error;
}
GuestHandler APS5_VABI signal_nid_postfix(int guest, GuestHandler handler) {
    const auto invalid = reinterpret_cast<GuestHandler>(static_cast<std::uintptr_t>(-1));
    if (!NativeSignal(guest) || handler == invalid) { Fail(guestInvalid); return invalid; }
#ifdef _WIN32
    std::lock_guard lock(registration);
    const GuestSignalAction action{handler, guestRestart, {}};
    if (!Install(guest, action)) { Fail(guestInvalid); return invalid; }
    const auto previous = dispositions[guest].handler;
    dispositions[guest] = action;
    return previous;
#else
    const GuestSignalAction action{handler, guestRestart, {}};
    GuestSignalAction previous{};
    if (sigaction_nid_postfix(guest, &action, &previous)) return invalid;
    return previous.handler;
#endif
}
int APS5_VABI raise_nid_postfix(int guest) {
    const int native = NativeSignal(guest);
    if (!native) return Fail(guestInvalid);
#ifdef _WIN32
    {
        std::lock_guard lock(registration);
        if (reinterpret_cast<std::uintptr_t>(dispositions[guest].handler) > 1 && !Delivered(dispositions[guest]))
            throw std::runtime_error("raise: SA_SIGINFO handlers are not delivered");
    }
#endif
    const int result = std::raise(native);
    if (result) return Fail(guestInvalid);
    return 0;
}
int APS5_VABI kill_nid_postfix(int pid, int guest) {
    if (guest != 0 && !ValidSetSignal(guest)) return Fail(guestInvalid);
    const int self = getpid_nid_postfix();
    if (pid != self && pid != 0 && pid != -self) { *__error_nid_postfix() = 3; return -1; }
    return guest == 0 ? 0 : raise_nid_postfix(guest);
}
int APS5_VABI sigaltstack_nid_postfix(const GuestStack* stack, GuestStack* previous) {
    GuestStack replacement = alternateStack;
    if (stack != nullptr) {
        if ((stack->flags & ~SsDisable) != 0) { *__error_nid_postfix() = 22; return -1; }
        if (stack->flags & SsDisable) replacement.flags = SsDisable;
        else if (stack->size < MinSignalStackSize) { *__error_nid_postfix() = 12; return -1; }
        else replacement = {stack->sp, stack->size, 0};
    }
    if (previous != nullptr) *previous = alternateStack;
    alternateStack = replacement;
    return 0;
}
int APS5_VABI _sigprocmask_nid_postfix(int how, const GuestSignalSet* set, GuestSignalSet* previousSet) {
#ifdef _WIN32
    if (set && (how < 1 || how > 3)) return Fail(guestInvalid);
    std::lock_guard lock(registration);
    if (previousSet) *previousSet = GuestSignalSet{{blockedMask.load(), 0, 0, 0}};
    if (set) {
        switch (how) {
            case 1: blockedMask.fetch_or(set->bits[0]); break;
            case 2: blockedMask.fetch_and(~set->bits[0]); break;
            case 3: blockedMask.store(set->bits[0]); break;
        }
    }
    return 0;
#else
    const int savedError = *__error_nid_postfix();
    const int error = ChangeMask(how, set, previousSet);
    if (error) return Fail(error);
    *__error_nid_postfix() = savedError;
    return 0;
#endif
}
int APS5_VABI sigprocmask_nid_postfix(int how, const void* set, void* previousSet) {
    return _sigprocmask_nid_postfix(how, static_cast<const GuestSignalSet*>(set),
                                    static_cast<GuestSignalSet*>(previousSet));
}
int APS5_VABI _is_signal_return_nid_postfix(std::uint64_t programCounter) {
    (void)programCounter;
    return 0;
}
}
