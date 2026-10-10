#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_FILESTREAM_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_FILESTREAM_HPP

#include "GuestFileDescriptors.hpp"
#include <cstdio>
#include <cerrno>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <cstdint>
#include <cstddef>
#include <mutex>
#include <type_traits>
#ifdef _WIN32
#include <io.h>
#endif

struct GuestFilePrefix {
    unsigned char* position = nullptr;
    std::int32_t readRemaining = 0;
    std::int32_t writeRemaining = 0;
    std::int16_t flags = 0;
    std::int16_t descriptor = -1;
    unsigned char* buffer = nullptr;
    std::int32_t bufferSize = 0;
    std::int32_t bufferPadding = 0;
    std::int32_t lineBufferSize = 0;
};
static_assert(offsetof(GuestFilePrefix, flags) == 16);
static_assert(offsetof(GuestFilePrefix, descriptor) == 18);
static_assert(offsetof(GuestFilePrefix, buffer) == 24);
static_assert(offsetof(GuestFilePrefix, lineBufferSize) == 40);

static constexpr const char* FOPEN_EXT_VERT = ".vert";
static constexpr const char* FOPEN_MSG_NULL_ARG = "null argument";
static constexpr const char* FOPEN_MSG_NOT_FOUND = "file not found";
static constexpr const char* FOPEN_MSG_OPEN_FAILED = "open failed";

struct FileStreamState;

class FileStream {
    GuestFilePrefix _guest{};
    std::byte _reserved[256 - sizeof(GuestFilePrefix)]{};
    FileStreamState* state = nullptr;
    bool dynamic = false;
    bool encodingError = false;

public:
    explicit FileStream(std::FILE* handle, bool dynamic = false);
    FileStream(std::FILE* handle, int accessMode, bool dynamic);
    FileStream(std::FILE* handle, const GuestFiles::Lease& lease, bool dynamic);
    ~FileStream();

    FileStream(const FileStream&) = delete;
    FileStream& operator=(const FileStream&) = delete;

    std::FILE* GetHandle();
    int Descriptor();
    bool IsDynamic() const { return dynamic; }
    GuestFilePrefix& GuestState() { return _guest; }
    bool Reopen(const std::filesystem::path& filename, const char* mode);
    void SyncStatus();
    void SetEncodingError();
    void ClearError();
    int Close();
    void Lock();
    void Unlock();
};
static_assert(std::is_standard_layout_v<FileStream>);

inline std::FILE* GetNativeStream(FileStream* stream) {
    if (!stream) { errno = 22; return nullptr; }
    return stream->GetHandle();
}

extern "C" {
extern FileStream _Stdin_nid_postfix;
extern FileStream _Stdout_nid_postfix;
extern FileStream _Stderr_nid_postfix;
std::mutex& GuestFileStreamMutex_nid_no_patch();
// Caller holds the stream mutex while replacing the matching registry entry.
int GuestFileStreamCheckRedirect_nid_no_patch(const GuestFiles::Lease& previous);
void GuestFileStreamRedirect_nid_no_patch(const GuestFiles::Lease& previous, const GuestFiles::Lease& replacement);
}

#endif
