#include "prx/libc/include/FileStream.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <cerrno>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

extern "C" {
FileStream* __stdinp_nid_postfix = &_Stdin_nid_postfix;
FileStream* __stdoutp_nid_postfix = &_Stdout_nid_postfix;
FileStream* __stderrp_nid_postfix = &_Stderr_nid_postfix;
int __isthreaded_nid_postfix = 1;

int APS5_VABI fgetc_nid_postfix(FileStream* stream) {
    auto* handle = GetNativeStream(stream);
    if (!handle) return EOF;
    const int savedError = errno;
    errno = 0;
    const int result = std::fgetc(handle);
    const int nativeError = errno;
    stream->SyncStatus();
    errno = nativeError != 0 && std::ferror(handle) ? GuestFiles::GuestFileNativeError_nid_no_patch(nativeError) : savedError;
    return result;
}
std::int32_t APS5_VABI fgetwc_nid_postfix(FileStream* stream) {
    return fgetc_nid_postfix(stream);
}

std::int32_t APS5_VABI ungetwc_nid_postfix(std::int32_t value, FileStream* stream) {
    if (value == -1) return -1;
    if (value < 0 || value > 255) {
        if (!stream) { errno = 22; return -1; }
        errno = 86;
        stream->SetEncodingError();
        return -1;
    }
    auto* handle = GetNativeStream(stream);
    if (!handle) return EOF;
    const int savedError = errno;
    errno = 0;
    const int result = std::ungetc(value, handle);
    const int nativeError = errno;
    stream->SyncStatus();
    errno = nativeError != 0 && std::ferror(handle) ? GuestFiles::GuestFileNativeError_nid_no_patch(nativeError) : savedError;
    return result;
}
int APS5_VABI getc_nid_postfix(FileStream* stream) { return fgetc_nid_postfix(stream); }
int APS5_VABI __srget_nid_postfix(FileStream* stream) { return fgetc_nid_postfix(stream); }
int APS5_VABI getchar_nid_postfix() { return fgetc_nid_postfix(__stdinp_nid_postfix); }
int APS5_VABI fputc_nid_postfix(int value, FileStream* stream) {
    auto* handle = GetNativeStream(stream);
    if (!handle) return EOF;
    const int savedError = errno;
    errno = 0;
    const int result = std::fputc(value, handle);
    const int nativeError = errno;
    stream->SyncStatus();
    errno = nativeError != 0 && std::ferror(handle) ? GuestFiles::GuestFileNativeError_nid_no_patch(nativeError) : savedError;
    return result;
}
int APS5_VABI putc_nid_postfix(int value, FileStream* stream) { return fputc_nid_postfix(value, stream); }
int APS5_VABI __swbuf_nid_postfix(int value, FileStream* stream) { return fputc_nid_postfix(value, stream); }
int APS5_VABI putchar_nid_postfix(int value) { return fputc_nid_postfix(value, __stdoutp_nid_postfix); }
int APS5_VABI ungetc_nid_postfix(int value, FileStream* stream) {
    auto* handle = GetNativeStream(stream);
    if (!handle) return EOF;
    const int savedError = errno;
    errno = 0;
    const int result = std::ungetc(value, handle);
    const int nativeError = errno;
    stream->SyncStatus();
    errno = nativeError != 0 && std::ferror(handle) ? GuestFiles::GuestFileNativeError_nid_no_patch(nativeError) : savedError;
    return result;
}
char* APS5_VABI fgets_nid_postfix(char* buffer, int size, FileStream* stream) {
    auto* handle = GetNativeStream(stream);
    if (!handle) return nullptr;
    const int savedError = errno;
    errno = 0;
    auto* result = std::fgets(buffer, size, handle);
    const int nativeError = errno;
    stream->SyncStatus();
    errno = nativeError != 0 && std::ferror(handle) ? GuestFiles::GuestFileNativeError_nid_no_patch(nativeError) : savedError;
    return result;
}
int APS5_VABI feof_nid_postfix(FileStream* stream) {
    if (!stream) { errno = 22; return 0; }
    stream->SyncStatus();
    return (stream->GuestState().flags & 0x20) != 0;
}
int APS5_VABI ferror_nid_postfix(FileStream* stream) {
    if (!stream) { errno = 22; return 0; }
    stream->SyncStatus();
    return (stream->GuestState().flags & 0x40) != 0;
}
void APS5_VABI clearerr_nid_postfix(FileStream* stream) {
    if (!stream) { errno = 22; return; }
    stream->ClearError();
}
int APS5_VABI fileno_nid_postfix(FileStream* stream) {
    if (!stream) { errno = 22; return -1; }
    return stream->Descriptor();
}
int APS5_VABI setvbuf_nid_postfix(FileStream* stream, char* buffer, int mode, std::size_t size) {
    if (mode < 0 || mode > 2) { errno = 22; return -1; }
    auto* handle = GetNativeStream(stream);
    if (!handle) return -1;
    const int native = mode == 0 ? _IOFBF : mode == 1 ? _IOLBF : _IONBF;
    if (native != _IONBF && size == 0) {
        buffer = nullptr;
        size = BUFSIZ;
    }
    return std::setvbuf(handle, buffer, native, size);
}
void APS5_VABI setbuf_nid_postfix(FileStream* stream, char* buffer) {
    setvbuf_nid_postfix(stream, buffer, buffer ? 0 : 2, buffer ? 1024 : 0);
}
}
