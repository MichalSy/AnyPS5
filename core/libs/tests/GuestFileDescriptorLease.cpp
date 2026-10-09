#include "prx/libc/include/GuestFileDescriptors.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libkernel/KernelErrors.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern "C" {
int APS5_VABI sceKernelOpen(const char*, int, std::uint16_t);
int APS5_VABI sceKernelClose(int);
std::int64_t APS5_VABI sceKernelRead(int, void*, std::size_t);
std::int64_t APS5_VABI sceKernelWrite(int, const void*, std::size_t);
std::int64_t APS5_VABI sceKernelLseek(int, std::int64_t, int);
int APS5_VABI close_nid_postfix(int);
int APS5_VABI pipe_nid_postfix(int*);
int* APS5_VABI __error_nid_postfix();
}

static void Require(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "Guest file descriptor lease: %s\n", message);
        std::exit(1);
    }
}

class Seed {
public:
    std::string path;

    explicit Seed(const char* payload) {
        char name[] = "anyps5-fd-lease-XXXXXX";
        const int descriptor = ::mkstemp(name);
        Require(descriptor >= 0, "owned native seed must open");
        const auto size = std::strlen(payload);
        Require(::write(descriptor, payload, size) == static_cast<ssize_t>(size) && ::close(descriptor) == 0,
            "seed payload must be persisted and its descriptor closed");
        path = name;
    }

    ~Seed() { Require(::unlink(path.c_str()) == 0, "fixture seed must be removed"); }
};

static int CheckNativeInode(const GuestFiles::Lease& lease, const char* path) {
    Require(lease != nullptr, "a real guest entry lease must exist");
    const int native = GuestFiles::NativeDescriptor_nid_no_patch(lease);
    struct stat expected{}, actual{};
    Require(::stat(path, &expected) == 0 && ::fstat(native, &actual) == 0, "leased native descriptor must be stat-able");
    Require(expected.st_dev == actual.st_dev && expected.st_ino == actual.st_ino, "leased native descriptor must own the intended inode");
    return native;
}

static void WaitFor(const std::atomic<int>& phase, int expected) {
    while (phase.load() != expected) std::this_thread::yield();
}

static void CheckPrivateReuse(int native, const GuestFiles::Identity& retired) {
    Require(::fcntl(native, F_GETFD) == -1 && errno == EBADF, "last lease release must close its native descriptor");
    const int opened = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    Require(opened >= 0, "private maps descriptor must open");
    if (opened != native) {
        Require(::fcntl(native, F_GETFD) == -1 && errno == EBADF, "native reuse must not replace another owner");
        Require(::dup2(opened, native) == native && ::close(opened) == 0, "private maps must reuse the released native number");
    }
    struct stat expected{}, actual{};
    Require(::fstat(native, &expected) == 0, "private maps inode must be recorded");
    Require(GuestFiles::CloseMatching_nid_no_patch(retired) == -1 && *__error_nid_postfix() == 9,
        "expired identity cleanup must report EBADF");
    Require(::fstat(native, &actual) == 0 && actual.st_dev == expected.st_dev && actual.st_ino == expected.st_ino,
        "expired entry cleanup must leave reused private ownership intact");
    std::array<char, 64> maps{};
    Require(::read(native, maps.data(), maps.size()) > 0, "reused private maps descriptor must remain readable");
    Require(::close(native) == 0, "fixture private maps descriptor must close");
}

