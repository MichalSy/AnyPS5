#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>

namespace {

using ModuleId = std::int32_t (*)(const void*);
using Open = void* (*)(const char*, int);
using Close = int (*)(void*);

ModuleId moduleId = nullptr;
Close closeModule = nullptr;
void* module = nullptr;
const void* image = nullptr;
std::int32_t expected = 0;

[[noreturn]] void Fail(const char* message) {
    std::fprintf(stderr, "%s\n", message);
    std::_Exit(1);
}

template<typename T>
T Resolve(void* library, const char* name) {
    auto* symbol = dlsym(library, name);
    if (!symbol) Fail(name);
    return reinterpret_cast<T>(symbol);
}

void CheckAfterTeardown() {
    if (moduleId(image) != expected) Fail("module id changed after static teardown");
    if (closeModule(module) != 0) Fail("module cannot be closed after static teardown");
}

}

int main(int argc, char** argv) {
    if (argc != 3) Fail("usage: guest_module_teardown_tests <libkernel> <fixture>");
    if (std::atexit(CheckAfterTeardown) != 0) Fail("atexit failed");
    void* kernel = dlopen(argv[1], RTLD_NOW);
    if (!kernel) Fail(dlerror());
    image = kernel;
    moduleId = Resolve<ModuleId>(kernel, "ModuleIdForImage_nid_no_patch");
    auto open = Resolve<Open>(kernel, "dlopen_nid_postfix");
    closeModule = Resolve<Close>(kernel, "dlclose_nid_postfix");
    module = open(argv[2], 2);
    if (!module) Fail(Resolve<const char* (*)()>(kernel, "dlerror_nid_postfix")());
    expected = moduleId(image);
    return 0;
}
