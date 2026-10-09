#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestFileDescriptors.hpp"
#include "prx/libkernel/File/include/FileFlags.hpp"
#include "prx/libkernel/Socket/include/SocketRuntime.hpp"
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <new>
#include <random>
#include <system_error>
#ifdef _WIN32
#include <cstdlib>
#include <io.h>
extern "C" _invalid_parameter_handler _set_thread_local_invalid_parameter_handler(_invalid_parameter_handler);
#else
#include <unistd.h>
#endif

extern "C" int* APS5_VABI __error_nid_postfix();
extern "C" int APS5_VABI open_nid_postfix(const char* path, int flags, int mode);

namespace {
int Failure(int error) {
    *__error_nid_postfix() = error;
    return -1;
}
#ifdef _WIN32
void IgnoreInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, std::uintptr_t) {}
#endif
}

extern "C" {

int APS5_VABI isatty_nid_postfix(int descriptor) {
    constexpr int GuestEbadf = 9, GuestEnotty = 25;
    if (descriptor < 0) { Failure(GuestEbadf); return 0; }
    if (descriptor >= GuestSockets::FirstDescriptor) {
        Failure(GuestSockets::GuestSocketIsOpen_nid_no_patch(descriptor) ? GuestEnotty : GuestEbadf);
        return 0;
    }
    const int savedError = *__error_nid_postfix();
    const auto lease = GuestFiles::GuestFileAcquire_nid_no_patch(descriptor);
    if (!lease) { Failure(errno); return 0; }
    const int nativeDescriptor = GuestFiles::GuestFileNativeDescriptor_nid_no_patch(lease);
    errno = 0;
#ifdef _WIN32
    const auto previous = _set_thread_local_invalid_parameter_handler(IgnoreInvalidParameter);
    const int result = ::_isatty(nativeDescriptor);
    _set_thread_local_invalid_parameter_handler(previous);
#else
    const int result = ::isatty(nativeDescriptor);
#endif
    *__error_nid_postfix() = result ? savedError : errno == EBADF ? GuestEbadf : GuestEnotty;
    return result ? 1 : 0;
}

int APS5_VABI mkstemp_nid_postfix(char* pattern) {
    constexpr int GuestEexist = 17;
    if (!pattern) return Failure(14);
    const auto length = std::strlen(pattern);
    std::size_t suffix = length;
    while (suffix != 0 && pattern[suffix - 1] == 'X') --suffix;
    if (length - suffix < 6) return Failure(22);
    const GuestArena::HostWrite destination(pattern, length + 1);
    if (!destination.Open()) return Failure(14);
    try {
        constexpr char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        std::random_device random;
        std::uniform_int_distribution<unsigned> character(0, sizeof(alphabet) - 2);
        for (unsigned attempt = 0; attempt < 256; ++attempt) {
            for (auto index = suffix; index < length; ++index) pattern[index] = alphabet[character(random)];
            const int descriptor = open_nid_postfix(pattern, SCE_KERNEL_O_RDWR | SCE_KERNEL_O_CREAT | SCE_KERNEL_O_EXCL, 0600);
            if (descriptor >= 0 || *__error_nid_postfix() != GuestEexist) return descriptor;
        }
        return Failure(GuestEexist);
    } catch (const std::bad_alloc&) { return Failure(12); }
      catch (const std::filesystem::filesystem_error& error) {
        return Failure(error.code() == std::errc::no_such_file_or_directory ? 2 : 5);
    } catch (const std::system_error&) { return Failure(5); }
}

char* APS5_VABI strcasestr_nid_postfix(const char* text, const char* needle) {
    if (*needle == '\0') return const_cast<char*>(text);
    for (; *text != '\0'; ++text) {
        const char* candidate = text;
        const char* match = needle;
        while (*candidate != '\0' && *match != '\0' &&
               std::tolower(static_cast<unsigned char>(*candidate)) ==
               std::tolower(static_cast<unsigned char>(*match))) {
            ++candidate;
            ++match;
        }
        if (*match == '\0') return const_cast<char*>(text);
    }
    return nullptr;
}

}
