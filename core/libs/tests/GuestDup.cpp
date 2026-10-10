#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libc/include/GuestFileDescriptors.hpp"
#include "prx/libc/include/FileStream.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

extern "C" {
int APS5_VABI dup_nid_postfix(int);
int APS5_VABI dup2_nid_postfix(int, int);
int APS5_VABI pipe_nid_postfix(int*);
int APS5_VABI close_nid_postfix(int);
std::int64_t APS5_VABI read_nid_postfix(int, void*, std::uint64_t);
std::int64_t APS5_VABI write_nid_postfix(int, const char*, std::int64_t);
int APS5_VABI socketpair_nid_postfix(int, int, int, int*);
std::int64_t APS5_VABI send_nid_postfix(int, const void*, std::uint64_t, int);
std::int64_t APS5_VABI recv_nid_postfix(int, void*, std::uint64_t, int);
int APS5_VABI open_nid_postfix(const char*, int, int);
FileStream* APS5_VABI fdopen_nid_postfix(int, const char*);
int APS5_VABI fclose_nid_postfix(FileStream*);
int APS5_VABI fileno_nid_postfix(FileStream*);
int APS5_VABI fflush_nid_postfix(FileStream*);
int APS5_VABI fputc_nid_postfix(int, FileStream*);
std::size_t APS5_VABI fwrite_nid_postfix(const void*, std::size_t, std::size_t, FileStream*);
int* APS5_VABI __error_nid_postfix();
}

static void Check(bool value, int line) {
    if (!value) {
        std::fprintf(stderr, "dup check failed at line %d (guest errno %d)\n", line, *__error_nid_postfix());
        std::abort();
    }
}
#define Require(value) Check((value), __LINE__)

constexpr int GuestEbadf = 9;
constexpr int Unix = 1;
constexpr int Stream = 1;
constexpr int FirstSocket = 0x10000000;

static void RequireFailure(int result, int error) {
    Require(result == -1);
    Require(*__error_nid_postfix() == error);
}

static void RequirePipeCarries(int writer, int reader, const char* text) {
    const auto length = static_cast<std::int64_t>(std::strlen(text));
    Require(write_nid_postfix(writer, text, length) == length);
    char buffer[16] = {};
    Require(read_nid_postfix(reader, buffer, sizeof(buffer)) == length);
    Require(std::memcmp(buffer, text, static_cast<std::size_t>(length)) == 0);
}

static void RequireSocketCarries(int writer, int reader, const char* text) {
    const auto length = std::strlen(text);
    Require(send_nid_postfix(writer, text, length, 0) == static_cast<std::int64_t>(length));
    char buffer[16] = {};
    Require(recv_nid_postfix(reader, buffer, sizeof(buffer), 0) == static_cast<std::int64_t>(length));
    Require(std::memcmp(buffer, text, length) == 0);
}

