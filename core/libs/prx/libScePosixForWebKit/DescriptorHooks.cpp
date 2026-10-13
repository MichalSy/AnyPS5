#include "prx/libc/include/GuestFileDescriptors.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libkernel/Socket/include/SocketRuntime.hpp"
#include <cerrno>

extern "C" {
int* APS5_VABI __error_nid_postfix();
int APS5_VABI isatty_nid_postfix_native_nid_no_patch(int);

int APS5_VABI isatty_nid_postfix(int descriptor) {
    if (GuestSocketIsOpen_nid_no_patch(descriptor))
        return isatty_nid_postfix_native_nid_no_patch(descriptor);
    const int savedError = *__error_nid_postfix();
    const auto lease = GuestFiles::GuestFileAcquire_nid_no_patch(descriptor);
    if (!lease) { *__error_nid_postfix() = errno; return 0; }
    const GuestSockets::NativeDescriptorScope nativeScope;
    const int result = isatty_nid_postfix_native_nid_no_patch(GuestFiles::GuestFileNativeDescriptor_nid_no_patch(lease));
    if (result) *__error_nid_postfix() = savedError;
    return result;
}
}
