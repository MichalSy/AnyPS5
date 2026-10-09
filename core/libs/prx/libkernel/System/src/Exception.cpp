#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/KernelErrors.hpp"
#include "prx/libkernel/Pthread/include/Pthread.hpp"
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <pthread.h>
#include <ucontext.h>
#endif

extern "C" Pthread APS5_VABI scePthreadSelf();
#ifdef _WIN32
extern "C" void Aps5RedirectedEntryStub();
#else
extern "C" std::mutex* NativeSignalRegistration_nid_no_patch();
extern "C" void CopyGuestSignalMask_nid_no_patch(const void* nativeMask, std::uint32_t* guestBits);
#endif

namespace {

struct GuestMcontext {
    std::uint64_t onstack;
    std::uint64_t rdi;
    std::uint64_t rsi;
    std::uint64_t rdx;
    std::uint64_t rcx;
    std::uint64_t r8;
    std::uint64_t r9;
    std::uint64_t rax;
    std::uint64_t rbx;
    std::uint64_t rbp;
    std::uint64_t r10;
    std::uint64_t r11;
    std::uint64_t r12;
    std::uint64_t r13;
    std::uint64_t r14;
    std::uint64_t r15;
    std::uint32_t trapno;
    std::uint16_t fs;
    std::uint16_t gs;
    std::uint64_t addr;
    std::uint32_t flags;
    std::uint16_t es;
    std::uint16_t ds;
    std::uint64_t err;
    std::uint64_t rip;
    std::uint64_t cs;
    std::uint64_t rflags;
    std::uint64_t rsp;
    std::uint64_t ss;
    std::uint64_t len;
    std::uint64_t fpformat;
    std::uint64_t ownedfp;
    std::uint64_t lbrfrom;
    std::uint64_t lbrto;
    std::uint64_t aux1;
    std::uint64_t aux2;
    std::uint64_t fpstate[104];
    std::uint64_t fsbase;
    std::uint64_t gsbase;
    std::uint64_t spare[6];
};

struct GuestUcontext {
    std::uint32_t sigmask[4];
    std::int32_t reserved[12];
    GuestMcontext mcontext;
    GuestUcontext* link;
    void* stackPointer;
    std::uint64_t stackSize;
    std::int32_t stackFlags;
    std::int32_t stackAlign;
    std::int32_t flags;
    std::int32_t spare[4];
    std::int32_t tail[3];
};

static_assert(offsetof(GuestUcontext, mcontext) == 0x40);
static_assert(offsetof(GuestUcontext, mcontext) + offsetof(GuestMcontext, rsp) == 0xf8);
static_assert(sizeof(GuestMcontext) == 0x480);
static_assert(sizeof(GuestUcontext) == 0x500);

using GuestExceptionHandler = void (APS5_VABI *)(int, void*);

constexpr std::array<int, 6> AllowedSignals{1, 4, 8, 10, 11, 30};

std::mutex handlersLock;
#ifdef _WIN32
std::array<void*, 32> handlers{};
#else
std::atomic<GuestExceptionHandler> linuxHandler{nullptr};
static_assert(decltype(linuxHandler)::is_always_lock_free);
#endif

bool Allowed(int signum) {
    for (const int allowed : AllowedSignals)
        if (allowed == signum) return true;
    return false;
}

#ifdef _WIN32
GuestExceptionHandler Handler(int signum) {
    std::lock_guard lock(handlersLock);
    return reinterpret_cast<GuestExceptionHandler>(handlers[signum]);
}

constexpr std::size_t RedZone = 128;
constexpr std::size_t HomeArea = 32;

struct Delivery {
    GuestExceptionHandler handler;
    int signum;
    CONTEXT context;
};

void Deliver(GuestExceptionHandler handler, int signum, CONTEXT& context) {
    GuestUcontext ucontext{};
    auto& m = ucontext.mcontext;
    m.rdi = context.Rdi;
    m.rsi = context.Rsi;
    m.rdx = context.Rdx;
    m.rcx = context.Rcx;
    m.r8 = context.R8;
    m.r9 = context.R9;
    m.rax = context.Rax;
    m.rbx = context.Rbx;
    m.rbp = context.Rbp;
    m.r10 = context.R10;
    m.r11 = context.R11;
    m.r12 = context.R12;
    m.r13 = context.R13;
    m.r14 = context.R14;
    m.r15 = context.R15;
    m.rip = context.Rip;
    m.rsp = context.Rsp;
    m.rflags = context.EFlags;
    m.cs = context.SegCs;
    m.ss = context.SegSs;
    m.len = sizeof(GuestMcontext);
    static_assert(sizeof(context.FltSave) <= sizeof(m.fpstate));
    std::memcpy(m.fpstate, &context.FltSave, sizeof(context.FltSave));
    handler(signum, &ucontext);
    context.Rdi = m.rdi;
    context.Rsi = m.rsi;
    context.Rdx = m.rdx;
    context.Rcx = m.rcx;
    context.R8 = m.r8;
    context.R9 = m.r9;
    context.Rax = m.rax;
    context.Rbx = m.rbx;
    context.Rbp = m.rbp;
    context.R10 = m.r10;
    context.R11 = m.r11;
    context.R12 = m.r12;
    context.R13 = m.r13;
    context.R14 = m.r14;
    context.R15 = m.r15;
    context.Rip = m.rip;
    context.Rsp = m.rsp;
    context.EFlags = static_cast<DWORD>(m.rflags);
    std::memcpy(&context.FltSave, m.fpstate, sizeof(context.FltSave));
}

[[noreturn]] void RedirectedEntry(Delivery* delivery) {
    CONTEXT context = delivery->context;
    Deliver(delivery->handler, delivery->signum, context);
    RtlRestoreContext(&context, nullptr);
    std::abort();
}

bool StackWritable(DWORD64 low, DWORD64 high) {
    for (DWORD64 address = low; address < high;) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) != sizeof(info)) return false;
        if (info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) != 0 || (info.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) == 0) return false;
        address = reinterpret_cast<DWORD64>(info.BaseAddress) + info.RegionSize;
    }
    return true;
}

