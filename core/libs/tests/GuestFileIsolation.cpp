#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libkernel/KernelErrors.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#ifdef _WIN32
#include <io.h>
#ifdef ANYPS5_PATCHED_FILE_TEST
#include <windows.h>
#endif
#else
#include <unistd.h>
#ifdef ANYPS5_PATCHED_FILE_TEST
#include <dlfcn.h>
#endif
#endif

#ifdef ANYPS5_PATCHED_FILE_TEST
#include "nid/NidCompute.hpp"
#endif

class FileStream;

extern "C" {
int APS5_VABI sceKernelOpen(const char*, int, std::uint16_t);
int APS5_VABI sceKernelClose(int);
std::int64_t APS5_VABI sceKernelRead(int, void*, std::size_t);
std::int64_t APS5_VABI sceKernelLseek(int, std::int64_t, int);
int APS5_VABI close_nid_postfix(int);
int* APS5_VABI __error_nid_postfix();
FileStream* APS5_VABI fopen_nid_postfix(const char*, const char*);
FileStream* APS5_VABI fdopen_nid_postfix(int, const char*);
int APS5_VABI fileno_nid_postfix(FileStream*);
std::size_t APS5_VABI fread_nid_postfix(void*, std::size_t, std::size_t, FileStream*);
int APS5_VABI fclose_nid_postfix(FileStream*);
}

struct Api {
    decltype(&sceKernelOpen) Open;
    decltype(&sceKernelClose) Close;
    decltype(&sceKernelRead) Read;
    decltype(&sceKernelLseek) Lseek;
    decltype(&close_nid_postfix) PosixClose;
    decltype(&__error_nid_postfix) Error;
    decltype(&fopen_nid_postfix) Fopen;
    decltype(&fdopen_nid_postfix) Fdopen;
    decltype(&fileno_nid_postfix) Fileno;
    decltype(&fread_nid_postfix) Fread;
    decltype(&fclose_nid_postfix) Fclose;
};

static Api api;

static void Require(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "Guest file isolation: %s\n", message);
        std::exit(1);
    }
}

#ifdef ANYPS5_PATCHED_FILE_TEST
template <typename TFunction>
static TFunction Resolve(void* module, const char* symbol) {
    const auto nid = Nid::ComputeNid(symbol, "");
#ifdef _WIN32
    auto* address = ::GetProcAddress(static_cast<HMODULE>(module), nid.c_str());
    HMODULE owner = nullptr;
    Require(address != nullptr && ::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<const char*>(address), &owner) && owner == module,
        "guest export must belong to the requested patched module");
#else
    ::dlerror();
    auto* address = ::dlsym(module, nid.c_str());
    Require(address != nullptr && ::dlerror() == nullptr, "patched guest export must resolve");
#endif
    return reinterpret_cast<TFunction>(address);
}
#endif

static int NativeDescriptor(std::FILE* stream) {
#ifdef _WIN32
    return ::_fileno(stream);
#else
    return ::fileno(stream);
#endif
}

static void CheckPrivateHostFile() {
    auto* host = std::tmpfile();
    Require(host != nullptr, "private host file must open");
    Require(std::fwrite("private", 1, 7, host) == 7 && std::fflush(host) == 0, "host payload must persist");
    const int native = NativeDescriptor(host);
    Require(native >= 3, "fixture must use a non-standard host descriptor");
    Require(api.Close(native) == SCE_KERNEL_ERROR_EBADF, "guest close must refuse a private host descriptor");
    Require(api.PosixClose(native) == -1 && *api.Error() == 9, "POSIX close must refuse a private host descriptor");
    std::array<char, 7> bytes{};
    bytes.fill('!');
    Require(api.Read(native, bytes.data(), bytes.size()) == SCE_KERNEL_ERROR_EBADF,
        "guest read must refuse a private host descriptor");
    for (const char byte : bytes) Require(byte == '!', "refused read must preserve the destination");
    Require(api.Lseek(native, 0, SEEK_SET) == SCE_KERNEL_ERROR_EBADF,
        "guest seek must refuse a private host descriptor");
    std::rewind(host);
    Require(std::fread(bytes.data(), 1, bytes.size(), host) == bytes.size() &&
        std::memcmp(bytes.data(), "private", bytes.size()) == 0, "private host ownership and bytes must survive");
    Require(std::fclose(host) == 0, "host owner must close its own file");
}

