#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libkernel/Socket/include/SocketRuntime.hpp"
#include "prx/libc/include/GuestFileDescriptors.hpp"
#include <cstdint>
#include <stdexcept>

extern "C" int* APS5_VABI __error_nid_postfix();

namespace {
constexpr int BadDescriptor = 9;
constexpr int BadAddress = 14;
constexpr int InvalidArgument = 22;
constexpr int NotTerminal = 25;
constexpr int OperationNotSupported = 45;
constexpr int AddressNotAvailable = 49;
constexpr int SetNow = 0;
constexpr int SetDrain = 1;
constexpr int SetFlush = 2;
constexpr int SetSoft = 0x10;
constexpr int InternetFamily = 2;

int Fail(int error) {
    *__error_nid_postfix() = error;
    return -1;
}

int TerminalError(int descriptor) {
    if (descriptor >= GuestSockets::FirstDescriptor) {
        const int family = GuestSockets::Family(descriptor);
        if (family < 0) return BadDescriptor;
        return family == InternetFamily ? AddressNotAvailable : OperationNotSupported;
    }
    const auto lease = GuestFiles::GuestFileAcquire_nid_no_patch(descriptor);
    if (!lease) return BadDescriptor;
    return NotTerminal;
}
}

extern "C" {
int APS5_VABI tcgetattr_nid_postfix(int descriptor, void* attributes) {
    static_cast<void>(attributes);
    return Fail(TerminalError(descriptor));
}

int APS5_VABI tcsetattr_nid_postfix(int descriptor, int action, const void* attributes) {
    if ((action & SetSoft) != 0 && attributes == nullptr) throw std::invalid_argument("tcsetattr: attributes is null");
    const int mode = action & ~SetSoft;
    if (mode != SetNow && mode != SetDrain && mode != SetFlush) return Fail(InvalidArgument);
    if (attributes == nullptr) return Fail(BadAddress);
    return Fail(TerminalError(descriptor));
}
}
