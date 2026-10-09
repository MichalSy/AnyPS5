#include "nid/NidCompute.hpp"
#include "prx/libc/include/FileStream.hpp"
#include "prx/libkernel/KernelErrors.hpp"
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <link.h>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

static void Require(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "Patched guest file runtime: %s\n", message);
        std::exit(1);
    }
}

static void CheckBinding(void* address, const char* symbol, const char* expectedPath) {
    Dl_info info{};
    Require(address != nullptr && ::dladdr(address, &info) != 0 && info.dli_fname != nullptr,
        "a runtime symbol must have an actual loaded owner");
    char actual[PATH_MAX], expected[PATH_MAX];
    Require(::realpath(info.dli_fname, actual) && ::realpath(expectedPath, expected), "runtime owner paths must resolve");
    std::printf("Binding %s: %s\n", symbol, actual);
    Require(std::strcmp(actual, expected) == 0, "runtime symbols must come from the exact patched image");
}

template <typename TFunction>
static TFunction Resolve(void* handle, const char* symbol, const char* expectedPath) {
    const auto nid = Nid::ComputeNid(symbol, "");
    ::dlerror();
    void* address = ::dlsym(handle, nid.c_str());
    const char* error = ::dlerror();
    if (error) std::fprintf(stderr, "NID lookup %s (%s): %s\n", symbol, nid.c_str(), error);
    Require(error == nullptr && address != nullptr, "the real patched NID export must resolve");
    CheckBinding(address, symbol, expectedPath);
    return reinterpret_cast<TFunction>(address);
}

static int RejectUnpatched(struct dl_phdr_info* info, std::size_t, void*) {
    Require(info->dlpi_name == nullptr || std::strstr(info->dlpi_name, "/unpatched/") == nullptr,
        "the loader must not fall back to an unpatched runtime image");
    return 0;
}

struct Api {
    int (*Socket)(int, int, int);
    int (*Open)(const char*, int, std::uint16_t);
    int (*Close)(int);
    std::int64_t (*Read)(int, void*, std::size_t);
    std::int64_t (*Lseek)(int, std::int64_t, int);
    int (*PosixOpen)(const char*, int, int);
    int (*PosixClose)(int);
    std::int64_t (*PosixRead)(int, void*, std::uint64_t);
    std::int64_t (*PosixLseek)(int, std::int64_t, int);
    FileStream* (*Fdopen)(int, const char*);
    int (*Fileno)(FileStream*);
    std::size_t (*Fread)(void*, std::size_t, std::size_t, FileStream*);
    int (*Fclose)(FileStream*);
    int* (*Error)();
};

class Seed {
public:
    std::string path;

    explicit Seed(const char* bytes) {
        char name[] = "anyps5-patched-file-XXXXXX";
        const int descriptor = ::mkstemp(name);
        Require(descriptor >= 0, "private native seed must open");
        const auto length = std::strlen(bytes);
        Require(::write(descriptor, bytes, length) == static_cast<ssize_t>(length) && ::close(descriptor) == 0,
            "the actual seed bytes must persist before guest IO");
        path = name;
    }

    ~Seed() { Require(::unlink(path.c_str()) == 0, "fixture persisted files must be removed"); }
};

static std::vector<int> NativeOwners(const char* path) {
    struct stat expected{};
    Require(::stat(path, &expected) == 0, "the real persisted inode must be stat-able");
    DIR* directory = ::opendir("/proc/self/fd");
    Require(directory != nullptr, "the fixture's native descriptor inventory must open");
    std::vector<int> owners;
    while (auto* entry = ::readdir(directory)) {
        char* end = nullptr;
        const auto value = std::strtol(entry->d_name, &end, 10);
        if (end == entry->d_name || *end != '\0' || value < 0 || value > INT32_MAX) continue;
        struct stat actual{};
        if (::fstat(static_cast<int>(value), &actual) == 0 && actual.st_dev == expected.st_dev && actual.st_ino == expected.st_ino)
            owners.push_back(static_cast<int>(value));
    }
    Require(::closedir(directory) == 0, "the fixture inventory observer must close");
    return owners;
}

