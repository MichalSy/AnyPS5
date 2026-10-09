#include "prx/libc/include/FileStream.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libkernel/KernelErrors.hpp"
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

extern "C" {
int APS5_VABI sceKernelOpen(const char*, int, std::uint16_t);
int APS5_VABI sceKernelClose(int);
std::int64_t APS5_VABI sceKernelRead(int, void*, std::size_t);
std::int64_t APS5_VABI sceKernelLseek(int, std::int64_t, int);
int APS5_VABI open_nid_postfix(const char*, int, int);
int APS5_VABI _open_nid_postfix(const char*, int, ...);
int APS5_VABI close_nid_postfix(int);
int APS5_VABI _close_nid_postfix(int);
int* APS5_VABI __error_nid_postfix();
FileStream* APS5_VABI fopen_nid_postfix(const char*, const char*);
FileStream* APS5_VABI fdopen_nid_postfix(int, const char*);
int APS5_VABI fclose_nid_postfix(FileStream*);
int APS5_VABI fileno_nid_postfix(FileStream*);
std::size_t APS5_VABI fread_nid_postfix(void*, std::size_t, std::size_t, FileStream*);
int APS5_VABI fseeko_nid_postfix(FileStream*, std::int64_t, int);
}

static void Require(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "Guest file host isolation: %s\n", message);
        std::exit(1);
    }
}

class Seed {
public:
    std::string path;

    explicit Seed(const char* payload) {
        char name[] = "anyps5-fd-isolation-XXXXXX";
        const int descriptor = ::mkstemp(name);
        Require(descriptor >= 0, "private native seed must open");
        const auto size = std::strlen(payload);
        Require(::write(descriptor, payload, size) == static_cast<ssize_t>(size), "native seed bytes must persist");
        Require(::close(descriptor) == 0, "native seed descriptor must close");
        path = name;
    }

    ~Seed() { Require(::unlink(path.c_str()) == 0, "fixture seed must be removed"); }
};

static std::vector<int> FindNativeFiles(const char* path) {
    struct stat expected{};
    Require(::stat(path, &expected) == 0, "native target stat must succeed");
    DIR* directory = ::opendir("/proc/self/fd");
    Require(directory != nullptr, "native descriptor listing must open");
    std::vector<int> result;
    while (auto* entry = ::readdir(directory)) {
        char* end = nullptr;
        const long number = std::strtol(entry->d_name, &end, 10);
        if (end == entry->d_name || *end != '\0' || number < 0 || number > INT32_MAX) continue;
        struct stat info{};
        if (::fstat(static_cast<int>(number), &info) == 0 && info.st_dev == expected.st_dev && info.st_ino == expected.st_ino)
            result.push_back(static_cast<int>(number));
    }
    Require(::closedir(directory) == 0, "native descriptor listing must close");
    return result;
}

static void CheckMaps(int descriptor, const struct stat& expected) {
    struct stat actual{};
    Require(::fcntl(descriptor, F_GETFD) >= 0 && ::fstat(descriptor, &actual) == 0, "private maps descriptor must remain open");
    Require(actual.st_dev == expected.st_dev && actual.st_ino == expected.st_ino, "private maps inode must remain unchanged");
    std::array<char, 64> bytes{};
    Require(::pread(descriptor, bytes.data(), bytes.size(), 0) > 0, "private maps descriptor must remain readable");
}

static struct stat ReuseWithMaps(int descriptor) {
    Require(::fcntl(descriptor, F_GETFD) == -1 && errno == EBADF, "old native number must be unowned before reuse");
    const int opened = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    Require(opened >= 0, "private maps descriptor must open");
    if (opened != descriptor) {
        Require(::fcntl(descriptor, F_GETFD) == -1 && errno == EBADF, "forced native reuse must not replace another owner");
        Require(::dup2(opened, descriptor) == descriptor, "private maps must reuse the released native number");
        Require(::close(opened) == 0, "unused private maps alias must close");
    }
    struct stat result{};
    Require(::fstat(descriptor, &result) == 0, "reused private maps inode must be recorded");
    CheckMaps(descriptor, result);
    return result;
}

static void CheckLogicalRange(int guest) {
    Require(guest >= 3 && guest <= 32767, "ordinary guest IDs must fit the signed16 FILE descriptor field");
}

