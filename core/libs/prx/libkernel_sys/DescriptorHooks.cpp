#include "prx/libc/include/GuestFileDescriptors.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <cerrno>

extern "C" {
int* APS5_VABI __error_nid_postfix();
int APS5_VABI fchdir_nid_postfix_native_nid_no_patch(int);

int APS5_VABI fchdir_nid_postfix(int descriptor) {
    const auto lease = GuestFiles::GuestFileAcquire_nid_no_patch(descriptor);
    if (!lease) { *__error_nid_postfix() = errno; return -1; }
    return fchdir_nid_postfix_native_nid_no_patch(GuestFiles::GuestFileNativeDescriptor_nid_no_patch(lease));
}
}