template <typename TCall> static bool Throws(TCall call) {
    try {
        call();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

static void DupOutlivesOriginal() {
    int pipe[2];
    Require(pipe_nid_postfix(pipe) == 0);
    const int duplicate = dup_nid_postfix(pipe[1]);
    Require(duplicate >= 0 && duplicate < FirstSocket && duplicate != pipe[1]);
    Require(close_nid_postfix(pipe[1]) == 0);
    RequirePipeCarries(duplicate, pipe[0], "dup");
    Require(close_nid_postfix(duplicate) == 0);
    char end = 0;
    Require(read_nid_postfix(pipe[0], &end, 1) == 0);
    Require(close_nid_postfix(pipe[0]) == 0);
}

static void Dup2ReplacesTarget() {
    int first[2];
    int second[2];
    Require(pipe_nid_postfix(first) == 0);
    Require(pipe_nid_postfix(second) == 0);
    Require(dup2_nid_postfix(first[1], second[1]) == second[1]);
    RequirePipeCarries(second[1], first[0], "dup2");
    char end = 0;
    Require(read_nid_postfix(second[0], &end, 1) == 0);
    Require(dup2_nid_postfix(first[1], first[1]) == first[1]);
    for (const int descriptor : {first[0], first[1], second[0], second[1]}) Require(close_nid_postfix(descriptor) == 0);
}

static void Dup2CreatesTarget() {
    int pipe[2];
    Require(pipe_nid_postfix(pipe) == 0);
    constexpr int target = 32767;
    Require(!GuestFiles::GuestFileAcquire_nid_no_patch(target));
    Require(dup2_nid_postfix(pipe[1], target) == target);
    RequirePipeCarries(target, pipe[0], "new slot");
    Require(close_nid_postfix(target) == 0);
    Require(dup2_nid_postfix(pipe[1], target) == target);
    RequirePipeCarries(target, pipe[0], "reopened");
    RequireFailure(dup2_nid_postfix(pipe[1], 32768), GuestEbadf);
    for (const int descriptor : {pipe[0], pipe[1], target}) Require(close_nid_postfix(descriptor) == 0);
}

static void StandardStreamRedirection() {
    for (auto* stream : {&_Stdout_nid_postfix, &_Stderr_nid_postfix}) {
        const int target = stream == &_Stdout_nid_postfix ? 1 : 2;
        Require(fflush_nid_postfix(stream) == 0);
        const int saved = dup_nid_postfix(target);
        Require(saved >= 3);
        int pipe[2];
        Require(pipe_nid_postfix(pipe) == 0);
        Require(dup2_nid_postfix(pipe[1], target) == target);
        Require(fileno_nid_postfix(stream) == target);
        Require(fputc_nid_postfix('S', stream) == 'S');
        Require(fflush_nid_postfix(stream) == 0);
        char value = 0;
        Require(read_nid_postfix(pipe[0], &value, 1) == 1 && value == 'S');
        Require(dup2_nid_postfix(saved, target) == target);
        Require(fileno_nid_postfix(stream) == target);
        for (const int descriptor : {saved, pipe[0], pipe[1]}) Require(close_nid_postfix(descriptor) == 0);
    }
}

static void BufferedStreamRedirection() {
    int first[2];
    int second[2];
    Require(pipe_nid_postfix(first) == 0 && pipe_nid_postfix(second) == 0);
    auto* stream = fdopen_nid_postfix(second[1], "wb");
    Require(stream != nullptr);
    Require(fwrite_nid_postfix("buffered", 1, 8, stream) == 8);
    auto retired = GuestFiles::GuestFileAcquire_nid_no_patch(second[1]);
    Require(dup2_nid_postfix(first[1], second[1]) == second[1]);
    Require(fileno_nid_postfix(stream) == second[1]);
    Require(fflush_nid_postfix(stream) == 0);
    char bytes[8]{};
    Require(read_nid_postfix(first[0], bytes, sizeof(bytes)) == 8 && std::memcmp(bytes, "buffered", 8) == 0);
    retired.reset();
    char end = 0;
    Require(read_nid_postfix(second[0], &end, 1) == 0);
    Require(fclose_nid_postfix(stream) == 0);
    for (const int descriptor : {first[0], first[1], second[0]}) Require(close_nid_postfix(descriptor) == 0);
}

static void ClosedStreamStaysStale() {
    int first[2];
    int second[2];
    Require(pipe_nid_postfix(first) == 0 && pipe_nid_postfix(second) == 0);
    auto* stream = fdopen_nid_postfix(first[1], "wb");
    Require(stream != nullptr);
    Require(close_nid_postfix(first[1]) == 0);
    Require(dup2_nid_postfix(second[1], first[1]) == first[1]);
    RequireFailure(fileno_nid_postfix(stream), GuestEbadf);
    Require(fputc_nid_postfix('X', stream) == EOF && *__error_nid_postfix() == GuestEbadf);
    Require(fclose_nid_postfix(stream) == EOF && *__error_nid_postfix() == GuestEbadf);
    RequirePipeCarries(first[1], second[0], "new owner");
    for (const int descriptor : {first[0], first[1], second[0], second[1]}) Require(close_nid_postfix(descriptor) == 0);
}

#ifndef _WIN32
static void HostDescriptorIsolation() {
    constexpr int target = 256;
    constexpr int hostOnly = 257;
    Require(::fcntl(target, F_GETFD) == -1 && errno == EBADF);
    Require(::fcntl(hostOnly, F_GETFD) == -1 && errno == EBADF);
    const int original = ::open("/dev/null", O_RDWR | O_CLOEXEC);
    Require(original >= 0 && original != target && original != hostOnly);
    Require(::dup2(original, target) == target && ::dup2(original, hostOnly) == hostOnly);
    Require(::close(original) == 0);
    struct stat expected{};
    Require(::fstat(target, &expected) == 0);
    const auto requirePrivate = [&] {
        for (const int descriptor : {target, hostOnly}) {
            struct stat actual{};
            Require(::fstat(descriptor, &actual) == 0);
            Require(actual.st_dev == expected.st_dev && actual.st_ino == expected.st_ino && actual.st_mode == expected.st_mode);
            Require(::write(descriptor, "private", 7) == 7);
        }
    };
    int pipe[2];
    Require(pipe_nid_postfix(pipe) == 0);
    Require(dup2_nid_postfix(pipe[1], target) == target);
    requirePrivate();
    const int duplicate = dup_nid_postfix(target);
    Require(duplicate >= 3 && duplicate != target);
    RequirePipeCarries(duplicate, pipe[0], "isolated");
    RequireFailure(dup_nid_postfix(hostOnly), GuestEbadf);
    RequireFailure(dup2_nid_postfix(hostOnly, target), GuestEbadf);
    RequirePipeCarries(target, pipe[0], "retained");
    for (const int descriptor : {target, duplicate, pipe[0], pipe[1]}) Require(close_nid_postfix(descriptor) == 0);
    requirePrivate();
    Require(::close(target) == 0 && ::close(hostOnly) == 0);
}

static void ReplacementRetainsLease() {
    int first[2];
    int second[2];
    Require(pipe_nid_postfix(first) == 0 && pipe_nid_postfix(second) == 0);
    auto retired = GuestFiles::GuestFileAcquire_nid_no_patch(second[0]);
    Require(retired != nullptr);
    const int native = GuestFiles::GuestFileNativeDescriptor_nid_no_patch(retired);
    Require(dup2_nid_postfix(first[0], second[0]) == second[0]);
    Require(::fcntl(native, F_GETFD) >= 0);
    Require(write_nid_postfix(second[1], "old", 3) == 3);
    char bytes[3]{};
    Require(::read(native, bytes, sizeof(bytes)) == 3 && std::memcmp(bytes, "old", 3) == 0);
    RequirePipeCarries(first[1], second[0], "new");
    retired.reset();
    Require(::fcntl(native, F_GETFD) == -1 && errno == EBADF);
    for (const int descriptor : {first[0], first[1], second[0], second[1]}) Require(close_nid_postfix(descriptor) == 0);
}

static void LoweredLimitPreservesStreamTarget() {
    constexpr int nativeTarget = 256;
    Require(::fcntl(nativeTarget, F_GETFD) == -1 && errno == EBADF);
    int source[2];
    Require(pipe_nid_postfix(source) == 0);
    int original[2];
    Require(::pipe(original) == 0 && original[0] != nativeTarget && original[1] != nativeTarget);
    Require(::dup2(original[1], nativeTarget) == nativeTarget && ::close(original[1]) == 0);
    Require(::fcntl(original[0], F_SETFL, O_NONBLOCK) == 0);
    auto* native = ::fdopen(nativeTarget, "wb");
    Require(native != nullptr);
    FileStream stream(native, 1, false);
    const int target = fileno_nid_postfix(&stream);
    Require(target >= 3);
    struct rlimit previous{};
    Require(::getrlimit(RLIMIT_NOFILE, &previous) == 0 && previous.rlim_cur > 128);
    auto lowered = previous;
    lowered.rlim_cur = 128;
    Require(::setrlimit(RLIMIT_NOFILE, &lowered) == 0);
    RequireFailure(dup2_nid_postfix(source[1], target), GuestEbadf);
    Require(fileno_nid_postfix(&stream) == target);
    Require(fputc_nid_postfix('L', &stream) == 'L' && fflush_nid_postfix(&stream) == 0);
    char value = 0;
    Require(::read(original[0], &value, 1) == 1 && value == 'L');
    Require(::setrlimit(RLIMIT_NOFILE, &previous) == 0);
    Require(stream.Close() == 0);
    Require(::fcntl(nativeTarget, F_GETFD) == -1 && errno == EBADF);
    Require(::close(original[0]) == 0);
    Require(close_nid_postfix(source[0]) == 0 && close_nid_postfix(source[1]) == 0);
}
#endif

static void BadDescriptors() {
    int pipe[2];
    Require(pipe_nid_postfix(pipe) == 0);
    const int closed = pipe[1];
    Require(close_nid_postfix(closed) == 0);
    RequireFailure(dup_nid_postfix(-1), GuestEbadf);
    RequireFailure(dup_nid_postfix(closed), GuestEbadf);
    RequireFailure(dup2_nid_postfix(closed, pipe[0]), GuestEbadf);
    RequireFailure(dup2_nid_postfix(closed, closed), GuestEbadf);
    RequireFailure(dup2_nid_postfix(pipe[0], -1), GuestEbadf);
    RequireFailure(dup2_nid_postfix(-1, pipe[0]), GuestEbadf);
    RequireFailure(dup_nid_postfix(FirstSocket + 0x0ffffff0), GuestEbadf);
    RequireFailure(dup2_nid_postfix(FirstSocket + 0x0ffffff0, FirstSocket + 0x0ffffff1), GuestEbadf);
    Require(close_nid_postfix(pipe[0]) == 0);
}

static void SocketDuplicates() {
    int pair[2];
    Require(socketpair_nid_postfix(Unix, Stream, 0, pair) == 0);
    const int duplicate = dup_nid_postfix(pair[0]);
    Require(duplicate >= FirstSocket && duplicate != pair[0] && duplicate != pair[1]);
    Require(close_nid_postfix(pair[0]) == 0);
    RequireSocketCarries(duplicate, pair[1], "socket");
    const int target = duplicate + 0x1000;
    Require(dup2_nid_postfix(duplicate, target) == target);
    Require(close_nid_postfix(duplicate) == 0);
    RequireSocketCarries(target, pair[1], "moved");
    Require(dup2_nid_postfix(target, target) == target);
    int next[2];
    Require(socketpair_nid_postfix(Unix, Stream, 0, next) == 0);
    Require(next[0] > target && next[1] > target);
    RequireSocketCarries(target, pair[1], "kept");
    Require(dup2_nid_postfix(next[0], target) == target);
    RequireSocketCarries(target, next[1], "over");
    Require(Throws([&] { dup2_nid_postfix(target, 1); }));
    int pipe[2];
    Require(pipe_nid_postfix(pipe) == 0);
    Require(Throws([&] { dup2_nid_postfix(pipe[0], target); }));
    for (const int descriptor : {target, pair[1], next[0], next[1], pipe[0], pipe[1]}) Require(close_nid_postfix(descriptor) == 0);
}

static void RandomDeviceDuplicates() {
    const int random = open_nid_postfix("/dev/urandom", 0, 0);
    Require(random >= 0);
    const int dupRandom = dup_nid_postfix(random);
    Require(dupRandom >= 0 && dupRandom != random);
    unsigned char randomBuf1[16] = {};
    unsigned char randomBuf2[16] = {};
    Require(read_nid_postfix(random, randomBuf1, sizeof(randomBuf1)) == sizeof(randomBuf1));
    Require(read_nid_postfix(dupRandom, randomBuf2, sizeof(randomBuf2)) == sizeof(randomBuf2));
    Require(std::memcmp(randomBuf1, randomBuf2, sizeof(randomBuf1)) != 0);
    Require(close_nid_postfix(random) == 0);
    Require(read_nid_postfix(dupRandom, randomBuf1, sizeof(randomBuf1)) == sizeof(randomBuf1));
    Require(dup2_nid_postfix(dupRandom, random) == random);

    Require(dup2_nid_postfix(random, random) == random);
    Require(read_nid_postfix(random, randomBuf1, sizeof(randomBuf1)) == sizeof(randomBuf1));

    int pipe[2];
    Require(pipe_nid_postfix(pipe) == 0);
    Require(write_nid_postfix(pipe[1], "hello", 5) == 5);
    Require(dup2_nid_postfix(pipe[0], random) == random);
    char textBuf[8] = {};
    Require(read_nid_postfix(random, textBuf, 5) == 5);
    Require(std::memcmp(textBuf, "hello", 5) == 0);

    const int targetHost = pipe[0];
    Require(dup2_nid_postfix(dupRandom, targetHost) == targetHost);
    unsigned char randomBuf3[16] = {};
    Require(read_nid_postfix(targetHost, randomBuf3, sizeof(randomBuf3)) == sizeof(randomBuf3));
    Require(std::memcmp(randomBuf3, randomBuf2, sizeof(randomBuf3)) != 0);

    const int secondRandom = open_nid_postfix("/dev/random", 0, 0);
    Require(secondRandom >= 0);
    Require(dup2_nid_postfix(dupRandom, secondRandom) == secondRandom);
    unsigned char randomBuf4[16] = {};
    Require(read_nid_postfix(secondRandom, randomBuf4, sizeof(randomBuf4)) == sizeof(randomBuf4));
    Require(std::memcmp(randomBuf4, randomBuf3, sizeof(randomBuf4)) != 0);

    const int invalidFd = FirstSocket - 100;
    RequireFailure(dup2_nid_postfix(invalidFd, secondRandom), GuestEbadf);
    unsigned char randomBuf5[16] = {};
    Require(read_nid_postfix(secondRandom, randomBuf5, sizeof(randomBuf5)) == sizeof(randomBuf5));

    for (const int descriptor : {random, dupRandom, targetHost, pipe[1], secondRandom}) Require(close_nid_postfix(descriptor) == 0);
}

int main() {
    DupOutlivesOriginal();
    Dup2ReplacesTarget();
    Dup2CreatesTarget();
    StandardStreamRedirection();
    BufferedStreamRedirection();
    ClosedStreamStaysStale();
#ifndef _WIN32
    HostDescriptorIsolation();
    ReplacementRetainsLease();
    LoweredLimitPreservesStreamTarget();
#endif
    BadDescriptors();
    SocketDuplicates();
    RandomDeviceDuplicates();
}
