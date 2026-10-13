#include "SceTypes.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>

extern "C" {
using GuestRudpEventHandler = void(APS5_VABI*)(int, int, int, void*);
int APS5_VABI sceRudpInit_nid_postfix(void*, int);
int APS5_VABI sceRudpCreateContext(GuestRudpEventHandler, void*, int*);
int APS5_VABI sceRudpTerminate(int);
int APS5_VABI sceRudpEnd();
int APS5_VABI sceRudpGetNumberOfPacketsToRead(int);
int APS5_VABI sceRudpGetSizeWritable(int);
int APS5_VABI sceRudpGetSizeReadable(int);
int APS5_VABI sceRudpEnableInternalIOThread2(uint32_t, uint32_t, uint64_t);
int APS5_VABI sceRudpProcessEvents(uint64_t);
int APS5_VABI sceRudpNetReceived(int, const void*, uint64_t, const void*, uint32_t);
int APS5_VABI sceRudpSetOption(int, int, const void*, uint32_t);
int APS5_VABI sceRudpGetOption(int, int, void*, uint32_t);
int APS5_VABI sceRudpGetRemoteInfo(int, void*, uint32_t*, uint16_t*);
}

static void Require(bool value) {
    if (!value) std::abort();
}

static void APS5_VABI Handler(int, int, int, void*) {}

int main() {
    constexpr int notInitialized = static_cast<int>(0x80770001u);
    constexpr int invalidContext = static_cast<int>(0x80770003u);
    constexpr int invalidArgument = static_cast<int>(0x80770004u);
    constexpr int notAcceptable = static_cast<int>(0x80770011u);
    alignas(16) static std::array<unsigned char, 0x10000> pool{};
    std::array<unsigned char, 16> address;
    address.fill(0x5a);
    const auto originalAddress = address;
    uint32_t addressSize = address.size();
    uint16_t vport = 0xa55a;
    struct OptionBuffer { uint32_t before, value, after; } output{0xa5a5a5a5, 0x5a5a5a5a, 0xa5a5a5a5};

    Require(sceRudpEnableInternalIOThread2(0x10000, 700, 1) == notInitialized);
    Require(sceRudpProcessEvents(0) == notInitialized);
    Require(sceRudpNetReceived(1, address.data(), 1, address.data(), address.size()) == notInitialized);
    Require(sceRudpGetOption(0, 2, &output.value, 4) == notInitialized);
    Require(sceRudpGetRemoteInfo(0, address.data(), &addressSize, &vport) == notInitialized);
    Require(sceRudpGetNumberOfPacketsToRead(0) == notInitialized);
    Require(sceRudpGetSizeWritable(0) == notInitialized);
    Require(sceRudpGetSizeReadable(0) == notInitialized);
    Require(sceRudpInit_nid_postfix(pool.data(), static_cast<int>(pool.size())) == 0);
    Require(sceRudpEnableInternalIOThread2(0x10000, 700, 1) == 0);
    Require(sceRudpProcessEvents(0) == 0);
    Require(sceRudpNetReceived(-1, address.data(), 1, address.data(), address.size()) == invalidArgument);
    Require(sceRudpNetReceived(1, nullptr, 1, address.data(), address.size()) == invalidArgument);
    Require(sceRudpNetReceived(1, address.data(), 0, address.data(), address.size()) == invalidArgument);
    Require(sceRudpNetReceived(1, address.data(), 1, nullptr, address.size()) == invalidArgument);
    Require(sceRudpNetReceived(1, address.data(), 1, address.data(), 0) == invalidArgument);
    Require(sceRudpNetReceived(1, address.data(), 1, address.data(), address.size()) == 0);
    Require(sceRudpGetNumberOfPacketsToRead(0) == invalidContext);
    Require(sceRudpGetSizeWritable(0) == invalidContext);
    Require(sceRudpGetSizeReadable(0) == invalidContext);
    Require(sceRudpGetNumberOfPacketsToRead(12345) == invalidContext);
    Require(sceRudpGetSizeWritable(12345) == invalidContext);
    Require(sceRudpGetSizeReadable(12345) == invalidContext);
    Require(sceRudpGetNumberOfPacketsToRead(-1) == invalidArgument);
    Require(sceRudpGetSizeWritable(-1) == invalidArgument);
    Require(sceRudpGetSizeReadable(-1) == invalidArgument);

    int context = -1;
    Require(sceRudpCreateContext(Handler, nullptr, &context) == 0);
    Require(sceRudpGetOption(-1, 2, &output.value, 4) == invalidArgument);
    Require(sceRudpGetOption(12345, 2, &output.value, 4) == invalidContext);
    Require(sceRudpGetOption(context, 2, &output.value, 4) == notAcceptable);
    Require(output.value == 0x5a5a5a5a);
    Require(sceRudpGetRemoteInfo(12345, address.data(), &addressSize, &vport) == invalidContext);
    Require(sceRudpGetRemoteInfo(context, nullptr, &addressSize, &vport) == invalidArgument);
    Require(sceRudpGetRemoteInfo(context, address.data(), nullptr, &vport) == invalidArgument);
    Require(sceRudpGetRemoteInfo(context, address.data(), &addressSize, nullptr) == invalidArgument);
    Require(sceRudpGetRemoteInfo(context, address.data(), &addressSize, &vport) == notAcceptable);
    Require(address == originalAddress && addressSize == address.size() && vport == 0xa55a);
    for (const int option : {2, 3, 5, 6, 9}) {
        const uint32_t value = 0xf0000000u + option;
        Require(sceRudpSetOption(context, option, &value, sizeof(value)) == 0);
        Require(sceRudpGetOption(context, option, &output.value, sizeof(output.value)) == 0);
        Require(output.value == value);
        Require(output.before == 0xa5a5a5a5 && output.after == 0xa5a5a5a5);
    }
    int secondContext = -1;
    Require(sceRudpCreateContext(Handler, nullptr, &secondContext) == 0);
    Require(sceRudpGetOption(secondContext, 2, &output.value, 4) == notAcceptable);
    const uint32_t previous = output.value;
    Require(sceRudpGetOption(context, 2, nullptr, 4) == invalidArgument);
    Require(sceRudpGetOption(context, 2, &output.value, 3) == invalidArgument);
    Require(sceRudpGetOption(context, 2, &output.value, 8) == invalidArgument);
    Require(sceRudpGetOption(context, 99, &output.value, 4) == invalidArgument);
    Require(sceRudpSetOption(context, 2, nullptr, 4) == invalidArgument);
    Require(sceRudpSetOption(context, 2, &output.value, 8) == invalidArgument);
    Require(output.value == previous && output.before == 0xa5a5a5a5 && output.after == 0xa5a5a5a5);
    Require(sceRudpGetNumberOfPacketsToRead(context) == 0);
    Require(sceRudpGetSizeWritable(context) == notAcceptable);
    Require(sceRudpGetSizeReadable(context) == notAcceptable);
    Require(sceRudpTerminate(context) == 0);
    Require(sceRudpGetNumberOfPacketsToRead(context) == invalidContext);
    Require(sceRudpGetSizeWritable(context) == invalidContext);
    Require(sceRudpGetSizeReadable(context) == invalidContext);
    Require(sceRudpEnd() == 0);
    return 0;
}