static void CheckStaleStream(const std::string& original, const std::string& replacement, bool attach) {
    FileStream* stream;
    int guest;
    if (attach) {
        guest = api.Open(original.c_str(), 0, 0);
        Require(guest >= 3, "SDK file must open");
        stream = api.Fdopen(guest, "rb");
    } else {
        stream = api.Fopen(original.c_str(), "rb");
        Require(stream != nullptr, "guest stream must open");
        guest = api.Fileno(stream);
    }
    Require(stream != nullptr && guest >= 3 && guest <= 32767 && api.Fileno(stream) == guest,
        "stream and kernel must share one logical descriptor");
    std::array<char, 7> bytes{};
    Require(api.Read(guest, bytes.data(), bytes.size()) == bytes.size() &&
        std::memcmp(bytes.data(), "payload", bytes.size()) == 0, "logical descriptor must reach its own file");
    Require(api.Close(guest) == 0, "logical close must succeed");
    const int next = api.Open(replacement.c_str(), 0, 0);
    Require(next == guest, "closed logical slot must be reusable");
    bytes.fill('!');
    Require(api.Fread(bytes.data(), 1, bytes.size(), stream) == 0 && *api.Error() == 9,
        "stale stream must refuse the reused logical slot");
    for (const char byte : bytes) Require(byte == '!', "stale stream must preserve the destination");
    Require(api.Fileno(stream) == -1 && *api.Error() == 9, "stale fileno must report EBADF");
    Require(api.Fclose(stream) == EOF && *api.Error() == 9,
        "stale stream close must retire only its own native ownership");
    Require(api.Read(next, bytes.data(), bytes.size()) == bytes.size() &&
        std::memcmp(bytes.data(), "replace", bytes.size()) == 0, "replacement must survive stale stream closure");
    Require(api.Close(next) == 0, "replacement descriptor must close");
}

int main(int argc, char** argv) {
#ifdef ANYPS5_PATCHED_FILE_TEST
    Require(argc == 3, "patched libc and kernel paths must be supplied");
#ifdef _WIN32
    Require(::SetDllDirectoryW(std::filesystem::path(argv[1]).parent_path().wstring().c_str()), "patched dependency directory must be selected");
    void* libc = ::LoadLibraryW(std::filesystem::path(argv[1]).wstring().c_str());
    void* kernel = ::LoadLibraryW(std::filesystem::path(argv[2]).wstring().c_str());
#else
    void* libc = ::dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
    void* kernel = ::dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
#endif
    Require(libc != nullptr && kernel != nullptr, "actual patched libraries must load eagerly");
    api = {
        Resolve<decltype(api.Open)>(kernel, "sceKernelOpen"),
        Resolve<decltype(api.Close)>(kernel, "sceKernelClose"),
        Resolve<decltype(api.Read)>(kernel, "sceKernelRead"),
        Resolve<decltype(api.Lseek)>(kernel, "sceKernelLseek"),
        Resolve<decltype(api.PosixClose)>(kernel, "close"),
        Resolve<decltype(api.Error)>(libc, "__error"),
        Resolve<decltype(api.Fopen)>(libc, "fopen"),
        Resolve<decltype(api.Fdopen)>(libc, "fdopen"),
        Resolve<decltype(api.Fileno)>(libc, "fileno"),
        Resolve<decltype(api.Fread)>(libc, "fread"),
        Resolve<decltype(api.Fclose)>(libc, "fclose")
    };
#else
    static_cast<void>(argc);
    static_cast<void>(argv);
    api = {sceKernelOpen, sceKernelClose, sceKernelRead, sceKernelLseek, close_nid_postfix, __error_nid_postfix, fopen_nid_postfix, fdopen_nid_postfix, fileno_nid_postfix, fread_nid_postfix, fclose_nid_postfix};
#endif
    CheckPrivateHostFile();
    const auto name = "anyps5-file-isolation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root = std::filesystem::current_path() / name;
    Require(std::filesystem::create_directory(root), "private fixture directory must be created");
    const auto original = (std::filesystem::path(name) / "original").generic_string();
    const auto replacement = (std::filesystem::path(name) / "replacement").generic_string();
    { std::ofstream file(root / "original", std::ios::binary); file << "payload"; Require(file.good(), "original seed must persist"); }
    { std::ofstream file(root / "replacement", std::ios::binary); file << "replace"; Require(file.good(), "replacement seed must persist"); }
    CheckStaleStream(original, replacement, false);
    CheckStaleStream(original, replacement, true);
    Require(std::filesystem::remove(root / "original") && std::filesystem::remove(root / "replacement") &&
        std::filesystem::remove(root), "closed fixtures must be removable");
#ifdef ANYPS5_PATCHED_FILE_TEST
#ifdef _WIN32
    Require(::FreeLibrary(static_cast<HMODULE>(kernel)) && ::FreeLibrary(static_cast<HMODULE>(libc)), "patched modules must unload");
#else
    Require(::dlclose(kernel) == 0 && ::dlclose(libc) == 0, "patched modules must unload");
#endif
#endif
    std::puts("guest file isolation tests passed");
}
