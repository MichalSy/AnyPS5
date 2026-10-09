#include "SceTypes.hpp"
#include "prx/libSceSystemService/SystemService.hpp"
#include "prx/libkernel/KernelErrors.hpp"
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

extern "C" int APS5_VABI sceSystemServiceGetHdrToneMapLuminance(SystemServiceHdrToneMapLuminance* luminance);
extern "C" int APS5_VABI sceSystemServiceParamGetString(int paramId, char* buf, std::size_t bufSize);
extern "C" int APS5_VABI sceSystemServiceLaunchWebBrowser(const char* uri, const void* options);
extern "C" int APS5_VABI sceSystemServiceLoadExec(const char* path, const char* const* arguments);

namespace {

void Require(bool value) { if (!value) std::abort(); }

bool ParamGetStringThrows(int paramId, char* buf, std::size_t bufSize) {
    try {
        static_cast<void>(sceSystemServiceParamGetString(paramId, buf, bufSize));
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

}

extern "C" int APS5_VABI sceSystemServicePowerTick(void);
extern "C" int APS5_VABI sceSystemServiceReportAbnormalTermination(const void* info);
extern "C" int APS5_VABI sceSystemServiceDisableMusicPlayer(void);
extern "C" int APS5_VABI sceSystemServiceReenableMusicPlayer(void);
extern "C" int APS5_VABI sceSystemServiceDisableMediaPlay(void);
extern "C" int APS5_VABI sceSystemServiceReenableMediaPlay(void);

int main() {
    const char* arguments[] = {"+set", "developer", "1", nullptr};
    const char* emptyArguments[] = {nullptr};
    Require(sceSystemServiceLoadExec(nullptr, nullptr) == SYSTEM_SERVICE_ERROR_PARAMETER);
    Require(sceSystemServiceLoadExec("", arguments) == SYSTEM_SERVICE_ERROR_PARAMETER);
    Require(sceSystemServiceLoadExec("/app0/another.bin", nullptr) == SCE_KERNEL_ERROR_EOPNOTSUPP);
    Require(sceSystemServiceLoadExec("/app0/another.bin", arguments) == SCE_KERNEL_ERROR_EOPNOTSUPP);
    Require(sceSystemServiceLoadExec("another.bin", emptyArguments) == SCE_KERNEL_ERROR_EOPNOTSUPP);
    Require(sceSystemServiceLaunchWebBrowser(nullptr, nullptr) == SYSTEM_SERVICE_ERROR_PARAMETER);
    Require(sceSystemServiceLaunchWebBrowser("", nullptr) == SYSTEM_SERVICE_ERROR_PARAMETER);
    Require(sceSystemServiceLaunchWebBrowser("https://example.org", nullptr) == SCE_KERNEL_ERROR_EOPNOTSUPP);
    Require(sceSystemServiceLaunchWebBrowser("https://example.org", reinterpret_cast<const void*>(1)) == SCE_KERNEL_ERROR_EOPNOTSUPP);
    Require(sceSystemServicePowerTick() == SYSTEM_SERVICE_OK);
    Require(sceSystemServicePowerTick() == SYSTEM_SERVICE_OK);
    Require(sceSystemServiceReportAbnormalTermination(nullptr) == SYSTEM_SERVICE_OK);
    int info = 0;
    Require(sceSystemServiceReportAbnormalTermination(&info) == SYSTEM_SERVICE_OK);
    Require(sceSystemServiceDisableMusicPlayer() == SYSTEM_SERVICE_OK);
    Require(sceSystemServiceDisableMusicPlayer() == SYSTEM_SERVICE_OK);
    Require(sceSystemServiceReenableMusicPlayer() == SYSTEM_SERVICE_OK);
    Require(sceSystemServiceReenableMusicPlayer() == SYSTEM_SERVICE_OK);
    Require(sceSystemServiceDisableMediaPlay() == SYSTEM_SERVICE_OK);
    Require(sceSystemServiceDisableMediaPlay() == SYSTEM_SERVICE_OK);
    Require(sceSystemServiceReenableMediaPlay() == SYSTEM_SERVICE_OK);
    Require(sceSystemServiceReenableMediaPlay() == SYSTEM_SERVICE_OK);
    Require(sceSystemServiceGetHdrToneMapLuminance(nullptr) == SYSTEM_SERVICE_ERROR_PARAMETER);
    SystemServiceHdrToneMapLuminance luminance{-1.0f, -1.0f, -1.0f};
    Require(sceSystemServiceGetHdrToneMapLuminance(&luminance) == SYSTEM_SERVICE_OK);
    Require(luminance.max_full_frame_tone_map_luminance == 100.0f);
    Require(luminance.max_tone_map_luminance == 100.0f);
    Require(luminance.min_tone_map_luminance == 0.0f);

    char name[SYSTEM_SERVICE_MAX_SYSTEM_NAME_LENGTH];
    std::memset(name, 'x', sizeof(name));
    Require(sceSystemServiceParamGetString(SYSTEM_SERVICE_PARAM_ID_SYSTEM_NAME, nullptr, sizeof(name)) == SYSTEM_SERVICE_ERROR_PARAMETER);
    Require(sceSystemServiceParamGetString(SYSTEM_SERVICE_PARAM_ID_SYSTEM_NAME, name, 0) == SYSTEM_SERVICE_ERROR_PARAMETER);
    Require(ParamGetStringThrows(SYSTEM_SERVICE_PARAM_ID_SYSTEM_NAME, name, sizeof(name) - 1));
    Require(ParamGetStringThrows(SYSTEM_SERVICE_PARAM_ID_LANG, name, sizeof(name)));
    Require(name[0] == 'x');
    Require(sceSystemServiceParamGetString(SYSTEM_SERVICE_PARAM_ID_SYSTEM_NAME, name, sizeof(name)) == SYSTEM_SERVICE_OK);
    Require(std::strcmp(name, "PS5") == 0);
}
