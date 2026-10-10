#include "prx/libkernel/File/include/FileFlags.hpp"
#include "prx/libkernel/File/include/NativeStat.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestFileDescriptors.hpp"
#include "prx/libkernel/File/include/File.hpp"
#include "prx/libkernel/File/include/DirectoryDescriptor.hpp"
#include "prx/libkernel/Socket/include/SocketRuntime.hpp"
#include "prx/libkernel/KernelErrors.hpp"
#include "SceTypes.hpp"

#include <cerrno>
#include <limits>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
extern "C" _invalid_parameter_handler _set_thread_local_invalid_parameter_handler(_invalid_parameter_handler);
static void IgnoreInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, std::uintptr_t) {}
static int NativeOpen(const std::filesystem::path& p, int nativeFlags, std::uint16_t mode) {
    return ::_wopen(p.wstring().c_str(), nativeFlags, static_cast<int>(mode));
}
static std::int64_t NativeLseek(int fd, std::int64_t offset, int whence) {
    const int initialError = errno;
    const auto previous = _set_thread_local_invalid_parameter_handler(IgnoreInvalidParameter);
    errno = initialError;
    const auto result = ::_lseeki64(fd, offset, whence);
    const int error = errno;
    _set_thread_local_invalid_parameter_handler(previous);
    errno = error;
    return result;
}
static int NativeRead(int fd, void* buf, std::size_t n) {
    if (n > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        errno = EINVAL;
        return -1;
    }
    char empty = 0;
    return ::_read(fd, buf == nullptr ? &empty : buf, static_cast<unsigned int>(n));
}
static int NativeWrite(int fd, const void* buf, std::size_t n) {
    if (n > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        errno = EINVAL;
        return -1;
    }
    const char empty = 0;
    return ::_write(fd, buf == nullptr ? &empty : buf, static_cast<unsigned int>(n));
}
static void NativeCleanup(int fd) noexcept {
    File::ForgetDirectoryDescriptor(fd);
}
static int NativeUnlink(const std::filesystem::path& p) {
    return ::_wunlink(p.wstring().c_str());
}
static int MapFlags(int sceFlags) {
    int f = 0;
    const int acc = sceFlags & SCE_KERNEL_O_ACCMODE;
    if (acc == SCE_KERNEL_O_RDONLY) f |= _O_RDONLY;
    else if (acc == SCE_KERNEL_O_WRONLY) f |= _O_WRONLY;
    else if (acc == SCE_KERNEL_O_RDWR) f |= _O_RDWR;
    else throw std::invalid_argument("sceKernelOpen: invalid access mode");
    if (sceFlags & SCE_KERNEL_O_APPEND) f |= _O_APPEND;
    if (sceFlags & SCE_KERNEL_O_CREAT) f |= _O_CREAT;
    if (sceFlags & SCE_KERNEL_O_TRUNC) f |= _O_TRUNC;
    if (sceFlags & SCE_KERNEL_O_EXCL) f |= _O_EXCL;
    f |= _O_BINARY;
    return f;
}
#else
#include <fcntl.h>
#include <unistd.h>
static int NativeOpen(const std::filesystem::path& p, int nativeFlags, std::uint16_t mode) {
    return ::open(p.c_str(), nativeFlags, static_cast<mode_t>(mode));
}
static std::int64_t NativeLseek(int fd, std::int64_t offset, int whence) {
    return ::lseek(fd, static_cast<off_t>(offset), whence);
}
static std::int64_t NativeRead(int fd, void* buf, std::size_t n) {
    return ::read(fd, buf, n);
}
static std::int64_t NativeWrite(int fd, const void* buf, std::size_t n) {
    return ::write(fd, buf, n);
}
static constexpr GuestFiles::NativeCleanup NativeCleanup = nullptr;
static int NativeUnlink(const std::filesystem::path& p) {
    return ::unlink(p.c_str());
}
static int MapFlags(int sceFlags) {
    int f = 0;
    const int acc = sceFlags & SCE_KERNEL_O_ACCMODE;
    if (acc == SCE_KERNEL_O_RDONLY) f |= O_RDONLY;
    else if (acc == SCE_KERNEL_O_WRONLY) f |= O_WRONLY;
    else if (acc == SCE_KERNEL_O_RDWR) f |= O_RDWR;
    else throw std::invalid_argument("sceKernelOpen: invalid access mode");
    if (sceFlags & SCE_KERNEL_O_APPEND) f |= O_APPEND;
    if (sceFlags & SCE_KERNEL_O_CREAT) f |= O_CREAT;
    if (sceFlags & SCE_KERNEL_O_TRUNC) f |= O_TRUNC;
    if (sceFlags & SCE_KERNEL_O_EXCL) f |= O_EXCL;
    if (sceFlags & SCE_KERNEL_O_SYNC) f |= O_SYNC;
    if (sceFlags & SCE_KERNEL_O_NONBLOCK) f |= O_NONBLOCK;
    if (sceFlags & SCE_KERNEL_O_DIRECTORY) f |= O_DIRECTORY;
    return f;
}
#endif

