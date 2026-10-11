#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libkernel/File/include/FileFlags.hpp"
#include "prx/libkernel/KernelErrors.hpp"
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#ifdef _WIN32
extern "C" _invalid_parameter_handler _set_thread_local_invalid_parameter_handler(_invalid_parameter_handler);
static int invalidParameterCalls;
static void ProbeInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, std::uintptr_t) {
    ++invalidParameterCalls;
}
#endif

extern "C" {
int APS5_VABI sceKernelOpen(const char*, int, std::uint16_t);
int APS5_VABI sceKernelClose(int);
std::int64_t APS5_VABI sceKernelRead(int, void*, std::size_t);
std::int64_t APS5_VABI sceKernelWrite(int, const void*, std::size_t);
std::int64_t APS5_VABI sceKernelLseek(int, std::int64_t, int);
std::int64_t APS5_VABI lseek_nid_postfix(int, std::int64_t, int);
int APS5_VABI pipe_nid_postfix(int*);
int* APS5_VABI __error_nid_postfix();
}

static void Check(bool value, int line) {
    if (!value) {
        std::fprintf(stderr, "File seek check failed at line %d (guest errno %d)\n", line, *__error_nid_postfix());
        std::abort();
    }
}
#define Require(value) Check((value), __LINE__)

static void CheckLargeOffsets(int file, const std::filesystem::path& path) {
    constexpr std::int64_t offsets[] = {(std::int64_t{1} << 32) + 123, 0x180020009LL};
    for (const auto offset : offsets) {
        *__error_nid_postfix() = 13;
        Require(sceKernelLseek(file, offset, 0) == offset);
        Require(*__error_nid_postfix() == 13);
        Require(lseek_nid_postfix(file, 0, 1) == offset);
        Require(*__error_nid_postfix() == 13);
        Require(lseek_nid_postfix(file, -23, 1) == offset - 23);
        Require(*__error_nid_postfix() == 13);
        Require(sceKernelLseek(file, 23, 1) == offset);
        Require(*__error_nid_postfix() == 13);
        Require(lseek_nid_postfix(file, offset, 0) == offset);
        Require(*__error_nid_postfix() == 13);
        Require(std::filesystem::file_size(path) == 10);
    }

#ifndef _WIN32
    constexpr auto offset = offsets[0];
    const char marker = 'K';
    Require(sceKernelLseek(file, offset, 0) == offset);
    Require(sceKernelWrite(file, &marker, 1) == 1);
    Require(std::filesystem::file_size(path) == static_cast<std::uint64_t>(offset + 1));
    Require(lseek_nid_postfix(file, -2, 2) == offset - 1);
    std::array<char, 2> tail = {'x', 'x'};
    Require(sceKernelRead(file, tail.data(), tail.size()) == 2);
    Require(tail[0] == '\0' && tail[1] == marker);
    Require(sceKernelLseek(file, -1, 2) == offset);
    char markerRead = '\0';
    Require(sceKernelRead(file, &markerRead, 1) == 1 && markerRead == marker);
#else
    Require(lseek_nid_postfix(file, -2, 2) == 8);
    std::array<char, 2> tail{};
    Require(sceKernelRead(file, tail.data(), tail.size()) == 2);
    Require(tail[0] == '8' && tail[1] == '9');
#endif

    Require(sceKernelLseek(file, 0, 0) == 0);
    std::array<char, 10> original{};
    Require(sceKernelRead(file, original.data(), original.size()) == 10);
    Require(std::memcmp(original.data(), "0123456789", original.size()) == 0);
    Require(lseek_nid_postfix(file, 3, 0) == 3);
    const char replacement = 'Q';
    Require(sceKernelWrite(file, &replacement, 1) == 1);
    Require(sceKernelLseek(file, -1, 1) == 3);
    char replacementRead = '\0';
    Require(sceKernelRead(file, &replacementRead, 1) == 1 && replacementRead == replacement);
}

