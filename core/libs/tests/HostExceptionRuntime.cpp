#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <limits.h>
#include <sys/resource.h>

extern "C" int RunHostIoFailure();
extern "C" int RunHostTypedException();

namespace {
bool CheckNativeBinding(const char* name, const char* expectedPath) {
    void* address = ::dlsym(RTLD_DEFAULT, name);
    Dl_info info {};
    if (!address || ::dladdr(address, &info) == 0 || !info.dli_fname) return false;
    char actual[PATH_MAX];
    char expected[PATH_MAX];
    if (!::realpath(info.dli_fname, actual) || !::realpath(expectedPath, expected)) return false;
    std::printf("Binding %s: %s\n", name, actual);
    return std::strcmp(actual, expected) == 0;
}
}

int main(int argc, char** argv) {
    const rlimit core {0, 0};
    if (::setrlimit(RLIMIT_CORE, &core) != 0) return 1;
    if (argc != 3) return 2;
    const char* symbols[] = {"__cxa_allocate_exception", "__cxa_throw", "__cxa_init_primary_exception",
                             "__cxa_begin_catch", "__gxx_personality_v0", "_Unwind_RaiseException",
                             "_ZSt9terminatev"};
    for (const char* name : symbols) {
        if (!CheckNativeBinding(name, argv[2])) {
            std::fprintf(stderr, "Expected patched native libc binding for %s\n", name);
            return 3;
        }
    }
    std::fflush(stdout);
    if (std::strcmp(argv[1], "io") == 0) return RunHostIoFailure();
    if (std::strcmp(argv[1], "typed") == 0) return RunHostTypedException();
    return 4;
}