static void CheckLeaseClose() {
    Seed oldFile("old-payload");
    Seed newFile("replacement");
    const int guest = sceKernelOpen(oldFile.path.c_str(), 0, 0);
    Require(guest >= 3 && guest <= 32767, "guest open must return an ordinary logical ID");
    int native = -1;
    GuestFiles::Identity identity;
    std::atomic<int> phase{0};
    std::thread worker([&] {
        auto lease = GuestFiles::Acquire_nid_no_patch(guest);
        native = CheckNativeInode(lease, oldFile.path.c_str());
        Require(GuestFiles::LogicalDescriptor_nid_no_patch(lease) == guest && GuestFiles::AccessMode_nid_no_patch(lease) == 0,
            "lease metadata must preserve logical ID and read-only access");
        identity = lease;
        phase.store(1);
        WaitFor(phase, 2);
        Require(::lseek(native, 0, SEEK_SET) == 0, "the old pinned open-file description must remain seekable");
        std::array<char, 11> payload{};
        Require(::read(native, payload.data(), payload.size()) == 11 && std::memcmp(payload.data(), "old-payload", 11) == 0,
            "in-flight ownership must continue reading the old inode after close and logical reuse");
        *__error_nid_postfix() = 13;
        lease.reset();
        Require(*__error_nid_postfix() == 13, "deferred native cleanup must preserve the releasing thread's errno");
        phase.store(3);
    });
    WaitFor(phase, 1);
    Require(sceKernelClose(guest) == 0, "logical close must complete while another thread pins the native entry");
    Require(::fcntl(native, F_GETFD) >= 0, "in-flight lease must keep the old native descriptor alive");
    Require(!GuestFiles::Acquire_nid_no_patch(guest) && *__error_nid_postfix() == 9, "new lookup must fail immediately after logical close");
    const int replacement = sceKernelOpen(newFile.path.c_str(), 0, 0);
    Require(replacement == guest, "a NEW guest open must reuse the retired logical ID while old IO remains pinned");
    {
        const auto lease = GuestFiles::Acquire_nid_no_patch(replacement);
        Require(CheckNativeInode(lease, newFile.path.c_str()) != native, "new logical ownership must use a different native inode and descriptor");
    }
    std::array<char, 2> prefix{};
    Require(sceKernelRead(replacement, prefix.data(), prefix.size()) == 2 && prefix[0] == 'r' && prefix[1] == 'e',
        "replacement payload and position must be real");
    Require(!GuestFiles::Matches_nid_no_patch(identity), "old identity must not match the reused logical entry");
    Require(GuestFiles::CloseMatching_nid_no_patch(identity) == -1 && *__error_nid_postfix() == 9,
        "old identity close must not retire the new logical entry");
    phase.store(2);
    WaitFor(phase, 3);
    worker.join();
    CheckPrivateReuse(native, identity);
    Require(sceKernelLseek(replacement, 0, 1) == 2, "old leased IO and cleanup must preserve the new entry's position");
    std::array<char, 9> suffix{};
    Require(sceKernelRead(replacement, suffix.data(), suffix.size()) == 9 && std::memcmp(suffix.data(), "placement", 9) == 0,
        "replacement bytes must survive final old-lease cleanup");
    Require(close_nid_postfix(replacement) == 0, "valid replacement must still close normally");
}

static void CheckCloseRace() {
    Seed file("old-payload");
    const int guest = sceKernelOpen(file.path.c_str(), 0, 0);
    auto initial = GuestFiles::Acquire_nid_no_patch(guest);
    const int native = CheckNativeInode(initial, file.path.c_str());
    const GuestFiles::Identity identity = initial;
    initial.reset();
    std::atomic<int> ready{0};
    std::atomic<bool> release{false};
    std::array<int, 2> results{};
    std::array<std::thread, 2> closers;
    for (int index = 0; index < 2; ++index) {
        closers[index] = std::thread([&, index] {
            ready.fetch_add(1);
            while (!release.load()) std::this_thread::yield();
            results[index] = sceKernelClose(guest);
        });
    }
    WaitFor(ready, 2);
    release.store(true);
    for (auto& closer : closers) closer.join();
    Require((results[0] == 0 && results[1] == SCE_KERNEL_ERROR_EBADF) ||
        (results[1] == 0 && results[0] == SCE_KERNEL_ERROR_EBADF), "exactly one concurrent logical closer must win");
    CheckPrivateReuse(native, identity);
    const int reopened = sceKernelOpen(file.path.c_str(), 0, 0);
    Require(reopened == guest, "a new valid guest open must reuse the ID after both old closers complete");
    std::array<char, 11> payload{};
    Require(sceKernelRead(reopened, payload.data(), payload.size()) == 11 && std::memcmp(payload.data(), "old-payload", 11) == 0,
        "valid reopen must retain real data after the close race");
    Require(close_nid_postfix(reopened) == 0, "reopened guest entry must close normally");
}