static void CheckHostReuse(const char* mode) {
    const bool posix = std::strcmp(mode, "posix") == 0;
    const bool underscore = std::strcmp(mode, "underscore") == 0;
    const bool stdio = std::strcmp(mode, "stdio") == 0;
    const bool fdopen = std::strcmp(mode, "fdopen") == 0;
    Seed seed("payload");
    FileStream* stream = nullptr;
    int guest;
    if (stdio) {
        stream = fopen_nid_postfix(seed.path.c_str(), "rb");
        Require(stream != nullptr, "guest fopen must succeed");
        guest = fileno_nid_postfix(stream);
    } else {
        guest = posix ? open_nid_postfix(seed.path.c_str(), 0, 0) :
            underscore ? _open_nid_postfix(seed.path.c_str(), 0) : sceKernelOpen(seed.path.c_str(), 0, 0);
        if (fdopen) {
            stream = fdopen_nid_postfix(guest, "rb");
            Require(stream != nullptr && fileno_nid_postfix(stream) == guest, "fdopen must retain the existing guest ID");
        }
    }
    CheckLogicalRange(guest);
    if (stream) Require(stream->GuestState().descriptor == guest, "FILE prefix and fileno must expose the same logical ID");
    const auto nativeFiles = FindNativeFiles(seed.path.c_str());
    Require(!nativeFiles.empty(), "guest open must own the real payload inode");
    const int native = nativeFiles.front();
    if (stream) Require(fclose_nid_postfix(stream) == 0, "guest fclose must release its ownership");
    else Require((posix ? close_nid_postfix(guest) : underscore ? _close_nid_postfix(guest) : sceKernelClose(guest)) == 0,
        "initial guest close must succeed");
    for (const int descriptor : nativeFiles)
        Require(::fcntl(descriptor, F_GETFD) == -1 && errno == EBADF, "all native descriptors belonging to the closed guest file must be released");

    const auto maps = ReuseWithMaps(native);
    *__error_nid_postfix() = 13;
    const int stale = posix ? close_nid_postfix(guest) : underscore ? _close_nid_postfix(guest) : sceKernelClose(guest);
    if (posix || underscore) Require(stale == -1 && *__error_nid_postfix() == 9, "stale POSIX close must report guest EBADF");
    else Require(stale == SCE_KERNEL_ERROR_EBADF, "stale SDK close must report SDK EBADF");
    CheckMaps(native, maps);
    std::array<char, 4> untouched = {'x', 'x', 'x', 'x'};
    Require(sceKernelRead(guest, untouched.data(), untouched.size()) == SCE_KERNEL_ERROR_EBADF,
        "a closed logical ID must not read a private host descriptor");
    Require(untouched == std::array<char, 4>{'x', 'x', 'x', 'x'}, "failed guest read must leave its destination unchanged");
    Require(sceKernelLseek(guest, 0, 0) == SCE_KERNEL_ERROR_EBADF, "a closed logical ID must not seek a private host descriptor");
    Require(fdopen_nid_postfix(native, "rb") == nullptr && *__error_nid_postfix() == 9,
        "guest fdopen must not adopt a private host fd number");
    CheckMaps(native, maps);

    const int reopened = sceKernelOpen(seed.path.c_str(), 0, 0);
    Require(reopened == guest, "a NEW guest open must reuse the logical ID independently of native reuse");
    const auto newNativeFiles = FindNativeFiles(seed.path.c_str());
    Require(!newNativeFiles.empty(), "new logical ownership must refer to the payload inode");
    for (const int descriptor : newNativeFiles) Require(descriptor != native, "new guest ownership must not refer to the private maps fd");
    std::array<char, 7> payload{};
    Require(sceKernelRead(reopened, payload.data(), payload.size()) == 7 && std::memcmp(payload.data(), "payload", 7) == 0,
        "the reused guest ID must read the real payload");
    Require(sceKernelLseek(reopened, 0, 1) == 7, "the reused guest ID must maintain its own real file position");
    Require(close_nid_postfix(reopened) == 0, "cross-API close of a valid guest file must succeed");
    Require(FindNativeFiles(seed.path.c_str()).empty(), "the new guest file must release its actual native ownership");
    CheckMaps(native, maps);
    Require(::close(native) == 0, "fixture must close its private maps descriptor");
}

