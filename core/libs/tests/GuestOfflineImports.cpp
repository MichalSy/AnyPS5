#include "SceTypes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string_view>

extern "C" {
int APS5_VABI sceHttpSetCookieEnabled(int, int);
int APS5_VABI sceHttpSendRequest(int, const void*, std::size_t);
int APS5_VABI sceNpAuthCreateRequest(void);
int APS5_VABI sceSystemGestureAppendTouchRecognizer(std::int32_t, SystemGestureTouchRecognizer*);
int* APS5_VABI __error_nid_postfix();
int APS5_VABI sceNpEntitlementAccessGetEntitlementKey(
    std::uint32_t, const NpUnifiedEntitlementLabel*, NpEntitlementAccessEntitlementKey*);
int APS5_VABI sceRudpInit_nid_postfix(void*, int);
int APS5_VABI sceRudpGetStatus(void*, std::size_t);
int APS5_VABI sceRudpTerminate();
}

static void Require(bool value) { if (!value) std::abort(); }

static void CheckUnsupportedGestureAppend() {
    constexpr int unsupported = static_cast<int>(0x8002002du);
    struct RecognizerBuffer {
        std::uint64_t before;
        SystemGestureTouchRecognizer recognizer;
        std::uint64_t after;
    } output;
    std::memset(&output, 0xa5, sizeof(output));
    std::array<unsigned char, sizeof(output)> original{};
    std::memcpy(original.data(), &output, sizeof(output));
    for (int savedError : {0, 13}) {
        *__error_nid_postfix() = savedError;
        for (std::int32_t handle : {1, 0, -1, 0x7fffffff}) {
            for (int attempt = 0; attempt < 4; ++attempt) {
                const int result = sceSystemGestureAppendTouchRecognizer(handle, &output.recognizer);
                Require(result == unsupported && result < 0);
                Require(std::memcmp(&output, original.data(), sizeof(output)) == 0);
                Require(sceSystemGestureAppendTouchRecognizer(handle, nullptr) == unsupported);
                Require(*__error_nid_postfix() == savedError);
            }
        }
    }
}

int main() {
    CheckUnsupportedGestureAppend();
    constexpr int signedOut = static_cast<int>(0x80550006u);
    static_assert(sizeof(int) == sizeof(std::int32_t));
    for (int savedError : {0, 13}) {
        *__error_nid_postfix() = savedError;
        for (int attempt = 0; attempt < 4; ++attempt) {
            const int result = sceNpAuthCreateRequest();
            Require(result == signedOut && result < 0);
            Require(*__error_nid_postfix() == savedError);
        }
    }

    constexpr int invalidValue = static_cast<int>(0x804311FE);
    constexpr int network = static_cast<int>(0x80431063);
    Require(sceHttpSetCookieEnabled(1, 0) == 0);
    Require(sceHttpSendRequest(1, nullptr, 0) == network);
    bool cookieUnsupported = false;
    try {
        sceHttpSetCookieEnabled(1, 1);
    } catch (const std::runtime_error& error) {
        cookieUnsupported = std::string_view(error.what()) == "sceHttpSetCookieEnabled not implemented";
    }
    Require(cookieUnsupported);
    for (int enabled : {-1, 2, 0x100}) {
        Require(sceHttpSetCookieEnabled(1, enabled) == invalidValue);
    }

    constexpr int parameter = static_cast<int>(0x817D0002);
    constexpr int noEntitlement = static_cast<int>(0x817D0007);
    static_assert(sizeof(NpEntitlementAccessEntitlementKey) == 16);
    NpUnifiedEntitlementLabel label{};
    std::memcpy(label.data, "unowned-addon", 14);
    struct KeyBuffer {
        std::uint64_t before;
        NpEntitlementAccessEntitlementKey key;
        std::uint64_t after;
    } output;
    std::memset(&output, 0xa5, sizeof(output));
    std::array<unsigned char, sizeof(output)> original{};
    std::memcpy(original.data(), &output, sizeof(output));
    Require(sceNpEntitlementAccessGetEntitlementKey(0, nullptr, &output.key) == parameter);
    Require(sceNpEntitlementAccessGetEntitlementKey(0, &label, nullptr) == parameter);
    Require(sceNpEntitlementAccessGetEntitlementKey(0, nullptr, nullptr) == parameter);
    for (std::uint32_t serviceLabel : {0u, 1u, 0xffffffffu}) {
        Require(sceNpEntitlementAccessGetEntitlementKey(serviceLabel, &label, &output.key) == noEntitlement);
        Require(std::memcmp(&output, original.data(), sizeof(output)) == 0);
    }

    std::array<unsigned char, 248> status;
    status.fill(0x5a);
    const auto originalStatus = status;
    auto unsupported = [](void* data, std::size_t size) {
        try {
            sceRudpGetStatus(data, size);
        } catch (const std::runtime_error& error) {
            return std::string_view(error.what()) == "sceRudpGetStatus not implemented";
        }
        return false;
    };
    Require(unsupported(status.data(), status.size()));
    Require(status == originalStatus);
    Require(sceRudpInit_nid_postfix(nullptr, 0) == 0);
    Require(unsupported(status.data(), status.size()));
    Require(status == originalStatus);
    Require(unsupported(nullptr, 0));
    Require(sceRudpTerminate() == 0);
    return 0;
}
