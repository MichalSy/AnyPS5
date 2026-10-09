#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libc/include/ApplicationHeap.hpp"
#include "prx/libc/include/HeapDiagnostics.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

extern "C" void APS5_VABI sceKernelSetThreadDtors(thread_dtors_func_t dtors);
extern "C" int APS5_VABI sceKernelGetModuleInfoFromAddr(std::uint64_t address, int flags, ModuleInfoEx* info);
extern "C" const char* __progname_nid_postfix;
extern "C" int* APS5_VABI __error_nid_postfix();
extern "C" char* APS5_VABI strerror_nid_postfix(int error);
extern "C" int APS5_VABI vsnprintf_nid_postfix(char* buffer, std::size_t size, const char* format, VaList* arguments);

namespace {

using ThreadDestructorFunction = void (APS5_VABI*)(void*);

struct ThreadDestructor {
    ThreadDestructorFunction function;
    void* object;
    void* dsoSymbol;
};

struct ThreadDestructorsTag {};

std::vector<ThreadDestructor>& ThreadDestructors() {
    return HostThreadLocal<std::vector<ThreadDestructor>, ThreadDestructorsTag>();
}

bool IsInLoadedImage(const void* address) {
    if (address == nullptr)
        return false;
#ifdef _WIN32
    HMODULE module = nullptr;
    return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(address), &module) != 0;
#else
    Dl_info info{};
    return dladdr(address, &info) != 0;
#endif
}

void CallThreadDestructor(const ThreadDestructor& destructor) {
    const auto* function = reinterpret_cast<const void*>(destructor.function);
    if (!IsInLoadedImage(function)) {
        std::ostringstream message;
        message << "thread_local destructor " << function << " of dso " << destructor.dsoSymbol << " is not in a loaded image";
        throw std::runtime_error(message.str());
    }
    destructor.function(destructor.object);
}

void APS5_VABI RunThreadDestructors_nid_no_patch() {
    auto& destructors = ThreadDestructors();
    while (!destructors.empty()) {
        const ThreadDestructor destructor = destructors.back();
        destructors.pop_back();
        CallThreadDestructor(destructor);
    }
}

bool FindDsoModule(const void* dsoSymbol, KernelModule& handle) {
    ModuleInfoEx info{};
    info.st_size = sizeof(ModuleInfoEx);
    if (sceKernelGetModuleInfoFromAddr(reinterpret_cast<std::uintptr_t>(dsoSymbol), 2, &info) != 0) {
        handle = 0;
        return false;
    }
    handle = info.id;
    return true;
}

bool ForceThreadDestructorPass(KernelModule handle) {
    auto& destructors = ThreadDestructors();
    bool found = false;
    for (std::size_t index = destructors.size(); index-- > 0;) {
        const ThreadDestructor destructor = destructors[index];
        KernelModule module = 0;
        const bool loaded = FindDsoModule(destructor.dsoSymbol, module);
        if (module != handle)
            continue;
        found = true;
        destructors.erase(destructors.begin() + static_cast<std::ptrdiff_t>(index));
        if (loaded && *static_cast<void* const*>(destructor.dsoSymbol) == destructor.dsoSymbol)
            CallThreadDestructor(destructor);
    }
    return found;
}

void RegisterThreadExitHook() {
    [[maybe_unused]] static const bool registered = [] {
        sceKernelSetThreadDtors(RunThreadDestructors_nid_no_patch);
        return true;
    }();
}

std::string SyslogFormat(const char* format, int error) {
    std::string result;
    for (const char* cursor = format; *cursor != '\0'; ++cursor) {
        if (*cursor == '%' && cursor[1] == '%') {
            result += "%%";
            ++cursor;
        } else if (*cursor == '%' && cursor[1] == 'm') {
            for (const char* text = strerror_nid_postfix(error); *text != '\0'; ++text) {
                if (*text == '%') result += '%';
                result += *text;
            }
            ++cursor;
        } else {
            result += *cursor;
        }
    }
    return result;
}

}

extern "C" {

int Need_sceLibcInternal_nid_postfix = 1;

const char* APS5_VABI getprogname_nid_postfix() {
    return __progname_nid_postfix;
}

std::size_t APS5_VABI malloc_usable_size_nid_postfix(const void* pointer) {
    return ApplicationHeapUsableSize_nid_no_patch(pointer);
}

void APS5_VABI syslog_nid_postfix(int priority, const char* format, ...) {
    const int savedError = *__error_nid_postfix();
    if (!format) return;
#ifdef _WIN32
    __builtin_sysv_va_list arguments;
    __builtin_sysv_va_start(arguments, format);
#else
    std::va_list arguments;
    va_start(arguments, format);
#endif
    try {
        const auto expanded = SyslogFormat(format, savedError);
        char buffer[4096]{};
        const int count = vsnprintf_nid_postfix(buffer, sizeof(buffer), expanded.c_str(), reinterpret_cast<VaList*>(arguments));
        if (count >= 0) {
            static std::mutex outputMutex;
            std::lock_guard lock(outputMutex);
            std::fprintf(stderr, "[guest syslog %d] %s", priority, buffer);
            const auto length = std::strlen(buffer);
            if (length == 0 || buffer[length - 1] != '\n') std::fputc('\n', stderr);
        }
    } catch (...) {
    }
#ifdef _WIN32
    __builtin_sysv_va_end(arguments);
#else
    va_end(arguments);
#endif
    *__error_nid_postfix() = savedError;
}

void APS5_VABI __cxa_finalize_nid_postfix(void* dsoHandle) {
    CxaFinalize_nid_no_patch(dsoHandle);
}

void APS5_VABI sceLibcHeapGetTraceInfo_nid_postfix(Info* info) {
    LibcHeapTraceInfo_nid_no_patch(info);
}

int APS5_VABI _sceLibcInternalThreadAtexit_nid_postfix(ThreadDestructorFunction destructor, void* object, void* dsoSymbol) {
    RegisterThreadExitHook();
    ThreadDestructors().push_back({destructor, object, dsoSymbol});
    return 0;
}

void APS5_VABI _sceLibcInternalThreadDtors_nid_postfix() {
    RunThreadDestructors_nid_no_patch();
}

int APS5_VABI _sceLibcInternalForceTlsDestructor_nid_postfix(KernelModule handle) {
    for (int pass = 0; pass < 4; ++pass) {
        if (!ForceThreadDestructorPass(handle))
            break;
    }
    return 0;
}

}
