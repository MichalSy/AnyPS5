#include "prx/libc/include/general/VabiMacros.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

extern "C" {
int APS5_VABI sceSysmoduleIsLoaded(std::uint16_t);
int APS5_VABI sceSysmoduleLoadModule(std::uint16_t);
int APS5_VABI sceSysmoduleUnloadModule(std::uint16_t);
int APS5_VABI sceSysmoduleLoadModuleInternal(std::uint32_t);
int APS5_VABI sceSysmoduleLoadModuleInternalWithArg(std::uint32_t, int, void*, std::uint64_t, int*);
int APS5_VABI sceSysmoduleUnloadModuleInternal(std::uint32_t);
}

namespace {

constexpr std::uint16_t SaveDataDialog = 0x00a0;
constexpr std::uint16_t Fiber = 0x0006;
constexpr std::uint16_t Unknown = 0x7000;
constexpr int NotLoaded = static_cast<int>(0x80a90002u);
constexpr int UnloadNotLoaded = static_cast<int>(0x80a90003u);

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template<class TAction>
void RequireThrows(TAction action, const char* message) {
    try {
        action();
    } catch (const std::runtime_error&) {
        return;
    }
    throw std::runtime_error(message);
}

void CheckOtherIds() {
    Require(sceSysmoduleUnloadModule(Fiber) == UnloadNotLoaded,
            "a known other module must retain its initial unload error");
    Require(sceSysmoduleUnloadModuleInternal(Fiber) == UnloadNotLoaded,
            "an internal unload of another module must stay strict");
    Require(sceSysmoduleIsLoaded(Fiber) == NotLoaded, "another module must remain unloaded");
    Require(sceSysmoduleLoadModule(Fiber) == 0, "other module first load failed");
    Require(sceSysmoduleLoadModule(Fiber) == 0, "other module second load failed");
    Require(sceSysmoduleUnloadModule(Fiber) == 0, "other module first release failed");
    Require(sceSysmoduleIsLoaded(Fiber) == 0, "other module lost its remaining reference");
    Require(sceSysmoduleUnloadModule(Fiber) == 0, "other module final release failed");
    Require(sceSysmoduleIsLoaded(Fiber) == NotLoaded, "other module final release retained a reference");
    Require(sceSysmoduleUnloadModule(Fiber) == UnloadNotLoaded,
            "other module duplicate release must remain an error");
    Require(sceSysmoduleUnloadModule(Unknown) == 0,
            "unknown public unload must preserve the existing behavior");
    Require(sceSysmoduleLoadModule(Unknown) == 0,
            "unknown public load must preserve the existing behavior");
    RequireThrows([] { sceSysmoduleUnloadModule(0); }, "zero public unload id must remain invalid");
    RequireThrows([] { sceSysmoduleLoadModule(0); }, "zero public load id must remain invalid");
    RequireThrows([] { sceSysmoduleUnloadModuleInternal(0); }, "zero internal unload id must remain invalid");
    RequireThrows([] { sceSysmoduleLoadModuleInternal(0); }, "zero internal load id must remain invalid");
    RequireThrows([] { sceSysmoduleUnloadModuleInternal(Unknown); },
                  "unknown internal unload id must retain its error");
}

void CheckBalancedSessions() {
    Require(sceSysmoduleLoadModule(SaveDataDialog) == 0, "real dialog first load failed");
    Require(sceSysmoduleLoadModule(SaveDataDialog) == 0, "real dialog second load failed");
    Require(sceSysmoduleIsLoaded(SaveDataDialog) == 0, "real dialog loads were not counted");
    Require(sceSysmoduleUnloadModule(SaveDataDialog) == 0, "dialog count2-to-count1 release failed");
    Require(sceSysmoduleIsLoaded(SaveDataDialog) == 0, "dialog remaining reference was lost");
    Require(sceSysmoduleUnloadModule(SaveDataDialog) == 0, "dialog count1-to-count0 release failed");
    Require(sceSysmoduleIsLoaded(SaveDataDialog) == NotLoaded, "dialog final release retained a reference");
    Require(sceSysmoduleUnloadModule(SaveDataDialog) == UnloadNotLoaded,
            "dialog double unload after a real load must remain an error");
    Require(sceSysmoduleUnloadModule(SaveDataDialog) == UnloadNotLoaded,
            "the initial allowance must stay closed after real loads");
    Require(sceSysmoduleUnloadModuleInternal(SaveDataDialog) == UnloadNotLoaded,
            "internal duplicate dialog unload must remain an error");
    Require(sceSysmoduleLoadModule(SaveDataDialog) == 0, "later real dialog load failed");
    Require(sceSysmoduleIsLoaded(SaveDataDialog) == 0, "later dialog reference is missing");
    Require(sceSysmoduleUnloadModule(SaveDataDialog) == 0, "later real dialog unload failed");
    Require(sceSysmoduleUnloadModule(SaveDataDialog) == UnloadNotLoaded,
            "later sessions must not reopen the initial allowance");
}

void CheckRealLoadClosesAllowance(const std::string& mode) {
    int result = -1;
    if (mode == "--load-first") {
        result = sceSysmoduleLoadModule(SaveDataDialog);
    } else if (mode == "--internal-load-first") {
        result = sceSysmoduleLoadModuleInternal(SaveDataDialog);
    } else {
        int returned = 321;
        result = sceSysmoduleLoadModuleInternalWithArg(SaveDataDialog, 0, nullptr, 0, &returned);
        Require(returned == 0, "real internal-with-args load must set its result");
    }
    Require(result == 0, "first real dialog load failed");
    Require(sceSysmoduleIsLoaded(SaveDataDialog) == 0, "first real load was not counted");
    Require(sceSysmoduleUnloadModule(SaveDataDialog) == 0, "first real dialog unload failed");
    Require(sceSysmoduleIsLoaded(SaveDataDialog) == NotLoaded, "first real release retained a reference");
    Require(sceSysmoduleUnloadModule(SaveDataDialog) == UnloadNotLoaded,
            "a real load must close an unused initial allowance");
    Require(sceSysmoduleUnloadModule(SaveDataDialog) == UnloadNotLoaded,
            "a closed initial allowance must not reopen");
    CheckBalancedSessions();
}

}