static void CheckMaps(int descriptor, const struct stat& expected) {
    struct stat actual{};
    std::array<char, 64> bytes{};
    Require(::fstat(descriptor, &actual) == 0 && actual.st_dev == expected.st_dev && actual.st_ino == expected.st_ino &&
        ::pread(descriptor, bytes.data(), bytes.size(), 0) > 0, "the reused private maps descriptor must retain its inode and readable bytes");
}

static void CheckSdkAndPrivateReuse(const Api& api, const char* path) {
    const int guest = api.Open(path, 0, 0);
    Require(guest >= 3 && guest <= 32767, "patched SDK open must return an ordinary logical ID");
    const auto owners = NativeOwners(path);
    Require(owners.size() == 1, "patched SDK ownership must refer to exactly one real native payload descriptor");
    const int native = owners.front();
    constexpr std::int64_t distant = 0x180020009LL;
    Require(api.Lseek(guest, distant, SEEK_SET) == distant && api.Lseek(guest, 0, SEEK_SET) == 0,
        "patched SDK seek must preserve successful full64 results");
    std::array<char, 15> bytes{};
    Require(api.Read(guest, bytes.data(), bytes.size()) == 15 && std::memcmp(bytes.data(), "patched-payload", 15) == 0,
        "patched SDK read must return the actual persisted payload");
    Require(api.Lseek(guest, 0, SEEK_CUR) == 15 && api.PosixClose(guest) == 0, "the real file position and cross-API close must work");
    Require(NativeOwners(path).empty() && ::fcntl(native, F_GETFD) == -1 && errno == EBADF,
        "logical close must release its real native ownership");
    const int opened = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    Require(opened >= 0, "a private native maps reader must open");
    if (opened != native) {
        Require(::fcntl(native, F_GETFD) == -1 && errno == EBADF, "controlled native fd reuse must not replace another owner");
        Require(::dup2(opened, native) == native && ::close(opened) == 0, "the private maps reader must reuse the retired native number");
    }
    struct stat maps{};
    Require(::fstat(native, &maps) == 0, "the private maps inode must be captured");
    Require(api.Close(guest) == SCE_KERNEL_ERROR_EBADF && api.PosixClose(guest) == -1 && *api.Error() == 9,
        "stale patched SDK and POSIX close must preserve private host ownership");
    std::array<char, 4> untouched = {'x', 'x', 'x', 'x'};
    Require(api.Read(guest, untouched.data(), untouched.size()) == SCE_KERNEL_ERROR_EBADF &&
        untouched == std::array<char, 4>{'x', 'x', 'x', 'x'}, "stale patched SDK read must preserve its output and private host fd");
    Require(api.Lseek(guest, 0, SEEK_SET) == SCE_KERNEL_ERROR_EBADF && api.Fdopen(native, "rb") == nullptr && *api.Error() == 9,
        "a private native fd number must not become a guest file");
    CheckMaps(native, maps);
    const int replacement = api.Open(path, 0, 0);
    Require(replacement == guest, "a NEW patched guest open must reuse the logical ID independently of host fd reuse");
    bytes.fill(0);
    Require(api.Read(replacement, bytes.data(), bytes.size()) == 15 && std::memcmp(bytes.data(), "patched-payload", 15) == 0 &&
        api.Close(replacement) == 0, "the reused logical ID must read its own actual payload and close normally");
    CheckMaps(native, maps);
    Require(::close(native) == 0, "the fixture must close its private maps reader");
}

static void CheckPosix(const Api& api, const char* path) {
    const int guest = api.PosixOpen(path, 0, 0);
    Require(guest >= 3 && guest <= 32767, "patched POSIX open must return an ordinary logical ID");
    Require(api.PosixLseek(guest, 0x180020009LL, SEEK_SET) == 0x180020009LL && api.PosixLseek(guest, 3, SEEK_SET) == 3,
        "patched POSIX seek must preserve full64 success and actual position");
    std::array<char, 12> bytes{};
    Require(api.PosixRead(guest, bytes.data(), bytes.size()) == 12 && std::memcmp(bytes.data(), "ched-payload", 12) == 0,
        "patched POSIX read must use the exact requested real file offset");
    Require(api.PosixClose(guest) == 0 && api.PosixLseek(guest, 0, SEEK_SET) == -1 && *api.Error() == 9,
        "patched POSIX closed seek must return minus one and guest EBADF");
}

