#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <filesystem>
#include <limits>
#include <utility>
#include <cerrno>
#include <cstring>
#include <system_error>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "prx/libc/include/FileStream.hpp"
#include "prx/libc/include/ApplicationHeap.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libc/include/GuestArena.hpp"

struct GuestFileMode {
    std::string native;
    bool writes = false;
    bool exclusive = false;
};

static std::optional<GuestFileMode> ParseFileMode(const char* mode) {
    const char access = mode[0];
    if (access != 'r' && access != 'w' && access != 'a') return std::nullopt;
    bool update = false;
    bool exclusive = false;
    for (const char* flag = mode + 1; *flag != '\0' && std::strchr("b+xev", *flag) != nullptr; ++flag) {
        if (*flag == '+') update = true;
        if (*flag == 'x') exclusive = true;
    }
    if (exclusive && access == 'r' && !update) return std::nullopt;
    GuestFileMode result{std::string(1, access), access != 'r' || update, exclusive};
    if (update) result.native += '+';
#ifdef _WIN32
    result.native += 'b';
#endif
    if (exclusive) result.native += 'x';
    return result;
}

extern "C" {

[[noreturn]] void APS5_VABI _ZSt11_Xbad_allocv_nid_postfix();

FileStream* APS5_VABI fdopen_nid_postfix(int descriptor, const char* mode) {
    if (!mode) { errno = 22; return nullptr; }
    const char* supported[] = {"r", "w", "a", "rb", "wb", "ab", "r+", "w+", "a+",
        "rb+", "wb+", "ab+", "r+b", "w+b", "a+b"};
    bool valid = false;
    for (const auto* candidate : supported) if (std::strcmp(mode, candidate) == 0) valid = true;
    if (!valid) { errno = 22; return nullptr; }
    std::string nativeMode;
    try { nativeMode = ParseFileMode(mode)->native; }
    catch (const std::bad_alloc&) { errno = 12; return nullptr; }
    const auto lease = GuestFiles::GuestFileAcquire_nid_no_patch(descriptor);
    if (!lease) return nullptr;
    const int access = GuestFiles::GuestFileAccessMode_nid_no_patch(lease);
    const int requested = std::strchr(mode, '+') ? 2 : *mode == 'r' ? 0 : 1;
    if ((requested == 2 && access != 2) || (requested == 0 && access == 1) || (requested == 1 && access == 0)) {
        errno = 22; return nullptr;
    }
    const int duplicate = GuestFiles::GuestFileDuplicateNative_nid_no_patch(lease);
    if (duplicate < 0) return nullptr;
#ifdef _WIN32
    auto* native = ::_fdopen(duplicate, nativeMode.c_str());
#else
    auto* native = ::fdopen(duplicate, mode);
#endif
    if (!native) {
        const int error = GuestFiles::GuestFileNativeError_nid_no_patch(errno);
#ifdef _WIN32
        ::_close(duplicate);
#else
        ::close(duplicate);
#endif
        errno = error;
        return nullptr;
    }
    std::unique_ptr<std::FILE, decltype(&std::fclose)> owner(native, std::fclose);
    try {
        auto* stream = new FileStream(native, lease, true);
        owner.release();
        return stream;
    } catch (const std::bad_alloc&) { owner.reset(); errno = 12; return nullptr; }
      catch (const std::system_error& error) { owner.reset(); errno = error.code().value(); return nullptr; }
}

FileStream* APS5_VABI freopen_nid_postfix(const char* filename, const char* mode, FileStream* stream) {
    if (!stream || !mode) { errno = 22; return nullptr; }
    if (!filename) { errno = 45; return nullptr; } // Mode-only reopening is not supported.
    std::optional<GuestFileMode> parsed;
    try { parsed = ParseFileMode(mode); }
    catch (const std::bad_alloc&) { errno = 12; return nullptr; }
    if (!parsed) { errno = 22; return nullptr; }
    bool reopened = false;
    const auto failure = [&](int error) -> FileStream* {
        if (reopened) {
            const bool dynamic = stream->IsDynamic();
            stream->Close();
            if (dynamic) delete stream;
        }
        errno = error;
        return nullptr;
    };
    try {
        const auto path = *filename ? ResolvePath_nid_no_patch(filename).string() : std::string{};
        if (stream->Reopen(path.c_str(), parsed->native.c_str())) {
            reopened = true;
            if (!path.empty() && parsed->writes) RecordWrittenPath_nid_no_patch(path);
            return stream;
        }
        const int error = errno;
        if (stream->IsDynamic()) delete stream;
        errno = error;
        return nullptr;
    } catch (const std::bad_alloc&) { return failure(12); }
      catch (const std::filesystem::filesystem_error& error) { return failure(GuestFiles::GuestFileNativeError_nid_no_patch(error.code().value())); }
}

FileStream* APS5_VABI fopen_nid_postfix(const char* filename, const char* mode) {
    if (!filename || !mode) { errno = 22; return nullptr; }
    if (GuestFiles::GuestFileInitializeStandards_nid_no_patch() != 0) return nullptr;
    try {
        const auto path = ResolvePath_nid_no_patch(filename);
        const auto parsed = ParseFileMode(mode);
        if (!parsed) { errno = 22; return nullptr; }
        const auto& nativeMode = parsed->native;
        std::unique_ptr<std::FILE, decltype(&std::fclose)> handle(std::fopen(path.string().c_str(), nativeMode.c_str()), std::fclose);
        if (!handle) { errno = GuestFiles::GuestFileNativeError_nid_no_patch(errno); return nullptr; }
        const int access = nativeMode.find('+') != std::string::npos ? 2 : *mode == 'r' ? 0 : 1;
        auto stream = std::make_unique<FileStream>(handle.get(), access, true);
        handle.release();
        if (parsed->writes) RecordWrittenPath_nid_no_patch(path);
        return stream.release();
    } catch (const std::bad_alloc&) { errno = 12; return nullptr; }
      catch (const std::filesystem::filesystem_error& error) { errno = GuestFiles::GuestFileNativeError_nid_no_patch(error.code().value()); return nullptr; }
      catch (const std::system_error& error) { errno = error.code().value(); return nullptr; }
}

int APS5_VABI fopen_s_nid_postfix(FileStream** result, const char* filename, const char* mode) {
    constexpr int GuestEinval = 22;
    if (!result || !filename || !mode) return GuestEinval;
    *result = fopen_nid_postfix(filename, mode);
    return *result ? 0 : errno;
}

int APS5_VABI fclose_nid_postfix(FileStream* stream) {
    if (!stream) { errno = 22; return EOF; }
    std::unique_ptr<FileStream> owner(stream->IsDynamic() ? stream : nullptr);
    return stream->Close();
}

int APS5_VABI fseek_nid_postfix(FileStream* stream, std::int64_t offset, int origin);

FileStream* APS5_VABI _ZSt7_FiopenPKcNSt5_IosbIiE9_OpenmodeEi_nid_postfix(const char* filename, int mode, int protection) {
    static_cast<void>(protection);
    constexpr int In = 0x01, Out = 0x02, Ate = 0x04, App = 0x08, Trunc = 0x10, Nocreate = 0x20, Noreplace = 0x40,
        Binary = 0x80;
    constexpr std::pair<int, const char*> modes[] = {
        {In, "r"}, {Out, "w"}, {Out | Trunc, "w"}, {Out | App, "a"},
        {In | Binary, "rb"}, {Out | Binary, "wb"}, {Out | Trunc | Binary, "wb"}, {Out | App | Binary, "ab"},
        {In | Out, "r+"}, {In | Out | Trunc, "w+"}, {In | Out | App, "a+"},
        {In | Out | Binary, "r+b"}, {In | Out | Trunc | Binary, "w+b"}, {In | Out | App | Binary, "a+b"}};
    const int open = (mode & (In | Out | App | Trunc | Binary)) | ((mode & Nocreate) ? In : 0) | ((mode & App) ? Out : 0);
    const char* openMode = nullptr;
    for (const auto& [flags, text] : modes) if (flags == open) openMode = text;
    if (!openMode) return nullptr;
    if ((mode & Noreplace) && (open & Out)) {
        if (auto* existing = fopen_nid_postfix(filename, "r")) {
            fclose_nid_postfix(existing);
            return nullptr;
        }
    }
    auto* stream = fopen_nid_postfix(filename, openMode);
    if (!stream || !(mode & Ate) || fseek_nid_postfix(stream, 0, SEEK_END) == 0) return stream;
    fclose_nid_postfix(stream);
    return nullptr;
}

size_t APS5_VABI fread_nid_postfix(void* buffer, size_t size, size_t count, FileStream* stream) {
    const int savedError = errno;
    auto* handle = GetNativeStream(stream);
    if (!handle) return 0;
    if (size == 0 || count == 0) return 0;
    if (!buffer) throw std::runtime_error("fread: null buffer");
    if (count > std::numeric_limits<std::size_t>::max() / size) throw std::overflow_error("fread: buffer size overflow");
    const auto bytes = size * count;
    if (bytes > std::numeric_limits<std::uintptr_t>::max() - reinterpret_cast<std::uintptr_t>(buffer)) throw std::overflow_error("fread: buffer address overflow");
    const GuestArena::HostWrite destination(buffer, bytes);
    if (!destination.Open()) throw std::runtime_error("fread: buffer is not writable");
    errno = 0;
    const auto result = std::fread(buffer, size, count, handle);
    const int nativeError = errno;
    stream->SyncStatus();
    errno = nativeError != 0 && std::ferror(handle) ? GuestFiles::GuestFileNativeError_nid_no_patch(nativeError) : savedError;
    return result;
}

size_t APS5_VABI fwrite_nid_postfix(const void* buffer, size_t size, size_t count, FileStream* stream) {
    const int savedError = errno;
    auto* handle = GetNativeStream(stream);
    if (!handle) return 0;
    if (size == 0 || count == 0) return 0;
    if (!buffer) throw std::runtime_error("fwrite: null buffer");
    errno = 0;
    const auto result = std::fwrite(buffer, size, count, handle);
    const int nativeError = errno;
    stream->SyncStatus();
    errno = nativeError != 0 && std::ferror(handle) ? GuestFiles::GuestFileNativeError_nid_no_patch(nativeError) : savedError;
    return result;
}

int APS5_VABI fseeko_nid_postfix(FileStream* stream, std::int64_t offset, int origin) {
    if (origin != SEEK_SET && origin != SEEK_CUR && origin != SEEK_END) { errno = 22; return -1; }
    auto* handle = GetNativeStream(stream);
    if (!handle) return -1;
#ifdef _WIN32
    const int result = _fseeki64(handle, offset, origin);
#else
    static_assert(sizeof(off_t) == 8);
    const int result = ::fseeko(handle, offset, origin);
#endif
    const int nativeError = errno;
    stream->SyncStatus();
    if (result) errno = GuestFiles::GuestFileNativeError_nid_no_patch(nativeError);
    return result;
}

std::int64_t APS5_VABI ftello_nid_postfix(FileStream* stream) {
    auto* handle = GetNativeStream(stream);
    if (!handle) return -1;
#ifdef _WIN32
    const auto result = _ftelli64(handle);
#else
    static_assert(sizeof(off_t) == 8);
    const auto result = ::ftello(handle);
#endif
    if (result == -1) errno = GuestFiles::GuestFileNativeError_nid_no_patch(errno);
    return result;
}

int APS5_VABI fseek_nid_postfix(FileStream* stream, std::int64_t offset, int origin) {
    return fseeko_nid_postfix(stream, offset, origin);
}

std::int64_t APS5_VABI ftell_nid_postfix(FileStream* stream) { return ftello_nid_postfix(stream); }

int APS5_VABI fgetpos_nid_postfix(FileStream* stream, std::int64_t* position) {
    if (!position) throw std::invalid_argument("fgetpos: null position");
    const auto result = ftello_nid_postfix(stream);
    if (result == -1) return -1;
    *position = result;
    return 0;
}

int APS5_VABI fsetpos_nid_postfix(FileStream* stream, const std::int64_t* position) {
    if (!position) throw std::invalid_argument("fsetpos: null position");
    return fseeko_nid_postfix(stream, *position, SEEK_SET);
}

int APS5_VABI fputs_nid_postfix(const char* str, FileStream* stream) {
    if (!str) { errno = 22; return EOF; }
    auto* handle = GetNativeStream(stream);
    if (!handle) return EOF;
    const int result = std::fputs(str, handle);
    stream->SyncStatus();
    if (result == EOF) errno = GuestFiles::GuestFileNativeError_nid_no_patch(errno);
    return result;
}

int APS5_VABI fflush_nid_postfix(FileStream* stream) {
    auto* handle = stream ? GetNativeStream(stream) : nullptr;
    if (stream && !handle) return EOF;
    const int result = std::fflush(handle);
    if (stream) stream->SyncStatus();
    if (result != 0) errno = GuestFiles::GuestFileNativeError_nid_no_patch(errno);
    return result;
}

int APS5_VABI malloc_stats_fast_nid_postfix(void* stats) {
    return ApplicationHeapStatsFast_nid_no_patch(stats);
}

void* APS5_VABI malloc_nid_postfix(size_t size) {
    return ApplicationHeapAllocate_nid_no_patch(size);
}

void APS5_VABI free_nid_postfix(void* ptr) {
    ApplicationHeapFree_nid_no_patch(ptr);
}

void* APS5_VABI realloc_nid_postfix(void* ptr, size_t newSize) {
    return ApplicationHeapReallocate_nid_no_patch(ptr, newSize);
}

void* APS5_VABI memalign_nid_postfix(size_t alignment, size_t size) {
    return ApplicationHeapAlign_nid_no_patch(alignment, size);
}

void* APS5_VABI aligned_alloc_nid_postfix(size_t alignment, size_t size) {
    return ApplicationHeapAlign_nid_no_patch(alignment, size);
}

void* APS5_VABI reallocalign_nid_postfix(void* ptr, size_t size, size_t alignment) {
    return ApplicationHeapRealign_nid_no_patch(ptr, size, alignment);
}

void* APS5_VABI calloc_nid_postfix(size_t count, size_t size) {
    return ApplicationHeapCalloc_nid_no_patch(count, size);
}

int APS5_VABI posix_memalign_nid_postfix(void** pointer, size_t alignment, size_t size) {
    if (!pointer || alignment < sizeof(void*) || (alignment & (alignment - 1)) != 0) return 22;
    const int savedError = errno;
    const int result = ApplicationHeapPosixAlign_nid_no_patch(pointer, alignment, size);
    errno = savedError;
    return result;
}

void* APS5_VABI bsearch_nid_postfix(const void* key, const void* base, size_t count,
    size_t size, int (APS5_VABI *compare)(const void*, const void*)) {
    if (count == 0) return nullptr;
    if (!key || !base || !compare || size == 0)
        throw std::invalid_argument("bsearch: invalid arguments");
    if (count > std::numeric_limits<size_t>::max() / size)
        throw std::overflow_error("bsearch: array size overflow");
    const auto* bytes = static_cast<const unsigned char*>(base);
    size_t first = 0;
    while (count != 0) {
        const size_t half = count / 2;
        const size_t middle = first + half;
        const auto* element = bytes + middle * size;
        const int result = compare(key, element);
        if (result == 0) return const_cast<unsigned char*>(element);
        if (result < 0) count = half;
        else { first = middle + 1; count -= half + 1; }
    }
    return nullptr;
}

void APS5_VABI qsort_nid_postfix(void* base, size_t count, size_t size, int (APS5_VABI *compare)(const void*, const void*)) {
    if (!compare) throw std::invalid_argument("qsort: null comparator");
    if (size == 0) throw std::invalid_argument("qsort: zero element size");
    if (count == 0) return;
    if (!base) throw std::invalid_argument("qsort: null base");
    if (count > std::numeric_limits<size_t>::max() / size) throw std::overflow_error("qsort: array size overflow");
    if (count == 1) return;

    auto* bytes = static_cast<unsigned char*>(base);
    const auto swapElements = [bytes, size](size_t left, size_t right) {
        auto* leftElement = bytes + left * size;
        auto* rightElement = bytes + right * size;
        for (size_t index = 0; index < size; ++index) std::swap(leftElement[index], rightElement[index]);
    };
    const auto siftDown = [bytes, size, compare, &swapElements](size_t root, size_t heapSize) {
        while (root < heapSize / 2) {
            size_t child = root * 2 + 1;
            if (child + 1 < heapSize && compare(bytes + child * size, bytes + (child + 1) * size) < 0) ++child;
            if (compare(bytes + root * size, bytes + child * size) >= 0) return;
            swapElements(root, child);
            root = child;
        }
    };

    for (size_t parent = count / 2; parent != 0; --parent) siftDown(parent - 1, count);
    for (size_t heapSize = count; heapSize > 1;) {
        swapElements(0, --heapSize);
        siftDown(0, heapSize);
    }
}

}
