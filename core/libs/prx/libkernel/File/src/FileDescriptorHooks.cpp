#include "prx/libc/include/GuestFileDescriptors.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libkernel/File/include/File.hpp"
#include "prx/libkernel/File/include/FileFlags.hpp"
#include "prx/libkernel/File/include/RandomDevice.hpp"
#include "prx/libkernel/File/include/DirectoryDescriptor.hpp"
#include "prx/libkernel/File/include/FileLock.hpp"
#include "prx/libkernel/File/include/DescriptorControl.hpp"
#include "prx/libkernel/Socket/include/SocketRuntime.hpp"
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <utility>

void EqueueDescriptorClosed(int descriptor);
struct KernelIovec;
struct KernelStatfs;
extern "C" int* APS5_VABI __error_nid_postfix();

namespace {
int KernelFailure(int error) { return static_cast<int>(0x80020000u | static_cast<unsigned>(error)); }
int PosixFailure(int error) { *__error_nid_postfix() = error; return -1; }
std::int64_t PosixResult(std::int64_t result) { return result < 0 ? PosixFailure(static_cast<int>(result) & 0xffff) : result; }

void NativeCleanup(int descriptor) noexcept {
    File::ForgetRandomDevice(descriptor);
    File::ForgetDescriptor(descriptor);
#ifdef _WIN32
    File::ForgetDirectoryDescriptor(descriptor);
    File::ForgetFileLock(descriptor);
#endif
}

template<typename Call>
auto WithDescriptor(int descriptor, int access, bool kernel, Call call) -> decltype(call(descriptor)) {
    using Result = decltype(call(descriptor));
    // Socket operations dispatch through their guest registry before native translation.
    if (GuestSockets::IsOpen(descriptor)) return call(descriptor);
    const auto lease = GuestFiles::GuestFileAcquire_nid_no_patch(descriptor);
    if (!lease) return static_cast<Result>(kernel ? KernelFailure(errno) : PosixFailure(errno));
    const int mode = GuestFiles::GuestFileAccessMode_nid_no_patch(lease);
    if ((access == 0 && mode == 1) || (access == 1 && mode == 0))
        return static_cast<Result>(kernel ? KernelFailure(9) : PosixFailure(9));
    const GuestSockets::NativeDescriptorScope nativeScope;
    const auto result = call(GuestFiles::GuestFileNativeDescriptor_nid_no_patch(lease));
    if (kernel && result == -1) return static_cast<Result>(KernelFailure(GuestFiles::GuestFileNativeError_nid_no_patch(errno)));
    return result;
}

int Adopt(int native, int access, bool kernel) {
    if (native < 0) return native;
    const auto lease = GuestFiles::GuestFileAdoptOwned_nid_no_patch(native, access, NativeCleanup);
    if (!lease) return kernel ? KernelFailure(errno) : PosixFailure(errno);
    return GuestFiles::GuestFileLogicalDescriptor_nid_no_patch(lease);
}
}

