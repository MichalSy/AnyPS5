#include "prx/libc/include/FileStream.hpp"
#include <cerrno>
#include <cstring>
#include <memory>
#include <system_error>
#ifdef _WIN32
#include <io.h>
extern "C" _invalid_parameter_handler _set_thread_local_invalid_parameter_handler(_invalid_parameter_handler);
#else
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

struct FileStreamState {
    std::FILE* handle;
    GuestFiles::Identity identity;
    int descriptor;
    FileStreamState* next = nullptr;
};

namespace {
std::mutex streamsMutex;
FileStreamState* streams = nullptr;
#ifdef _WIN32
void IgnoreInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, std::uintptr_t) {}
#endif

int NativeDescriptor(std::FILE* handle) {
#ifdef _WIN32
    return ::_fileno(handle);
#else
    return ::fileno(handle);
#endif
}

int NativeDuplicate(int descriptor) {
#ifdef _WIN32
    return ::_dup(descriptor);
#else
    return ::fcntl(descriptor, F_DUPFD_CLOEXEC, 3);
#endif
}

void NativeClose(int descriptor) {
#ifdef _WIN32
    ::_close(descriptor);
#else
    ::close(descriptor);
#endif
}

std::FILE* AttachNative(int descriptor, const char* mode) {
#ifdef _WIN32
    return ::_fdopen(descriptor, mode);
#else
    return ::fdopen(descriptor, mode);
#endif
}

int AccessMode(std::FILE* handle) {
#ifdef _WIN32
    static_cast<void>(handle);
    return 2;
#else
    return ::fcntl(NativeDescriptor(handle), F_GETFL) & O_ACCMODE;
#endif
}

int ModeAccess(const char* mode) {
    return std::strchr(mode, '+') ? 2 : *mode == 'r' ? 0 : 1;
}

FileStreamState* CreateState(std::FILE* handle, const GuestFiles::Lease& lease, int descriptor = -1) {
    auto* state = new FileStreamState{handle, lease,
        descriptor < 0 ? GuestFiles::GuestFileLogicalDescriptor_nid_no_patch(lease) : descriptor};
    std::lock_guard lock(streamsMutex);
    state->next = streams;
    streams = state;
    return state;
}

GuestFiles::Lease AdoptStream(std::FILE* handle, int accessMode) {
    if (GuestFiles::GuestFileInitializeStandards_nid_no_patch() != 0) return {};
    const int descriptor = NativeDuplicate(NativeDescriptor(handle));
    if (descriptor < 0) { errno = GuestFiles::GuestFileNativeError_nid_no_patch(errno); return {}; }
    return GuestFiles::GuestFileAdoptOwned_nid_no_patch(descriptor, accessMode);
}
}

extern "C" std::mutex& GuestFileStreamMutex_nid_no_patch() { return streamsMutex; }

extern "C" int GuestFileStreamCheckRedirect_nid_no_patch(const GuestFiles::Lease& previous) {
    if (!previous) return 0;
#ifndef _WIN32
    struct rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) != 0) return GuestFiles::GuestFileNativeError_nid_no_patch(errno);
#endif
    for (auto* state = streams; state; state = state->next) {
        if (!state->handle || state->identity.lock() != previous) continue;
        const int target = NativeDescriptor(state->handle);
#ifdef _WIN32
        const auto handler = _set_thread_local_invalid_parameter_handler(IgnoreInvalidParameter);
        const bool valid = ::_get_osfhandle(target) != -1;
        _set_thread_local_invalid_parameter_handler(handler);
        if (!valid) return 9;
#else
        if (target < 0 || static_cast<rlim_t>(target) >= limit.rlim_cur || ::fcntl(target, F_GETFD) < 0) return 9;
#endif
    }
    return 0;
}

extern "C" void GuestFileStreamRedirect_nid_no_patch(const GuestFiles::Lease& previous,
    const GuestFiles::Lease& replacement) {
    if (!previous) return;
    const int saved = errno;
    const int native = GuestFiles::GuestFileNativeDescriptor_nid_no_patch(replacement);
    for (auto* state = streams; state; state = state->next) {
        if (!state->handle || state->identity.lock() != previous) continue;
        const int target = NativeDescriptor(state->handle);
#ifdef _WIN32
        if (::_dup2(native, target) != 0) continue;
#else
        int result;
        do { result = ::dup2(native, target); } while (result < 0 && errno == EINTR);
        if (result < 0) continue;
#endif
        state->identity = replacement;
    }
    errno = saved;
}