static void CheckFdopen(const Api& api, const char* oldPath, const char* replacementPath) {
    const int guest = api.Open(oldPath, 0, 0);
    Require(guest >= 3 && guest <= 32767, "fdopen source must be a real ordinary guest ID");
    FileStream* stream = api.Fdopen(guest, "rb");
    Require(stream != nullptr && api.Fileno(stream) == guest, "patched fdopen must retain the existing logical descriptor");
    std::int16_t prefix = -1;
    std::memcpy(&prefix, reinterpret_cast<const unsigned char*>(stream) + offsetof(GuestFilePrefix, descriptor), sizeof(prefix));
    Require(prefix == guest, "the actual signed16 FILE prefix must contain the logical descriptor");
    std::array<char, 7> start{};
    Require(api.Fread(start.data(), 1, start.size(), stream) == 7 && std::memcmp(start.data(), "patched", 7) == 0,
        "patched fdopen must read the actual persisted payload");
    Require(api.Close(guest) == 0 && NativeOwners(oldPath).size() == 1, "retiring the logical entry must leave only its private FILE ownership");
    const int replacement = api.Open(replacementPath, 0, 0);
    Require(replacement == guest, "a NEW guest open must reuse the retired FILE's logical ID");
    std::array<char, 2> first{};
    Require(api.Read(replacement, first.data(), first.size()) == 2 && first[0] == 'r' && first[1] == 'e',
        "the replacement inode must have its own real contents and position");
    std::array<char, 4> untouched = {'x', 'x', 'x', 'x'};
    Require(api.Fread(untouched.data(), 1, untouched.size(), stream) == 0 && *api.Error() == 9 &&
        untouched == std::array<char, 4>{'x', 'x', 'x', 'x'}, "stale patched FILE IO must reject its old identity without reading the replacement");
    Require(api.Fileno(stream) == -1 && *api.Error() == 9 && api.Fclose(stream) == EOF && *api.Error() == 9,
        "stale patched FILE cleanup must report EBADF and release only its private FILE");
    Require(NativeOwners(oldPath).empty() && api.Lseek(replacement, 0, SEEK_CUR) == 2,
        "old FILE cleanup must release the old inode and preserve the replacement position");
    std::array<char, 9> rest{};
    Require(api.Read(replacement, rest.data(), rest.size()) == 9 && std::memcmp(rest.data(), "placement", 9) == 0 &&
        api.Close(replacement) == 0 && NativeOwners(replacementPath).empty(), "replacement contents and ownership must survive stale FILE cleanup");
}

