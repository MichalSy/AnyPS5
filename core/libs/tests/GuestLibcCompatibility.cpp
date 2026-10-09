#include "prx/libc/include/ApplicationHeap.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/Socket/include/SocketRuntime.hpp"
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <source_location>
#include <string>
#ifdef _WIN32
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

extern "C" {
const char* APS5_VABI getprogname_nid_postfix();
extern const char* __progname_nid_postfix;
std::size_t APS5_VABI malloc_usable_size_nid_postfix(const void*);
void APS5_VABI syslog_nid_postfix(int, const char*, ...);
int APS5_VABI isatty_nid_postfix(int);
int APS5_VABI mkstemp_nid_postfix(char*);
int APS5_VABI close_nid_postfix(int);
std::int64_t APS5_VABI read_nid_postfix(int, void*, std::size_t);
std::int64_t APS5_VABI write_nid_postfix(int, const void*, std::size_t);
std::int64_t APS5_VABI lseek_nid_postfix(int, std::int64_t, int);
int APS5_VABI socket_nid_postfix(int, int, int);
int* APS5_VABI __error_nid_postfix();
}

namespace {
void Require(bool condition, std::source_location location = std::source_location::current()) {
    if (!condition) {
        std::fprintf(stderr, "Guest libc compatibility check failed at line %u\n", location.line());
        std::abort();
    }
}

int NativeDuplicate(int descriptor) {
#ifdef _WIN32
    return ::_dup(descriptor);
#else
    return ::dup(descriptor);
#endif
}

bool NativeRedirect(int from, int to) {
#ifdef _WIN32
    return ::_dup2(from, to) == 0;
#else
    return ::dup2(from, to) == to;
#endif
}

int NativeClose(int descriptor) {
#ifdef _WIN32
    return ::_close(descriptor);
#else
    return ::close(descriptor);
#endif
}

int NativeDescriptor(std::FILE* stream) {
#ifdef _WIN32
    return ::_fileno(stream);
#else
    return ::fileno(stream);
#endif
}

void CheckLog() {
    auto* capture = std::tmpfile();
    Require(capture != nullptr);
    const int saved = NativeDuplicate(NativeDescriptor(stderr));
    Require(saved >= 0);
    std::fflush(stderr);
    Require(NativeRedirect(NativeDescriptor(capture), NativeDescriptor(stderr)));
    *__error_nid_postfix() = 45;
    syslog_nid_postfix(6, "test %s %d %.2f %ld %d %d %d %d %%m %m", "message", 42, 1.25,
        std::int64_t{123456789}, 2, 3, 4, 5);
    const int afterLog = *__error_nid_postfix();
    syslog_nid_postfix(6, nullptr);
    const int afterNull = *__error_nid_postfix();
    std::fflush(stderr);
    Require(NativeRedirect(saved, NativeDescriptor(stderr)));
    Require(NativeClose(saved) == 0);
    Require(afterLog == 45 && afterNull == 45);
    std::rewind(capture);
    std::array<char, 1024> text{};
    const auto count = std::fread(text.data(), 1, text.size() - 1, capture);
    text[count] = '\0';
    Require(std::strstr(text.data(), "message 42 1.25 123456789 2 3 4 5 %m Operation not supported\n") != nullptr);
    Require(std::fclose(capture) == 0);
}
}

int main() {
    Require(getprogname_nid_postfix() == __progname_nid_postfix);
    Require(std::strcmp(getprogname_nid_postfix(), "eboot.bin") == 0);
    std::array<void*, 10> allocator{};
    ApplicationHeapRegister_nid_no_patch(allocator.data());
    auto* pointer = ApplicationHeapAlign_nid_no_patch(4096, 73);
    Require(malloc_usable_size_nid_postfix(pointer) == 73);
    Require(malloc_usable_size_nid_postfix(nullptr) == 0);
    ApplicationHeapFree_nid_no_patch(pointer);
    CheckLog();

    Require(isatty_nid_postfix(-1) == 0 && *__error_nid_postfix() == 9);
    Require(isatty_nid_postfix(GuestSockets::FirstDescriptor) == 0 && *__error_nid_postfix() == 9);
    const int socket = socket_nid_postfix(2, 1, 0);
    Require(socket >= GuestSockets::FirstDescriptor);
    Require(isatty_nid_postfix(socket) == 0 && *__error_nid_postfix() == 25);
    Require(close_nid_postfix(socket) == 0);

    Require(mkstemp_nid_postfix(nullptr) == -1 && *__error_nid_postfix() == 14);
    char invalid[] = "invalid.XXXXX";
    Require(mkstemp_nid_postfix(invalid) == -1 && *__error_nid_postfix() == 22);
    Require(std::strcmp(invalid, "invalid.XXXXX") == 0);
    const auto root = std::filesystem::temp_directory_path() / ("anyps5-mkstemp-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Require(std::filesystem::create_directory(root));
    AddPathAlias_nid_no_patch("/compat-temp", root.string().c_str());
    char first[] = "/compat-temp/file.XXXXXX";
    char second[] = "/compat-temp/file.XXXXXX";
    const int file = mkstemp_nid_postfix(first);
    Require(file >= 3 && file <= 32767);
    Require(std::strcmp(first, "/compat-temp/file.XXXXXX") != 0);
    Require(std::filesystem::exists(ResolvePath_nid_no_patch(first)));
    Require(isatty_nid_postfix(file) == 0 && *__error_nid_postfix() == 25);
#ifndef _WIN32
    struct stat status{};
    Require(::stat(ResolvePath_nid_no_patch(first).c_str(), &status) == 0 && (status.st_mode & 0777) == 0600);
#endif
    Require(write_nid_postfix(file, "x", 1) == 1 && lseek_nid_postfix(file, 0, SEEK_SET) == 0);
    char byte = '\0';
    Require(read_nid_postfix(file, &byte, 1) == 1 && byte == 'x');
    Require(lseek_nid_postfix(file, 0, SEEK_CUR) == 1);
    auto* persisted = std::fopen(ResolvePath_nid_no_patch(first).string().c_str(), "rb");
    Require(persisted != nullptr && std::fgetc(persisted) == 'x' && std::fgetc(persisted) == EOF);
    Require(std::fclose(persisted) == 0);
    const int another = mkstemp_nid_postfix(second);
    Require(another >= 3 && another <= 32767 && another != file && std::strcmp(first, second) != 0);
    Require(close_nid_postfix(file) == 0 && close_nid_postfix(another) == 0);
    RemovePathAlias_nid_no_patch("/compat-temp");
    std::filesystem::remove_all(root);
    char missing[] = "/anyps5-missing-directory/file.XXXXXX";
    Require(mkstemp_nid_postfix(missing) == -1 && *__error_nid_postfix() == 2);
}