// The manifest drives both these wrappers and the source-local CMake renames.
// Calls between the upstream bodies use private native names, not these hooks.
#define FD_CUSTOM(name)
#define FD_KERNEL(result, name, parameters, arguments, access) \
    extern "C" result APS5_VABI name##_native_nid_no_patch parameters; \
    extern "C" result APS5_VABI name parameters { \
        return WithDescriptor(d, access, true, [&](int d) { return name##_native_nid_no_patch arguments; }); \
    }
#define FD_POSIX(result, name, parameters, arguments, access) \
    extern "C" result APS5_VABI name##_native_nid_no_patch parameters; \
    extern "C" result APS5_VABI name parameters { \
        return WithDescriptor(d, access, false, [&](int d) { return name##_native_nid_no_patch arguments; }); \
    }
#include "prx/libkernel/File/include/FileDescriptorHooks.def"
#undef FD_CUSTOM
#undef FD_KERNEL
#undef FD_POSIX

extern "C" {
int APS5_VABI sceKernelOpen_native_nid_no_patch(const char*, int, std::uint16_t);
int APS5_VABI sceKernelFcntl_native_nid_no_patch(int, int, ...);
std::int64_t APS5_VABI sceKernelLseek_native_nid_no_patch(int, std::int64_t, int);
int APS5_VABI open_nid_postfix_native_nid_no_patch(const char*, int, int);
int APS5_VABI dup_nid_postfix_native_nid_no_patch(int);
int APS5_VABI pipe_nid_postfix_native_nid_no_patch(int*);
int APS5_VABI shm_open_nid_postfix_native_nid_no_patch(const char*, int, int);

int APS5_VABI sceKernelOpen(const char* path, int flags, std::uint16_t mode) {
    if (!path) return KernelFailure(14);
    if ((flags & SCE_KERNEL_O_ACCMODE) == SCE_KERNEL_O_ACCMODE) return KernelFailure(22);
    if (GuestFiles::GuestFileInitializeStandards_nid_no_patch() != 0) return KernelFailure(errno);
    return Adopt(sceKernelOpen_native_nid_no_patch(path, flags, mode), flags & SCE_KERNEL_O_ACCMODE, true);
}

int APS5_VABI open_nid_postfix(const char* path, int flags, int mode) {
    return static_cast<int>(PosixResult(sceKernelOpen(path, flags, static_cast<std::uint16_t>(mode))));
}
int APS5_VABI creat_nid_postfix(const char* path, int mode) {
    return open_nid_postfix(path, SCE_KERNEL_O_WRONLY | SCE_KERNEL_O_CREAT | SCE_KERNEL_O_TRUNC, mode);
}
int APS5_VABI _open_nid_postfix(const char* path, int flags, ...) {
    int mode = 0;
    if (flags & SCE_KERNEL_O_CREAT) {
#ifdef _WIN32
        __builtin_sysv_va_list arguments;
        __builtin_sysv_va_start(arguments, flags);
        mode = __builtin_va_arg(arguments, int);
        __builtin_sysv_va_end(arguments);
#else
        std::va_list arguments;
        va_start(arguments, flags);
        mode = va_arg(arguments, int);
        va_end(arguments);
#endif
    }
    return open_nid_postfix(path, flags, mode);
}
std::int64_t APS5_VABI sceKernelLseek(int d, std::int64_t offset, int whence) {
    if (whence < 0 || whence > 2) return KernelFailure(22);
    return WithDescriptor(d, -1, true, [&](int native) { return sceKernelLseek_native_nid_no_patch(native, offset, whence); });
}
int APS5_VABI fcntl_nid_postfix_native_nid_no_patch(int, int, ...);
int APS5_VABI dup2_nid_postfix(int, int);
static int GuestFcntl(int descriptor, int command, std::intptr_t argument) {
    if (GuestSockets::IsOpen(descriptor)) {
        const auto type = File::FcntlArgumentOf(command);
        if (type == File::FcntlArgument::Int) return fcntl_nid_postfix_native_nid_no_patch(descriptor, command, static_cast<int>(argument));
        if (type == File::FcntlArgument::Pointer) return fcntl_nid_postfix_native_nid_no_patch(descriptor, command, reinterpret_cast<void*>(argument));
        return fcntl_nid_postfix_native_nid_no_patch(descriptor, command);
    }
    const auto lease = GuestFiles::GuestFileAcquire_nid_no_patch(descriptor);
    if (!lease) return PosixFailure(errno);
    if (command == 10 || command == 18) {
        const auto duplicate = dup2_nid_postfix(descriptor, static_cast<int>(argument));
        if (duplicate < 0 || command == 10) return duplicate;
        const auto destination = GuestFiles::GuestFileAcquire_nid_no_patch(duplicate);
        const GuestSockets::NativeDescriptorScope nativeScope;
        return File::Fcntl(GuestFiles::GuestFileNativeDescriptor_nid_no_patch(destination), 2, 1) < 0 ? -1 : duplicate;
    }
    const GuestSockets::NativeDescriptorScope nativeScope;
    const bool duplicate = command == 0 || command == 17;
    const auto result = File::Fcntl(GuestFiles::GuestFileNativeDescriptor_nid_no_patch(lease), command, duplicate ? 0 : argument);
    if (result < 0 || !duplicate) return result;
    const auto adopted = GuestFiles::GuestFileAdoptOwned_nid_no_patch(result, GuestFiles::GuestFileAccessMode_nid_no_patch(lease), NativeCleanup, static_cast<int>(argument));
    return adopted ? GuestFiles::GuestFileLogicalDescriptor_nid_no_patch(adopted) : PosixFailure(errno);
}
#define READ_FCNTL_ARGUMENT \
    const auto type = File::FcntlArgumentOf(command); \
    std::intptr_t argument = 0; \
    if (type != File::FcntlArgument::None) { \
        APS5_FCNTL_VA_LIST arguments; \
        APS5_FCNTL_VA_START(arguments, command); \
        argument = type == File::FcntlArgument::Int ? APS5_FCNTL_VA_ARG(arguments, int) : reinterpret_cast<std::intptr_t>(APS5_FCNTL_VA_ARG(arguments, void*)); \
        APS5_FCNTL_VA_END(arguments); \
    }
#ifdef _WIN32
#define APS5_FCNTL_VA_LIST __builtin_sysv_va_list
#define APS5_FCNTL_VA_START __builtin_sysv_va_start
#define APS5_FCNTL_VA_ARG __builtin_va_arg
#define APS5_FCNTL_VA_END __builtin_sysv_va_end
#else
#define APS5_FCNTL_VA_LIST std::va_list
#define APS5_FCNTL_VA_START va_start
#define APS5_FCNTL_VA_ARG va_arg
#define APS5_FCNTL_VA_END va_end
#endif
int APS5_VABI sceKernelFcntl(int descriptor, int command, ...) {
    READ_FCNTL_ARGUMENT;
    const auto result = GuestFcntl(descriptor, command, argument);
    return result < 0 ? KernelFailure(*__error_nid_postfix()) : result;
}
int APS5_VABI fcntl_nid_postfix(int descriptor, int command, ...) {
    READ_FCNTL_ARGUMENT;
    return GuestFcntl(descriptor, command, argument);
}
#undef READ_FCNTL_ARGUMENT
#undef APS5_FCNTL_VA_LIST
#undef APS5_FCNTL_VA_START
#undef APS5_FCNTL_VA_ARG
#undef APS5_FCNTL_VA_END
int APS5_VABI sceKernelClose(int descriptor) {
    int socketError = 0;
    if (KernelCloseSocket_nid_no_patch(descriptor, &socketError)) return socketError == 0 ? 0 : KernelFailure(socketError);
    if (GuestSockets::IsOpen(descriptor))
        return GuestSockets::Close(descriptor) == 0 ? 0 : KernelFailure(*__error_nid_postfix());
    if (GuestFiles::GuestFileClose_nid_no_patch(descriptor) != 0) return KernelFailure(errno);
    EqueueDescriptorClosed(descriptor);
    return 0;
}
int APS5_VABI close_nid_postfix(int descriptor) {
    return static_cast<int>(PosixResult(sceKernelClose(descriptor)));
}
int APS5_VABI _close_nid_postfix(int descriptor) { return close_nid_postfix(descriptor); }

int APS5_VABI dup_nid_postfix(int descriptor) {
    if (GuestSockets::IsOpen(descriptor)) return GuestSockets::Duplicate(descriptor);
    const auto lease = GuestFiles::GuestFileAcquire_nid_no_patch(descriptor);
    if (!lease) return PosixFailure(errno);
    return Adopt(dup_nid_postfix_native_nid_no_patch(GuestFiles::GuestFileNativeDescriptor_nid_no_patch(lease)),
        GuestFiles::GuestFileAccessMode_nid_no_patch(lease), false);
}
int APS5_VABI dup2_nid_postfix(int from, int to) {
    if (GuestSockets::IsOpen(from)) return GuestSockets::DuplicateTo(from, to);
    if (to < 0 || to >= 32768) return PosixFailure(9);
    const auto lease = GuestFiles::GuestFileAcquire_nid_no_patch(from);
    if (!lease) return PosixFailure(errno);
    if (from == to) return to;
    if (GuestSockets::IsOpen(to) && GuestSockets::Close(to) != 0) return -1;
    const int native = dup_nid_postfix_native_nid_no_patch(GuestFiles::GuestFileNativeDescriptor_nid_no_patch(lease));
    if (native < 0) return -1;
    const auto replacement = GuestFiles::GuestFileReplaceOwned_nid_no_patch(to, native,
        GuestFiles::GuestFileAccessMode_nid_no_patch(lease), NativeCleanup);
    if (!replacement) return PosixFailure(errno);
    EqueueDescriptorClosed(to);
    return to;
}
int APS5_VABI pipe_nid_postfix(int* descriptors) {
    if (!descriptors) return PosixFailure(14);
    const GuestArena::HostWrite destination(descriptors, 2 * sizeof(int));
    if (!destination.Open()) return PosixFailure(14);
    int native[2];
    if (pipe_nid_postfix_native_nid_no_patch(native) != 0) return -1;
    const auto pair = GuestFiles::GuestFileAdoptPairOwned_nid_no_patch(native[0], native[1], 0, 1, NativeCleanup);
    if (!pair[0]) return PosixFailure(errno);
    const int logical[2] = {GuestFiles::GuestFileLogicalDescriptor_nid_no_patch(pair[0]),
                            GuestFiles::GuestFileLogicalDescriptor_nid_no_patch(pair[1])};
    std::memcpy(descriptors, logical, sizeof(logical));
    return 0;
}
int APS5_VABI shm_open_nid_postfix(const char* path, int flags, int mode) {
    return Adopt(shm_open_nid_postfix_native_nid_no_patch(path, flags, mode), flags & SCE_KERNEL_O_ACCMODE, false);
}
std::int64_t APS5_VABI pread_nid_postfix(int d, void* buf, std::size_t count, std::int64_t offset) {
    return PosixResult(sceKernelPread(d, buf, count, offset));
}
std::int64_t APS5_VABI pwrite_nid_disambig1_nid_postfix(int d, const void* buf, std::size_t count, std::int64_t offset) {
    return PosixResult(sceKernelPwrite(d, buf, count, offset));
}
}

bool DescriptorIsOpen_nid_no_patch(int descriptor) {
    if (GuestSockets::IsOpen(descriptor)) return GuestSockets::IsOpen(descriptor);
    return static_cast<bool>(GuestFiles::GuestFileAcquire_nid_no_patch(descriptor));
}

extern "C" int APS5_VABI tcsetattr_nid_postfix_native_nid_no_patch(int, int, const void*);
extern "C" int APS5_VABI tcsetattr_nid_postfix(int d, int action, const void* attributes) {
    const int mode = action & ~0x10;
    if (mode < 0 || mode > 2 || !attributes)
        return tcsetattr_nid_postfix_native_nid_no_patch(-1, action, attributes);
    return WithDescriptor(d, -1, false, [&](int native) {
        return tcsetattr_nid_postfix_native_nid_no_patch(native, action, attributes);
    });
}

extern "C" const char* GetDirectoryDescriptorPath_native_nid_no_patch(int, int*);
extern "C" const char* GetDirectoryDescriptorPath_nid_no_patch(int d, int* error) {
    if (!error) return nullptr;
    const auto lease = GuestFiles::GuestFileAcquire_nid_no_patch(d);
    if (!lease) { *error = errno; return nullptr; }
    return GetDirectoryDescriptorPath_native_nid_no_patch(GuestFiles::GuestFileNativeDescriptor_nid_no_patch(lease), error);
}

extern "C" int APS5_VABI tcgetpgrp_nid_postfix_native_nid_no_patch(int);
extern "C" int APS5_VABI tcsetpgrp_nid_postfix_native_nid_no_patch(int, int);
extern "C" int APS5_VABI tcgetpgrp_nid_postfix(int descriptor) {
    return WithDescriptor(descriptor, -1, false, [&](int native) {
        return descriptor >= 0 && descriptor < 3 ? PosixFailure(25) : tcgetpgrp_nid_postfix_native_nid_no_patch(native);
    });
}
extern "C" int APS5_VABI tcsetpgrp_nid_postfix(int descriptor, int group) {
    return WithDescriptor(descriptor, -1, false, [&](int native) {
        return descriptor >= 0 && descriptor < 3 ? PosixFailure(25) : tcsetpgrp_nid_postfix_native_nid_no_patch(native, group);
    });
}

extern "C" int APS5_VABI tcflush_nid_postfix_native_nid_no_patch(int, int);
extern "C" int APS5_VABI tcflow_nid_postfix_native_nid_no_patch(int, int);
extern "C" int APS5_VABI tcflush_nid_postfix(int descriptor, int queue) {
    if (queue < 1 || queue > 3) return PosixFailure(22);
    return WithDescriptor(descriptor, -1, false, [&](int native) { return tcflush_nid_postfix_native_nid_no_patch(native, queue); });
}
extern "C" int APS5_VABI tcflow_nid_postfix(int descriptor, int action) {
    if (action < 1 || action > 4) return PosixFailure(22);
    return WithDescriptor(descriptor, -1, false, [&](int native) { return tcflow_nid_postfix_native_nid_no_patch(native, action); });
}