static void CheckErrors(int file, const std::filesystem::path& path) {
    Require(sceKernelLseek(file, 5, 0) == 5);
    Require(sceKernelLseek(file, 0, -1) == SCE_KERNEL_ERROR_EINVAL);
    Require(sceKernelLseek(file, 0, 3) == SCE_KERNEL_ERROR_EINVAL);
    Require(sceKernelLseek(file, 0, 1) == 5);
    *__error_nid_postfix() = 13;
    Require(lseek_nid_postfix(file, 0, -1) == -1);
    Require(*__error_nid_postfix() == 22);
    Require(lseek_nid_postfix(file, 0, 3) == -1);
    Require(*__error_nid_postfix() == 22);
    Require(sceKernelLseek(file, 0, 1) == 5);
#ifndef _WIN32
    Require(sceKernelLseek(file, -1, 0) == SCE_KERNEL_ERROR_EINVAL);
    Require(sceKernelLseek(file, 0, 1) == 5);
    Require(lseek_nid_postfix(file, -1, 0) == -1);
    Require(*__error_nid_postfix() == 22);
    Require(sceKernelLseek(file, 0, 1) == 5);
#endif

#ifdef _WIN32
    const auto previous = _set_thread_local_invalid_parameter_handler(ProbeInvalidParameter);
#endif
    Require(sceKernelLseek(-1, 0, 0) == SCE_KERNEL_ERROR_EBADF);
    Require(static_cast<std::uint64_t>(sceKernelLseek(-1, 0, 0)) == 0xffffffff80020009ULL);
    *__error_nid_postfix() = 13;
    Require(lseek_nid_postfix(-1, 0, 0) == -1);
    Require(*__error_nid_postfix() == 9);
#ifdef _WIN32
    Require(_set_thread_local_invalid_parameter_handler(previous) == ProbeInvalidParameter);
    Require(invalidParameterCalls == 0);
#endif

    const int closed = sceKernelOpen(path.string().c_str(), SCE_KERNEL_O_RDONLY, 0);
    Require(closed >= 0 && sceKernelClose(closed) == 0);
    Require(sceKernelLseek(closed, 0, 0) == SCE_KERNEL_ERROR_EBADF);
    Require(lseek_nid_postfix(closed, 0, 0) == -1);
    Require(*__error_nid_postfix() == 9);
    Require(sceKernelLseek(file, 0, 1) == 5);
}

static void CheckThreadError(int file) {
    auto* mainError = __error_nid_postfix();
    *mainError = 13;
    std::thread worker([file, mainError] {
        auto* threadError = __error_nid_postfix();
        Require(threadError != mainError);
        *threadError = 34;
        Require(lseek_nid_postfix(-1, 0, 0) == -1 && *threadError == 9);
        *threadError = 22;
        Require(lseek_nid_postfix(file, 0, 1) == 5 && *threadError == 22);
    });
    worker.join();
    Require(*mainError == 13);
}

#ifndef _WIN32
static void CheckPipe() {
    int ends[2] = {-1, -1};
    Require(pipe_nid_postfix(ends) == 0);
    const char payload[] = "pipe";
    Require(sceKernelWrite(ends[1], payload, 4) == 4);
    for (const int end : ends) {
        Require(sceKernelLseek(end, 0, 0) == SCE_KERNEL_ERROR_ESPIPE);
        Require(lseek_nid_postfix(end, 0, 0) == -1);
        Require(*__error_nid_postfix() == 29);
    }
    std::array<char, 4> received{};
    Require(sceKernelRead(ends[0], received.data(), received.size()) == 4);
    Require(std::memcmp(received.data(), payload, received.size()) == 0);
    Require(sceKernelClose(ends[0]) == 0 && sceKernelClose(ends[1]) == 0);
}
#endif

int main() {
    const auto root = std::filesystem::path("anyps5-file-seek-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Require(std::filesystem::create_directory(root));
    const auto path = root / "data.bin";
    const int file = sceKernelOpen(path.string().c_str(), SCE_KERNEL_O_RDWR | SCE_KERNEL_O_CREAT | SCE_KERNEL_O_EXCL, 0600);
    Require(file >= 0);
    Require(sceKernelWrite(file, "0123456789", 10) == 10);
    CheckLargeOffsets(file, path);
    CheckErrors(file, path);
    CheckThreadError(file);
#ifndef _WIN32
    CheckPipe();
#endif
    Require(sceKernelClose(file) == 0);
    Require(std::filesystem::remove(path));
    Require(std::filesystem::remove(root));
}