void CALLBACK WaitingEntry(ULONG_PTR parameter) {
    auto* delivery = reinterpret_cast<Delivery*>(parameter);
    const auto handler = delivery->handler;
    const int signum = delivery->signum;
    delete delivery;
    CONTEXT context{};
    RtlCaptureContext(&context);
    Deliver(handler, signum, context);
}

static_assert(HomeArea + 8 == 40, "Aps5RedirectedEntryStub finds the delivery 40 bytes above its stack pointer");
static_assert(offsetof(Delivery, context) == 16 && offsetof(CONTEXT, Rax) == 0x78 && offsetof(CONTEXT, Rbp) == 0xa0 && offsetof(CONTEXT, R15) == 0xf0, "Aps5RedirectedEntryStub stores the live registers into the delivery's context");

bool Exited(HANDLE native) {
    return WaitForSingleObject(native, 0) == WAIT_OBJECT_0;
}

bool RaiseOn(Pthread thread, GuestExceptionHandler handler, int signum) {
    if (thread == scePthreadSelf()) {
        CONTEXT context{};
        RtlCaptureContext(&context);
        Deliver(handler, signum, context);
        return true;
    }
    const auto native = static_cast<HANDLE>(thread->nativeHandle);
    auto queued = std::make_unique<Delivery>(Delivery{handler, signum, {}});
    if (SuspendThread(native) == static_cast<DWORD>(-1)) {
        if (Exited(native)) return false;
        throw std::runtime_error("sceKernelRaiseException: cannot suspend the target thread");
    }
    if (Exited(native)) {
        ResumeThread(native);
        return false;
    }
    if (thread->waitCount.load(std::memory_order_seq_cst) > 0) {
        const bool accepted = QueueUserAPC(WaitingEntry, native, reinterpret_cast<ULONG_PTR>(queued.get())) != 0;
        ResumeThread(native);
        if (!accepted) throw std::runtime_error("sceKernelRaiseException: cannot queue delivery to the waiting thread");
        queued.release();
        return true;
    }
    alignas(16) Delivery delivery{handler, signum, {}};
    delivery.context.ContextFlags = CONTEXT_FULL | CONTEXT_FLOATING_POINT;
    if (!GetThreadContext(native, &delivery.context)) {
        ResumeThread(native);
        throw std::runtime_error("sceKernelRaiseException: cannot read the target thread context");
    }
    const DWORD64 slot = (delivery.context.Rsp - RedZone - sizeof(Delivery)) & ~static_cast<DWORD64>(15);
    if (!StackWritable(slot - HomeArea - 8, delivery.context.Rsp - RedZone)) {
        ResumeThread(native);
        throw std::runtime_error("sceKernelRaiseException: the target thread stack below its red zone is not committed");
    }
    std::memcpy(reinterpret_cast<void*>(slot), &delivery, sizeof(Delivery));
    CONTEXT redirected = delivery.context;
    redirected.Rsp = slot - HomeArea - 8;
    redirected.Rip = reinterpret_cast<DWORD64>(&Aps5RedirectedEntryStub);
    if (!SetThreadContext(native, &redirected)) {
        ResumeThread(native);
        throw std::runtime_error("sceKernelRaiseException: cannot redirect the target thread");
    }
    ResumeThread(native);
    return true;
}
#else
int NativeError(int error) {
    switch (error) {
    case 0: return 0;
    case EPERM: return SCE_KERNEL_ERROR_EPERM;
    case ESRCH: return SCE_KERNEL_ERROR_ESRCH;
    case EINTR: return SCE_KERNEL_ERROR_EINTR;
    case EAGAIN: return SCE_KERNEL_ERROR_EAGAIN;
    case ENOMEM: return SCE_KERNEL_ERROR_ENOMEM;
    case EINVAL: return SCE_KERNEL_ERROR_EINVAL;
    default: return SCE_KERNEL_ERROR_EOPNOTSUPP;
    }
}