FileStream::FileStream(std::FILE* handle, bool dynamic) : dynamic(dynamic) {
    if (!handle) throw std::runtime_error("FileStream: null handle");
    if (handle == stdin || handle == stdout || handle == stderr) {
        const int descriptor = handle == stdin ? 0 : handle == stdout ? 1 : 2;
        _guest.flags = descriptor == 0 ? 4 : 8;
        _guest.descriptor = static_cast<std::int16_t>(descriptor);
        const auto lease = GuestFiles::GuestFileAcquire_nid_no_patch(descriptor);
        if (!lease) { state = CreateState(nullptr, {}, descriptor); return; }
        const int duplicate = GuestFiles::GuestFileDuplicateNative_nid_no_patch(lease);
        if (duplicate < 0) { state = CreateState(nullptr, lease, descriptor); return; }
        auto* native = AttachNative(duplicate, descriptor == 0 ? "rb" : "wb");
        if (!native) {
            const int error = GuestFiles::GuestFileNativeError_nid_no_patch(errno);
            NativeClose(duplicate);
            state = CreateState(nullptr, lease, descriptor);
            errno = error;
            return;
        }
        if (descriptor == 2) std::setvbuf(native, nullptr, _IONBF, 0);
        try { state = CreateState(native, lease); }
        catch (...) { std::fclose(native); throw; }
        return;
    }
    const auto lease = AdoptStream(handle, AccessMode(handle));
    if (!lease) throw std::system_error(errno, std::generic_category(), "FileStream: descriptor adoption failed");
    try { state = CreateState(handle, lease); }
    catch (...) { GuestFiles::GuestFileCloseMatching_nid_no_patch(lease); throw; }
    _guest.flags = 0x10;
    _guest.descriptor = static_cast<std::int16_t>(state->descriptor);
}

FileStream::FileStream(std::FILE* handle, int accessMode, bool dynamic) : dynamic(dynamic) {
    if (!handle) throw std::runtime_error("FileStream: null handle");
    const auto lease = AdoptStream(handle, accessMode);
    if (!lease) throw std::system_error(errno, std::generic_category(), "FileStream: descriptor adoption failed");
    try { state = CreateState(handle, lease); }
    catch (...) { GuestFiles::GuestFileCloseMatching_nid_no_patch(lease); throw; }
    _guest.flags = 0x10;
    _guest.descriptor = static_cast<std::int16_t>(state->descriptor);
}

FileStream::FileStream(std::FILE* handle, const GuestFiles::Lease& lease, bool dynamic) : dynamic(dynamic) {
    if (!handle || !lease || !GuestFiles::GuestFileMatches_nid_no_patch(lease))
        throw std::system_error(9, std::generic_category(), "FileStream: invalid descriptor identity");
    state = CreateState(handle, lease);
    _guest.flags = 0x10;
    _guest.descriptor = static_cast<std::int16_t>(state->descriptor);
}

FileStream::~FileStream() {
    const int saved = errno;
    if (state && state->handle) Close();
    std::lock_guard lock(streamsMutex);
    for (auto** entry = &streams; *entry; entry = &(*entry)->next) {
        if (*entry == state) { *entry = state->next; break; }
    }
    delete state;
    errno = saved;
}

std::FILE* FileStream::GetHandle() {
    std::lock_guard lock(streamsMutex);
    if (state && state->handle && GuestFiles::GuestFileMatches_nid_no_patch(state->identity)) return state->handle;
    _guest.flags = static_cast<std::int16_t>(_guest.flags | 0x40);
    _guest.readRemaining = 0;
    _guest.writeRemaining = 0;
    errno = 9;
    return nullptr;
}

int FileStream::Descriptor() {
    if (!GetHandle()) return -1;
    _guest.descriptor = static_cast<std::int16_t>(state->descriptor);
    return state->descriptor;
}

