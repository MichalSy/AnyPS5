#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_GUESTFILEDESCRIPTORS_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_GUESTFILEDESCRIPTORS_HPP

#include <array>
#include <memory>

namespace GuestFiles {

class Entry;
using Lease = std::shared_ptr<const Entry>;
using Identity = std::weak_ptr<const Entry>;
using NativeCleanup = void (*)(int) noexcept;

extern "C" {
int GuestFileInitializeStandards_nid_no_patch();
Lease GuestFileAcquire_nid_no_patch(int descriptor);
Lease GuestFileAdoptOwned_nid_no_patch(int nativeDescriptor, int accessMode, NativeCleanup cleanup = nullptr);
std::array<Lease, 2> GuestFileAdoptPairOwned_nid_no_patch(int first, int second,
    int firstAccessMode, int secondAccessMode, NativeCleanup cleanup = nullptr);
Lease GuestFileReplaceOwnedMatching_nid_no_patch(const Identity& expected, int nativeDescriptor,
    int accessMode, NativeCleanup cleanup = nullptr);
int GuestFileClose_nid_no_patch(int descriptor);
int GuestFileCloseMatching_nid_no_patch(const Identity& expected);
bool GuestFileMatches_nid_no_patch(const Identity& expected);
int GuestFileNativeDescriptor_nid_no_patch(const Lease& lease);
int GuestFileLogicalDescriptor_nid_no_patch(const Lease& lease);
int GuestFileAccessMode_nid_no_patch(const Lease& lease);
int GuestFileDuplicateNative_nid_no_patch(const Lease& lease);
int GuestFileNativeError_nid_no_patch(int nativeError);
}

}

#endif