struct BlockCallerSignal {
    sigset_t previous{};
    int error;
    int savedErrno = errno;

    BlockCallerSignal() {
        sigset_t blocked;
        ::sigemptyset(&blocked);
        ::sigaddset(&blocked, SIGUSR1);
        error = ::pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    }

    ~BlockCallerSignal() {
        if (error == 0) ::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
        errno = savedErrno;
    }
};

void LinuxDeliver(int signum, siginfo_t*, void* rawContext) {
    const int savedErrno = errno;
    const auto handler = linuxHandler.load(std::memory_order_acquire);
    if (signum != SIGUSR1 || handler == nullptr || rawContext == nullptr) {
        errno = savedErrno;
        return;
    }
    auto& native = *static_cast<ucontext_t*>(rawContext);
    auto& registers = native.uc_mcontext.gregs;
    alignas(64) GuestUcontext guest{};
    auto& m = guest.mcontext;
    CopyGuestSignalMask_nid_no_patch(&native.uc_sigmask, guest.sigmask);
    guest.stackPointer = native.uc_stack.ss_sp;
    guest.stackSize = native.uc_stack.ss_size;
    guest.stackFlags = ((native.uc_stack.ss_flags & SS_ONSTACK) ? 1 : 0)
                     | ((native.uc_stack.ss_flags & SS_DISABLE) ? 4 : 0);
    m.onstack = (native.uc_stack.ss_flags & SS_ONSTACK) ? 1 : 0;
    m.rdi = registers[REG_RDI];
    m.rsi = registers[REG_RSI];
    m.rdx = registers[REG_RDX];
    m.rcx = registers[REG_RCX];
    m.r8 = registers[REG_R8];
    m.r9 = registers[REG_R9];
    m.rax = registers[REG_RAX];
    m.rbx = registers[REG_RBX];
    m.rbp = registers[REG_RBP];
    m.r10 = registers[REG_R10];
    m.r11 = registers[REG_R11];
    m.r12 = registers[REG_R12];
    m.r13 = registers[REG_R13];
    m.r14 = registers[REG_R14];
    m.r15 = registers[REG_R15];
    m.trapno = static_cast<std::uint32_t>(registers[REG_TRAPNO]);
    m.addr = registers[REG_CR2];
    m.flags = 1;
    m.err = registers[REG_ERR];
    m.rip = registers[REG_RIP];
    m.rflags = registers[REG_EFL];
    m.rsp = registers[REG_RSP];
    const auto segments = static_cast<std::uint64_t>(registers[REG_CSGSFS]);
    m.cs = segments & 0xffff;
    m.gs = static_cast<std::uint16_t>(segments >> 16);
    m.fs = static_cast<std::uint16_t>(segments >> 32);
    m.ss = segments >> 48;
    m.len = sizeof(GuestMcontext);
    const auto fp = native.uc_mcontext.fpregs;
    static_assert(sizeof(*fp) == 512);
    m.fpformat = fp == nullptr ? 0x10000 : 0x10002;
    m.ownedfp = fp == nullptr ? 0x20000 : 0x20001;
    if (fp != nullptr) std::memcpy(m.fpstate, fp, sizeof(*fp));
    handler(30, &guest);
    registers[REG_RDI] = m.rdi;
    registers[REG_RSI] = m.rsi;
    registers[REG_RDX] = m.rdx;
    registers[REG_RCX] = m.rcx;
    registers[REG_R8] = m.r8;
    registers[REG_R9] = m.r9;
    registers[REG_RAX] = m.rax;
    registers[REG_RBX] = m.rbx;
    registers[REG_RBP] = m.rbp;
    registers[REG_R10] = m.r10;
    registers[REG_R11] = m.r11;
    registers[REG_R12] = m.r12;
    registers[REG_R13] = m.r13;
    registers[REG_R14] = m.r14;
    registers[REG_R15] = m.r15;
    registers[REG_RIP] = m.rip;
    registers[REG_RSP] = m.rsp;
    registers[REG_EFL] = m.rflags;
    if (fp != nullptr) std::memcpy(fp, m.fpstate, 464);
    errno = savedErrno;
}