static int SceErrorFromGuest(int error) {
    return static_cast<int>(0x80020000u | static_cast<unsigned>(error));
}
extern "C" int* APS5_VABI __error_nid_postfix();

static int SceErrorFromErrno(int error) {
    return SceErrorFromGuest(GuestFiles::GuestFileNativeError_nid_no_patch(error));
}

extern "C" {

int APS5_VABI sceKernelOpen(const char* path, int flags, std::uint16_t mode) {
    if (path == nullptr) return SCE_KERNEL_ERROR_EFAULT;
    if (*path == '\0') return SCE_KERNEL_ERROR_ENOENT;
    if ((flags & SCE_KERNEL_O_ACCMODE) == SCE_KERNEL_O_ACCMODE) return SCE_KERNEL_ERROR_EINVAL;
    if (GuestFiles::GuestFileInitializeStandards_nid_no_patch() != 0) return SceErrorFromGuest(errno);
    const int nativeFlags = MapFlags(flags);
    APS5_LOG_OUT("path=%s flags=0x%X nativeFlags=0x%X mode=0%o", path, flags, nativeFlags, mode);
    auto native = ResolvePath_nid_no_patch(path);
    int fd = NativeOpen(native, nativeFlags, mode);
#ifdef _WIN32
    if (fd < 0 && errno != ENOENT && (flags & SCE_KERNEL_O_ACCMODE) == SCE_KERNEL_O_RDONLY) {
        std::error_code error;
        if (std::filesystem::is_directory(native, error)) {
            if ((flags & (SCE_KERNEL_O_CREAT | SCE_KERNEL_O_EXCL)) == (SCE_KERNEL_O_CREAT | SCE_KERNEL_O_EXCL)) errno = EEXIST;
            else if ((flags & SCE_KERNEL_O_ACCMODE) != SCE_KERNEL_O_RDONLY || (flags & (SCE_KERNEL_O_CREAT | SCE_KERNEL_O_TRUNC))) errno = EISDIR;
            else fd = File::OpenDirectoryDescriptor(native);
        }
    }
#endif
    if (fd < 0) return SceErrorFromErrno(errno);
    const auto lease = GuestFiles::GuestFileAdoptOwned_nid_no_patch(fd, flags & SCE_KERNEL_O_ACCMODE, NativeCleanup);
    if (!lease) return SceErrorFromGuest(errno);
    if ((flags & SCE_KERNEL_O_ACCMODE) != SCE_KERNEL_O_RDONLY || (flags & (SCE_KERNEL_O_CREAT | SCE_KERNEL_O_TRUNC)))
        RecordWrittenPath_nid_no_patch(native);
    return GuestFiles::GuestFileLogicalDescriptor_nid_no_patch(lease);
}

int APS5_VABI sceKernelClose(int d) {
    return GuestFiles::GuestFileClose_nid_no_patch(d) == 0 ? 0 : SceErrorFromGuest(errno);
}

std::int64_t APS5_VABI sceKernelRead(int d, void* buf, std::size_t nbytes) {
    if (d >= GuestSockets::FirstDescriptor) {
        const auto n = GuestSockets::Read(d, buf, nbytes);
        return n < 0 ? SceKernelError(*__error_nid_postfix()) : n;
    }
    const auto lease = GuestFiles::GuestFileAcquire_nid_no_patch(d);
    if (!lease) return SceErrorFromGuest(errno);
    if (GuestFiles::GuestFileAccessMode_nid_no_patch(lease) == 1) return SCE_KERNEL_ERROR_EBADF;
    if (buf == nullptr && nbytes != 0) return SCE_KERNEL_ERROR_EFAULT;
    const GuestArena::HostWrite destination(buf, nbytes);
    if (!destination.Open()) return SCE_KERNEL_ERROR_EFAULT;
    const auto result = NativeRead(GuestFiles::GuestFileNativeDescriptor_nid_no_patch(lease), buf, nbytes);
    return result < 0 ? SceErrorFromErrno(errno) : static_cast<std::int64_t>(result);
}

std::int64_t APS5_VABI sceKernelWrite(int d, const void* buf, std::size_t nbytes) {
    if (d >= GuestSockets::FirstDescriptor) {
        const auto n = GuestSockets::Write(d, buf, nbytes);
        return n < 0 ? SceKernelError(*__error_nid_postfix()) : n;
    }
    const auto lease = GuestFiles::GuestFileAcquire_nid_no_patch(d);
    if (!lease) return SceErrorFromGuest(errno);
    if (GuestFiles::GuestFileAccessMode_nid_no_patch(lease) == 0) return SCE_KERNEL_ERROR_EBADF;
    if (buf == nullptr && nbytes != 0) return SCE_KERNEL_ERROR_EFAULT;
    const auto result = NativeWrite(GuestFiles::GuestFileNativeDescriptor_nid_no_patch(lease), buf, nbytes);
    return result < 0 ? SceErrorFromErrno(errno) : static_cast<std::int64_t>(result);
}

std::int64_t APS5_VABI sceKernelLseek(int d, std::int64_t offset, int whence) {
    if (whence < 0 || whence > 2) return SCE_KERNEL_ERROR_EINVAL;
    const auto lease = GuestFiles::GuestFileAcquire_nid_no_patch(d);
    if (!lease) return SceErrorFromGuest(errno);
    const auto result = NativeLseek(GuestFiles::GuestFileNativeDescriptor_nid_no_patch(lease), offset, whence);
    return result < 0 ? SceErrorFromErrno(errno) : result;
}

int APS5_VABI sceKernelStat(const char* path, FileStat* sb) {
    if (path == nullptr) {
        throw std::invalid_argument(std::string(__func__) + ": path is null");
    }
    if (sb == nullptr) {
        throw std::invalid_argument(std::string(__func__) + ": sb is null");
    }
    const auto native = ResolvePath_nid_no_patch(path);
    std::error_code error;
    if (!std::filesystem::exists(native, error)) {
        return SceErrorFromErrno(2);
    }
    File::FillFileStat(native, sb);
    return 0;
}

int APS5_VABI sceKernelUnlink(const char* path) {
    if (path == nullptr) {
        throw std::invalid_argument(std::string(__func__) + ": path is null");
    }
    auto native = ResolvePath_nid_no_patch(path);
    constexpr int GuestEperm = 1;
    std::error_code error;
    if (std::filesystem::is_directory(std::filesystem::symlink_status(native, error))) return SceErrorFromErrno(GuestEperm);
    if (NativeUnlink(native) != 0) {
        return SceErrorFromErrno(errno);
    }
    RecordWrittenPath_nid_no_patch(native);
    return 0;
}

int APS5_VABI sceKernelFcntl() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

}