static void CheckBlockedRead() {
    Seed replacementFile("replacement");
    int ends[2] = {-1, -1};
    Require(pipe_nid_postfix(ends) == 0, "guest pipe must open");
    auto initial = GuestFiles::Acquire_nid_no_patch(ends[0]);
    Require(initial != nullptr, "read pipe must have a real guest entry");
    const int native = GuestFiles::NativeDescriptor_nid_no_patch(initial);
    Require(native >= 0, "read pipe must have a real native descriptor");
    initial.reset();
    std::array<char, 4> payload = {'x', 'x', 'x', 'x'};
    std::int64_t result = -1;
    std::atomic<long> threadId{0};
    std::atomic<bool> finished{false};
    std::thread reader([&] {
        threadId.store(::syscall(SYS_gettid));
        result = sceKernelRead(ends[0], payload.data(), payload.size());
        finished.store(true);
    });
    while (threadId.load() == 0) std::this_thread::yield();
    const auto path = "/proc/self/task/" + std::to_string(threadId.load()) + "/syscall";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    bool blocked = false;
    while (!finished.load() && std::chrono::steady_clock::now() < deadline) {
        std::ifstream status(path);
        std::string line;
        std::getline(status, line);
        std::istringstream fields(line);
        long number = -1;
        std::string descriptor;
        if (fields >> number >> descriptor && number == SYS_read) {
            char* end = nullptr;
            const auto active = std::strtol(descriptor.c_str(), &end, 0);
            if (end != descriptor.c_str() && *end == '\0' && active == native) {
                blocked = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Require(blocked, "the actual guest SDK read must be blocked in the expected native pipe syscall");
    Require(sceKernelClose(ends[0]) == 0, "logical close must not wait on the blocked IO or global table mutex");
    Require(::fcntl(native, F_GETFD) >= 0, "the actual SDK read must retain its native lease while blocked");
    const int replacement = sceKernelOpen(replacementFile.path.c_str(), 0, 0);
    Require(replacement == ends[0], "a NEW open must reuse the retired logical read ID");
    {
        const auto lease = GuestFiles::Acquire_nid_no_patch(replacement);
        Require(CheckNativeInode(lease, replacementFile.path.c_str()) != native, "new guest ownership must not reuse the still-pinned native pipe fd");
    }
    Require(sceKernelWrite(ends[1], "pipe", 4) == 4, "guest write must unblock the original pipe IO");
    reader.join();
    Require(result == 4 && std::memcmp(payload.data(), "pipe", 4) == 0, "blocked SDK IO must complete on its original pipe after logical-ID reuse");
    Require(::fcntl(native, F_GETFD) == -1 && errno == EBADF, "completed SDK IO must release the retired native pipe ownership");
    std::array<char, 11> replacementBytes{};
    Require(sceKernelRead(replacement, replacementBytes.data(), replacementBytes.size()) == 11 &&
        std::memcmp(replacementBytes.data(), "replacement", 11) == 0, "the replacement file must retain its own initial position and real payload");
    Require(sceKernelClose(replacement) == 0 && sceKernelClose(ends[1]) == 0, "fixture guest resources must close normally");
}

struct NativeFileInfo {
    int descriptor;
    dev_t device;
    ino_t inode;
    mode_t mode;
    dev_t deviceId;
    bool operator==(const NativeFileInfo&) const = default;
};

static std::vector<NativeFileInfo> NativeInventory() {
    DIR* directory = ::opendir("/proc/self/fd");
    Require(directory != nullptr, "native inventory must open");
    const int observer = ::dirfd(directory);
    std::vector<NativeFileInfo> result;
    while (auto* entry = ::readdir(directory)) {
        char* end = nullptr;
        const auto number = std::strtol(entry->d_name, &end, 10);
        if (end == entry->d_name || *end != '\0' || number < 0 || number > INT32_MAX || number == observer) continue;
        struct stat info{};
        Require(::fstat(static_cast<int>(number), &info) == 0, "an inventoried native descriptor must be valid");
        result.push_back({static_cast<int>(number), info.st_dev, info.st_ino, info.st_mode, info.st_rdev});
    }
    Require(::closedir(directory) == 0, "native inventory observer must close");
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) { return left.descriptor < right.descriptor; });
    return result;
}

static void CheckPipeRollback() {
    struct rlimit original{};
    Require(::getrlimit(RLIMIT_NOFILE, &original) == 0, "fixture descriptor limit must be readable");
    constexpr rlim_t required = 32768 + 128;
    if (original.rlim_cur < required) {
        Require(original.rlim_max == RLIM_INFINITY || original.rlim_max >= required, "host hard limit must allow the actual guest table capacity");
        auto raised = original;
        raised.rlim_cur = required;
        Require(::setrlimit(RLIMIT_NOFILE, &raised) == 0, "only the fixture process soft descriptor limit may be raised");
    }
    Seed sentinel("sentinel");
    int prime[2] = {-1, -1};
    Require(pipe_nid_postfix(prime) == 0 && close_nid_postfix(prime[0]) == 0 && close_nid_postfix(prime[1]) == 0,
        "guest pipe and output-buffer machinery must initialize before the inventory baseline");
    const auto originalInventory = NativeInventory();
    std::vector<int> occupied;
    occupied.reserve(32764);
    const int first = sceKernelOpen(sentinel.path.c_str(), 0, 0);
    Require(first == 3, "fresh ordinary guest allocation must begin at ID3");
    occupied.push_back(first);
    const auto started = std::chrono::steady_clock::now();
    for (int expected = 4; expected <= 32766; ++expected) {
        const int native = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        Require(native >= 0, "fixture must own every adopted native null descriptor");
        auto lease = GuestFiles::AdoptOwned_nid_no_patch(native, 0);
        Require(lease != nullptr && GuestFiles::LogicalDescriptor_nid_no_patch(lease) == expected,
            "real adoption must fill the exact supported logical-ID range");
        occupied.push_back(expected);
        if ((expected & 1023) == 0)
            Require(std::chrono::steady_clock::now() - started < std::chrono::seconds(20), "actual table capacity setup must stay bounded");
    }
    Require(!GuestFiles::Acquire_nid_no_patch(32767) && *__error_nid_postfix() == 9, "exactly one logical pipe slot must remain empty");
    const auto before = NativeInventory();
    int outputs[2] = {0x12345678, 0x55667788};
    Require(pipe_nid_postfix(outputs) == -1 && *__error_nid_postfix() == 24, "a pipe requiring two logical IDs must fail with guest EMFILE");
    Require(outputs[0] == 0x12345678 && outputs[1] == 0x55667788, "failed atomic pipe adoption must leave both output canaries unchanged");
    Require(NativeInventory() == before, "failed atomic pipe adoption must close both new native ends without changing existing ownership");
    std::array<char, 8> contents{};
    Require(sceKernelRead(first, contents.data(), contents.size()) == 8 && std::memcmp(contents.data(), "sentinel", 8) == 0,
        "existing guest payload and IO must survive the failed pipe adoption");
    {
        const auto lease = GuestFiles::Acquire_nid_no_patch(occupied.back());
        CheckNativeInode(lease, "/dev/null");
        char untouched = 'x';
        Require(sceKernelRead(occupied.back(), &untouched, 1) == 0 && untouched == 'x', "existing null-device ownership must retain real EOF behavior");
    }
    Require(sceKernelClose(occupied.back()) == 0, "one occupied ID must release to make two pipe slots available");
    occupied.pop_back();
    int ends[2] = {-1, -1};
    Require(pipe_nid_postfix(ends) == 0 && ends[0] == 32766 && ends[1] == 32767, "the same real pipe must publish both available IDs atomically");
    Require(sceKernelWrite(ends[1], "atomic-pipe", 11) == 11, "successful high-ID guest pipe must write its real payload");
    std::array<char, 11> payload{};
    Require(sceKernelRead(ends[0], payload.data(), payload.size()) == 11 && std::memcmp(payload.data(), "atomic-pipe", 11) == 0,
        "successful high-ID guest pipe must read its real payload");
    Require(close_nid_postfix(ends[0]) == 0 && close_nid_postfix(ends[1]) == 0, "successful guest pipe must release both owned ends");
    for (const int descriptor : occupied) Require(sceKernelClose(descriptor) == 0, "fixture adopted ownership must close normally");
    Require(NativeInventory() == originalInventory, "all fixture adoptions must restore the original native fd and inode inventory");
    if (original.rlim_cur < required) Require(::setrlimit(RLIMIT_NOFILE, &original) == 0, "fixture process soft limit must be restored");
}

int main(int argc, char** argv) {
    Require(argc == 2, "one fresh-process mode is required");
    if (std::strcmp(argv[1], "lease") == 0) CheckLeaseClose();
    else if (std::strcmp(argv[1], "blocked-read") == 0) CheckBlockedRead();
    else if (std::strcmp(argv[1], "pipe-rollback") == 0) CheckPipeRollback();
    else {
        Require(std::strcmp(argv[1], "race") == 0, "mode must be known");
        CheckCloseRace();
    }
    std::puts("guest file descriptor lease tests passed");
}