bool LinuxOwnsSignal(const struct sigaction& action) {
    return (action.sa_flags & SA_SIGINFO) != 0 && action.sa_sigaction == LinuxDeliver;
}
#endif

}

#ifdef _WIN32
extern "C" [[noreturn]] void Aps5RedirectedEntry(void* delivery) {
    RedirectedEntry(static_cast<Delivery*>(delivery));
}
asm(".text\n"
    ".globl Aps5RedirectedEntryStub\n"
    "Aps5RedirectedEntryStub:\n"
    "    movq %rax, 176(%rsp)\n"
    "    movq %rcx, 184(%rsp)\n"
    "    movq %rdx, 192(%rsp)\n"
    "    movq %rbx, 200(%rsp)\n"
    "    movq %rbp, 216(%rsp)\n"
    "    movq %rsi, 224(%rsp)\n"
    "    movq %rdi, 232(%rsp)\n"
    "    movq %r8, 240(%rsp)\n"
    "    movq %r9, 248(%rsp)\n"
    "    movq %r10, 256(%rsp)\n"
    "    movq %r11, 264(%rsp)\n"
    "    movq %r12, 272(%rsp)\n"
    "    movq %r13, 280(%rsp)\n"
    "    movq %r14, 288(%rsp)\n"
    "    movq %r15, 296(%rsp)\n"
    "    leaq 40(%rsp), %rcx\n"
    "    jmp Aps5RedirectedEntry\n");
#endif