int main(int argc, char** argv) {
    Require(argc == 3 || argc == 4, "exact patched libc and kernel paths and optional WebKit path are required");
    const rlimit core{0, 0};
    Require(::setrlimit(RLIMIT_CORE, &core) == 0, "the bounded runtime fixture must disable large core exports");
    void* libc = ::dlopen(argv[1], RTLD_NOW | RTLD_NOLOAD | RTLD_GLOBAL);
    Require(libc != nullptr, "the exact patched libc must already be loaded through DT_NEEDED");
    Api api{};
    api.Error = Resolve<decltype(api.Error)>(libc, "__error", argv[1]);
    ::dl_iterate_phdr(RejectUnpatched, nullptr);
    ::dlerror();
    void* kernel = ::dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
    const char* loaderError = ::dlerror();
    if (loaderError) std::fprintf(stderr, "Patched kernel RTLD_NOW: %s\n", loaderError);
    Require(kernel != nullptr && loaderError == nullptr, "all native imports of the actual patched kernel must resolve eagerly");
    ::dl_iterate_phdr(RejectUnpatched, nullptr);
    api.Socket = Resolve<decltype(api.Socket)>(kernel, "socket", argv[2]);
    api.Open = Resolve<decltype(api.Open)>(kernel, "sceKernelOpen", argv[2]);
    api.Close = Resolve<decltype(api.Close)>(kernel, "sceKernelClose", argv[2]);
    api.Read = Resolve<decltype(api.Read)>(kernel, "sceKernelRead", argv[2]);
    api.Lseek = Resolve<decltype(api.Lseek)>(kernel, "sceKernelLseek", argv[2]);
    api.PosixOpen = Resolve<decltype(api.PosixOpen)>(kernel, "open", argv[2]);
    api.PosixClose = Resolve<decltype(api.PosixClose)>(kernel, "close", argv[2]);
    api.PosixRead = Resolve<decltype(api.PosixRead)>(kernel, "read", argv[2]);
    api.PosixLseek = Resolve<decltype(api.PosixLseek)>(kernel, "lseek", argv[2]);
    api.Fdopen = Resolve<decltype(api.Fdopen)>(libc, "fdopen", argv[1]);
    api.Fileno = Resolve<decltype(api.Fileno)>(libc, "fileno", argv[1]);
    api.Fread = Resolve<decltype(api.Fread)>(libc, "fread", argv[1]);
    api.Fclose = Resolve<decltype(api.Fclose)>(libc, "fclose", argv[1]);
    const char* helpers[] = {
        "GuestFileInitializeStandards_nid_no_patch", "GuestFileAcquire_nid_no_patch", "GuestFileAdoptOwned_nid_no_patch",
        "GuestFileAdoptPairOwned_nid_no_patch", "GuestFileReplaceOwnedMatching_nid_no_patch", "GuestFileClose_nid_no_patch",
        "GuestFileCloseMatching_nid_no_patch", "GuestFileMatches_nid_no_patch", "GuestFileNativeDescriptor_nid_no_patch",
        "GuestFileLogicalDescriptor_nid_no_patch", "GuestFileAccessMode_nid_no_patch", "GuestFileDuplicateNative_nid_no_patch",
        "GuestFileNativeError_nid_no_patch"
    };
    for (const char* helper : helpers) CheckBinding(::dlsym(libc, helper), helper, argv[1]);
    for (const char* helper : {"GuestSocketClose_nid_no_patch", "GuestSocketIsOpen_nid_no_patch"})
        CheckBinding(::dlsym(kernel, helper), helper, argv[2]);
    void* webkit = nullptr;
    int (*isatty)(int) = nullptr;
    if (argc == 4) {
        ::dlerror();
        webkit = ::dlopen(argv[3], RTLD_NOW | RTLD_LOCAL);
        const char* error = ::dlerror();
        if (error) std::fprintf(stderr, "Patched WebKit RTLD_NOW: %s\n", error);
        Require(webkit != nullptr && error == nullptr, "all actual patched WebKit native imports must resolve eagerly");
        isatty = Resolve<decltype(isatty)>(webkit, "isatty", argv[3]);
        ::dl_iterate_phdr(RejectUnpatched, nullptr);
    }
    std::fflush(stdout);
    {
        Seed payload("patched-payload");
        Seed replacement("replacement");
        CheckSdkAndPrivateReuse(api, payload.path.c_str());
        CheckPosix(api, payload.path.c_str());
        CheckFdopen(api, payload.path.c_str(), replacement.path.c_str());
        if (isatty) {
            const int guest = api.Open(payload.path.c_str(), 0, 0);
            Require(guest >= 3 && isatty(guest) == 0 && *api.Error() == 25, "patched WebKit must classify a real guest file as ENOTTY");
            Require(api.Close(guest) == 0 && isatty(guest) == 0 && *api.Error() == 9,
                "patched WebKit must reject the retired logical ID with guest EBADF");
            const int socket = api.Socket(2, 1, 0);
            Require(socket >= 0x10000000 && isatty(socket) == 0 && *api.Error() == 25,
                "patched WebKit must classify an actual live guest socket as ENOTTY");
            Require(api.PosixClose(socket) == 0 && isatty(socket) == 0 && *api.Error() == 9 &&
                api.Close(socket) == SCE_KERNEL_ERROR_EBADF,
                "patched socket close and WebKit must agree on the retired socket identity");
        }
    }
    if (webkit) Require(::dlclose(webkit) == 0, "the fixture must release its explicit WebKit loader reference");
    Require(::dlclose(kernel) == 0 && ::dlclose(libc) == 0, "the fixture must release its explicit loader references");
    std::puts("patched guest file runtime tests passed");
}