int main(int argc, char** argv) {
    try {
        Require(argc == 2, "expected one sysmodule cleanup mode");
        const std::string mode = argv[1];
        Require(mode == "--strict" || mode == "--initial-cleanup" || mode == "--load-first" ||
                mode == "--internal-load-first" || mode == "--internal-arg-load-first",
                "unknown sysmodule cleanup mode");
        const auto* value = std::getenv("ANYPS5_SAVEDATA_INITIAL_CLEANUP");
        const bool enabled = value != nullptr && std::strcmp(value, "1") == 0;
        Require(enabled == (mode != "--strict"), "cleanup option does not match the test mode");
        CheckOtherIds();
        Require(sceSysmoduleIsLoaded(SaveDataDialog) == NotLoaded, "dialog starts with an unexpected reference");
        Require(sceSysmoduleUnloadModuleInternal(SaveDataDialog) == UnloadNotLoaded,
                "the allowance must not apply to internal unload");
        if (mode == "--strict" || mode == "--initial-cleanup") {
            Require(sceSysmoduleUnloadModule(SaveDataDialog) == (enabled ? 0 : UnloadNotLoaded),
                    "first pre-load public unload must respect the exact opt-in value");
            Require(sceSysmoduleIsLoaded(SaveDataDialog) == NotLoaded,
                    "initial cleanup must not create a load reference");
            Require(sceSysmoduleUnloadModule(SaveDataDialog) == UnloadNotLoaded,
                    "only one pre-load public unload may succeed");
            CheckBalancedSessions();
        } else {
            CheckRealLoadClosesAllowance(mode);
        }
        std::printf("Sysmodule initial-cleanup checks passed: %s\n", mode.c_str());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Sysmodule initial-cleanup check failed: %s\n", error.what());
        return 1;
    }
}