extern "C" {

int APS5_VABI sceKernelInstallExceptionHandler(int signum, void* handler) {
 if (!Allowed(signum) || handler == nullptr) return SCE_KERNEL_ERROR_EINVAL;
#ifdef _WIN32
 std::lock_guard lock(handlersLock);
 if (handlers[signum] != nullptr) return SCE_KERNEL_ERROR_EAGAIN;
 handlers[signum] = handler;
 return 0;
#else
 if (signum != 30) return SCE_KERNEL_ERROR_EOPNOTSUPP;
 BlockCallerSignal blocked;
 if (blocked.error != 0) return NativeError(blocked.error);
 std::scoped_lock lock(handlersLock, *NativeSignalRegistration_nid_no_patch());
 if (linuxHandler.load(std::memory_order_acquire) != nullptr) return SCE_KERNEL_ERROR_EAGAIN;
 struct sigaction current{};
 if (::sigaction(SIGUSR1, nullptr, &current) != 0) return NativeError(errno);
 if (current.sa_handler != SIG_DFL && current.sa_handler != SIG_IGN) return SCE_KERNEL_ERROR_EOPNOTSUPP;
 struct sigaction replacement{};
 replacement.sa_sigaction = LinuxDeliver;
 replacement.sa_flags = SA_SIGINFO | SA_RESTART;
 ::sigemptyset(&replacement.sa_mask);
 linuxHandler.store(reinterpret_cast<GuestExceptionHandler>(handler), std::memory_order_release);
 if (::sigaction(SIGUSR1, &replacement, nullptr) != 0) {
     linuxHandler.store(nullptr, std::memory_order_release);
     return NativeError(errno);
 }
 return 0;
#endif
}

int APS5_VABI sceKernelRemoveExceptionHandler(int signum) {
 if (!Allowed(signum)) return SCE_KERNEL_ERROR_EINVAL;
#ifdef _WIN32
 std::lock_guard lock(handlersLock);
 handlers[signum] = nullptr;
 return 0;
#else
 return SCE_KERNEL_ERROR_EOPNOTSUPP;
#endif
}

int APS5_VABI sceKernelRaiseException(Pthread thread, int signum) {
 if (signum != 30) return SCE_KERNEL_ERROR_EINVAL;
 if (thread == nullptr || thread->_finished.load(std::memory_order_acquire)) return SCE_KERNEL_ERROR_ESRCH;
#ifdef _WIN32
 const auto handler = Handler(signum);
 if (handler == nullptr) throw std::runtime_error("sceKernelRaiseException: no handler installed for the signal");
 return RaiseOn(thread, handler, signum) ? 0 : SCE_KERNEL_ERROR_ESRCH;
#else
 if (thread->nativeHandle == std::thread::native_handle_type{}) return SCE_KERNEL_ERROR_ESRCH;
 BlockCallerSignal blocked;
 if (blocked.error != 0) return NativeError(blocked.error);
 std::scoped_lock lock(handlersLock, *NativeSignalRegistration_nid_no_patch());
 if (linuxHandler.load(std::memory_order_acquire) == nullptr) return SCE_KERNEL_ERROR_EINVAL;
 struct sigaction current{};
 if (::sigaction(SIGUSR1, nullptr, &current) != 0) return NativeError(errno);
 if (!LinuxOwnsSignal(current)) return SCE_KERNEL_ERROR_EOPNOTSUPP;
 if (thread->_finished.load(std::memory_order_acquire)) return SCE_KERNEL_ERROR_ESRCH;
 return NativeError(::pthread_kill(thread->nativeHandle, SIGUSR1));
#endif
}

void APS5_VABI sceKernelDebugRaiseException(int c1, int c2) {
  APS5_LOG_OUT("sceKernelDebugRaiseException c1=%d c2=%d", c1, c2);
}

void APS5_VABI sceKernelDebugRaiseExceptionOnReleaseMode(int c1, int c2) {
  APS5_LOG_OUT("sceKernelDebugRaiseExceptionOnReleaseMode c1=%d c2=%d", c1, c2);
}

}