bool FileStream::Reopen(const std::filesystem::path& filename, const char* mode) {
#ifdef _WIN32
    std::wstring wideMode;
    for (const char character : std::string_view(mode)) wideMode.push_back(static_cast<unsigned char>(character));
#endif
    if (!GetHandle()) return false;
    GuestFiles::Identity expected;
    std::FILE* previous;
    {
        std::lock_guard lock(streamsMutex);
        expected = state->identity;
        previous = std::exchange(state->handle, nullptr);
    }
#ifdef _WIN32
    std::unique_ptr<std::FILE, decltype(&std::fclose)> replacement(::_wfreopen(filename.c_str(), wideMode.c_str(), previous), std::fclose);
#else
    std::unique_ptr<std::FILE, decltype(&std::fclose)> replacement(std::freopen(filename.c_str(), mode, previous), std::fclose);
#endif
    const int openError = errno;
    if (!replacement) {
        GuestFiles::GuestFileCloseMatching_nid_no_patch(expected);
        _guest = {};
        errno = GuestFiles::GuestFileNativeError_nid_no_patch(openError);
        return false;
    }
    const int duplicate = NativeDuplicate(NativeDescriptor(replacement.get()));
    if (duplicate < 0) {
        const int error = GuestFiles::GuestFileNativeError_nid_no_patch(errno);
        GuestFiles::GuestFileCloseMatching_nid_no_patch(expected);
        _guest = {};
        replacement.reset();
        errno = error;
        return false;
    }
    const auto lease = GuestFiles::GuestFileReplaceOwnedMatching_nid_no_patch(expected, duplicate, ModeAccess(mode));
    if (!lease) {
        const int error = errno;
        GuestFiles::GuestFileCloseMatching_nid_no_patch(expected);
        _guest = {};
        replacement.reset();
        errno = error;
        return false;
    }
    {
        std::lock_guard lock(streamsMutex);
        state->handle = replacement.release();
        state->identity = lease;
    }
    state->descriptor = GuestFiles::GuestFileLogicalDescriptor_nid_no_patch(lease);
    _guest = {};
    _guest.flags = 0x10;
    _guest.descriptor = static_cast<std::int16_t>(state->descriptor);
    encodingError = false;
    return true;
}

void FileStream::SyncStatus() {
    const int saved = errno;
    auto* handle = GetHandle();
    _guest.readRemaining = 0;
    _guest.writeRemaining = 0;
    if (!handle) return;
    _guest.flags = static_cast<std::int16_t>((_guest.flags & ~0x60) |
        (std::feof(handle) ? 0x20 : 0) | (std::ferror(handle) || encodingError ? 0x40 : 0));
    errno = saved;
}

void FileStream::SetEncodingError() { encodingError = true; SyncStatus(); }

void FileStream::ClearError() {
    auto* handle = GetHandle();
    if (!handle) return;
    std::clearerr(handle);
    encodingError = false;
    SyncStatus();
}

int FileStream::Close() {
    const int saved = errno;
    GuestFiles::Identity expected;
    std::FILE* handle;
    {
        std::lock_guard lock(streamsMutex);
        if (!state || !state->handle) { errno = 9; return EOF; }
        expected = state->identity;
        handle = std::exchange(state->handle, nullptr);
        state->identity.reset();
    }
    const int logicalResult = GuestFiles::GuestFileCloseMatching_nid_no_patch(expected);
    const int logicalError = errno;
    const int nativeResult = std::fclose(handle);
    const int nativeError = errno;
    _guest = {};
    encodingError = false;
    if (logicalResult != 0) { errno = logicalError; return EOF; }
    if (nativeResult != 0) { errno = GuestFiles::GuestFileNativeError_nid_no_patch(nativeError); return EOF; }
    errno = saved;
    return 0;
}

void FileStream::Lock() {
    auto* handle = GetHandle();
    if (!handle) return;
#ifdef _WIN32
    ::_lock_file(handle);
#else
    ::flockfile(handle);
#endif
}

void FileStream::Unlock() {
    if (!state || !state->handle) { errno = 9; return; }
#ifdef _WIN32
    ::_unlock_file(state->handle);
#else
    ::funlockfile(state->handle);
#endif
}