static void CheckStaleStream(bool fdopen) {
    Seed oldFile("old-payload");
    Seed newFile("replacement");
    FileStream* stream;
    int guest;
    if (fdopen) {
        guest = sceKernelOpen(oldFile.path.c_str(), 0, 0);
        stream = fdopen_nid_postfix(guest, "rb");
    } else {
        stream = fopen_nid_postfix(oldFile.path.c_str(), "rb");
        guest = stream ? fileno_nid_postfix(stream) : -1;
    }
    Require(stream != nullptr, "the original guest stream must open");
    CheckLogicalRange(guest);
    Require(fileno_nid_postfix(stream) == guest && stream->GuestState().descriptor == guest, "the original stream must expose its logical ID");
    Require(sceKernelClose(guest) == 0, "SDK close must retire the stream's logical entry");
    Require(FindNativeFiles(oldFile.path.c_str()).size() == 1, "a stale FILE must retain only its private host FILE descriptor");
    const int replacement = sceKernelOpen(newFile.path.c_str(), 0, 0);
    Require(replacement == guest, "a NEW open must reuse the retired logical ID");
    std::array<char, 2> prefix{};
    Require(sceKernelRead(replacement, prefix.data(), prefix.size()) == 2 && prefix[0] == 'r' && prefix[1] == 'e',
        "replacement payload and initial position must be real");
    std::array<char, 4> untouched = {'x', 'x', 'x', 'x'};
    Require(fread_nid_postfix(untouched.data(), 1, untouched.size(), stream) == 0 && *__error_nid_postfix() == 9,
        "the old FILE must reject IO after logical-ID reuse");
    Require(untouched == std::array<char, 4>{'x', 'x', 'x', 'x'}, "stale FILE read must not change its destination");
    Require(fseeko_nid_postfix(stream, 0, SEEK_SET) == -1 && *__error_nid_postfix() == 9,
        "the old FILE must not seek the replacement entry");
    Require(fileno_nid_postfix(stream) == -1 && *__error_nid_postfix() == 9, "stale fileno must report normal guest EBADF");
    Require(fclose_nid_postfix(stream) == EOF && *__error_nid_postfix() == 9, "stale fclose must report EBADF and free only its private ownership");
    Require(FindNativeFiles(oldFile.path.c_str()).empty(), "stale fclose must release the old private FILE exactly once");
    Require(sceKernelLseek(replacement, 0, 1) == 2, "old FILE operations must not alter the replacement position");
    std::array<char, 9> suffix{};
    Require(sceKernelRead(replacement, suffix.data(), suffix.size()) == 9 && std::memcmp(suffix.data(), "placement", 9) == 0,
        "replacement payload must remain intact after stale fclose");
    Require(sceKernelClose(replacement) == 0, "the replacement entry must still close normally");
    Require(FindNativeFiles(newFile.path.c_str()).empty(), "replacement close must release its real native ownership");
}

static void CheckClosedStandard() {
    Seed seed("payload");
    const int initialized = sceKernelOpen(seed.path.c_str(), 0, 0);
    CheckLogicalRange(initialized);
    Require(sceKernelClose(initialized) == 0, "standard initialization probe must close");
    struct stat original{};
    Require(::fstat(0, &original) == 0, "fixture host stdin must initially exist");
    const int saved = ::dup(0);
    Require(saved >= 0, "fixture must privately preserve its host stdin");
    Require(sceKernelClose(0) == 0, "guest stdin ownership must close once");
    struct stat retained{};
    Require(::fstat(0, &retained) == 0 && retained.st_dev == original.st_dev && retained.st_ino == original.st_ino,
        "closing guest stdin must retain the original host stdin");
    Require(::close(0) == 0, "fixture may release only its own host stdin number for controlled reuse");
    const auto maps = ReuseWithMaps(0);
    Require(sceKernelClose(0) == SCE_KERNEL_ERROR_EBADF, "stale guest stdin close must not close reused private host fd0");
    Require(close_nid_postfix(0) == -1 && *__error_nid_postfix() == 9, "POSIX stdin close must reject an absent guest standard");
    std::array<char, 4> untouched = {'x', 'x', 'x', 'x'};
    Require(sceKernelRead(0, untouched.data(), untouched.size()) == SCE_KERNEL_ERROR_EBADF,
        "closed guest stdin must not read reused private host fd0");
    Require(untouched == std::array<char, 4>{'x', 'x', 'x', 'x'}, "closed guest stdin read must preserve its destination");
    CheckMaps(0, maps);
    const int reopened = sceKernelOpen(seed.path.c_str(), 0, 0);
    CheckLogicalRange(reopened);
    std::array<char, 7> payload{};
    Require(sceKernelRead(reopened, payload.data(), payload.size()) == 7 && std::memcmp(payload.data(), "payload", 7) == 0,
        "ordinary guest open must remain correct while host fd0 is private");
    Require(sceKernelClose(reopened) == 0, "ordinary guest close must succeed");
    CheckMaps(0, maps);
    Require(::dup2(saved, 0) == 0 && ::close(saved) == 0, "fixture must restore its original host stdin");
    Require(sceKernelClose(0) == SCE_KERNEL_ERROR_EBADF, "restored host stdin must not be lazily adopted as a guest entry");
    Require(::fstat(0, &retained) == 0 && retained.st_dev == original.st_dev && retained.st_ino == original.st_ino,
        "the restored host stdin must remain intact");
}

int main(int argc, char** argv) {
    Require(argc == 2, "one fresh-process mode is required");
    if (std::strcmp(argv[1], "stdio-stale") == 0) CheckStaleStream(false);
    else if (std::strcmp(argv[1], "fdopen-stale") == 0) CheckStaleStream(true);
    else if (std::strcmp(argv[1], "standard") == 0) CheckClosedStandard();
    else {
        Require(std::strcmp(argv[1], "kernel") == 0 || std::strcmp(argv[1], "posix") == 0 ||
            std::strcmp(argv[1], "underscore") == 0 || std::strcmp(argv[1], "stdio") == 0 || std::strcmp(argv[1], "fdopen") == 0,
            "mode must be known");
        CheckHostReuse(argv[1]);
    }
    std::puts("guest file descriptor host isolation tests passed");
}
