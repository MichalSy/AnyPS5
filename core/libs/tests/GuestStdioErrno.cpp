#include "prx/libc/include/FileStream.hpp"
#include "prx/libc/include/GuestFileDescriptors.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

extern "C" {
int APS5_VABI pipe_nid_postfix(int*);
std::int64_t APS5_VABI read_nid_postfix(int, void*, std::size_t);
std::int64_t APS5_VABI write_nid_postfix(int, const void*, std::size_t);
int APS5_VABI close_nid_postfix(int);
FileStream* APS5_VABI fdopen_nid_postfix(int, const char*);
int APS5_VABI fclose_nid_postfix(FileStream*);
int APS5_VABI fgetc_nid_postfix(FileStream*);
int APS5_VABI fputc_nid_postfix(int, FileStream*);
std::size_t APS5_VABI fread_nid_postfix(void*, std::size_t, std::size_t, FileStream*);
std::size_t APS5_VABI fwrite_nid_postfix(const void*, std::size_t, std::size_t, FileStream*);
int APS5_VABI ferror_nid_postfix(FileStream*);
int APS5_VABI feof_nid_postfix(FileStream*);
void APS5_VABI clearerr_nid_postfix(FileStream*);
int APS5_VABI setvbuf_nid_postfix(FileStream*, char*, int, std::size_t);
}

namespace {
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void Nonblocking(int descriptor) {
    const auto lease = GuestFiles::GuestFileAcquire_nid_no_patch(descriptor);
    Require(static_cast<bool>(lease), "logical pipe lookup failed");
    const int native = GuestFiles::GuestFileNativeDescriptor_nid_no_patch(lease);
    const int flags = ::fcntl(native, F_GETFL);
    Require(flags >= 0 && ::fcntl(native, F_SETFL, flags | O_NONBLOCK) == 0,
        "nonblocking native pipe configuration failed");
}

void ReadStickyError() {
    int descriptors[2];
    Require(pipe_nid_postfix(descriptors) == 0, "guest pipe creation failed");
    Nonblocking(descriptors[0]);
    auto* stream = fdopen_nid_postfix(descriptors[0], "rb");
    Require(stream != nullptr, "guest pipe fdopen failed");
    errno = 0;
    Require(fgetc_nid_postfix(stream) == EOF && errno == 35, "initial nonblocking read did not return guest EAGAIN");
    Require(ferror_nid_postfix(stream) != 0 && errno == 35, "ferror remapped guest errno");
    Require(feof_nid_postfix(stream) == 0 && errno == 35, "feof changed guest errno");
    const char expected = 'R';
    Require(write_nid_postfix(descriptors[1], &expected, 1) == 1, "pipe payload write failed");
    errno = 35;
    char actual = 0;
    Require(fread_nid_postfix(&actual, 1, 1, stream) == 1 && actual == expected,
        "successful read after sticky error lost payload");
    Require(errno == 35 && ferror_nid_postfix(stream) != 0 && errno == 35,
        "successful fread remapped sticky guest EAGAIN");
    Require(fgetc_nid_postfix(stream) == EOF && errno == 35, "repeated nonblocking read changed EAGAIN");
    clearerr_nid_postfix(stream);
    Require(errno == 35 && ferror_nid_postfix(stream) == 0 && errno == 35,
        "clearerr changed guest errno or retained the error flag");
    Require(fclose_nid_postfix(stream) == 0 && close_nid_postfix(descriptors[1]) == 0,
        "read pipe cleanup failed");
}

void WriteStickyError() {
    int descriptors[2];
    Require(pipe_nid_postfix(descriptors) == 0, "guest write pipe creation failed");
    Nonblocking(descriptors[1]);
    auto* stream = fdopen_nid_postfix(descriptors[1], "wb");
    Require(stream && setvbuf_nid_postfix(stream, nullptr, 2, 0) == 0, "unbuffered pipe fdopen failed");
    std::array<char, 4096> bytes{};
    std::size_t filled = 0;
    {
        const auto lease = GuestFiles::GuestFileAcquire_nid_no_patch(descriptors[1]);
        Require(static_cast<bool>(lease), "write pipe lease failed");
        const int native = GuestFiles::GuestFileNativeDescriptor_nid_no_patch(lease);
        while (true) {
            const auto count = ::write(native, bytes.data(), bytes.size());
            if (count < 0) { Require(errno == EAGAIN || errno == EWOULDBLOCK, "pipe fill failed unexpectedly"); break; }
            Require(count > 0, "pipe fill made no progress");
            filled += static_cast<std::size_t>(count);
        }
    }
    errno = 0;
    Require(fputc_nid_postfix('X', stream) == EOF && errno == 35, "full pipe write did not report guest EAGAIN");
    while (filled != 0) {
        const auto requested = filled < bytes.size() ? filled : bytes.size();
        const auto count = read_nid_postfix(descriptors[0], bytes.data(), requested);
        Require(count > 0, "pipe drain failed");
        filled -= static_cast<std::size_t>(count);
    }
    errno = 35;
    const char expected = 'W';
    Require(fwrite_nid_postfix(&expected, 1, 1, stream) == 1, "successful write after sticky error failed");
    Require(errno == 35 && ferror_nid_postfix(stream) != 0 && errno == 35,
        "successful fwrite remapped sticky guest EAGAIN");
    char actual = 0;
    Require(read_nid_postfix(descriptors[0], &actual, 1) == 1 && actual == expected,
        "successful write after sticky error lost payload");
    clearerr_nid_postfix(stream);
    Require(errno == 35 && ferror_nid_postfix(stream) == 0 && errno == 35,
        "write clearerr changed guest errno");
    Require(fclose_nid_postfix(stream) == 0 && close_nid_postfix(descriptors[0]) == 0,
        "write pipe cleanup failed");
}
}

int main() {
    try {
        ReadStickyError();
        WriteStickyError();
        std::cout << "PASS: native stdio failures map once and sticky guest errno survives successful IO\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
