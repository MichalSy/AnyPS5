#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_GUESTFILEDESCRIPTORS_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_GUESTFILEDESCRIPTORS_HPP

#include <array>
#include <memory>

namespace GuestFiles {

class Entry;
using Lease = std::shared_ptr<const Entry>;
using Identity = std::weak_ptr<const Entry>;
using NativeCleanup = void (*)(int) noexcept;

int InitializeStandards_nid_no_patch();
Lease Acquire_nid_no_patch(int descriptor);
Lease AdoptOwned_nid_no_patch(int nativeDescriptor, int accessMode, NativeCleanup cleanup = nullptr);
std::array<Lease, 2> AdoptPairOwned_nid_no_patch(int first, int second,
    int firstAccessMode, int secondAccessMode, NativeCleanup cleanup = nullptr);
Lease ReplaceOwnedMatching_nid_no_patch(const Identity& expected, int nativeDescriptor,
    int accessMode, NativeCleanup cleanup = nullptr);
int Close_nid_no_patch(int descriptor);
int CloseMatching_nid_no_patch(const Identity& expected);
bool Matches_nid_no_patch(const Identity& expected);
int NativeDescriptor_nid_no_patch(const Lease& lease);
int LogicalDescriptor_nid_no_patch(const Lease& lease);
int AccessMode_nid_no_patch(const Lease& lease);
int DuplicateNative_nid_no_patch(const Lease& lease);
int NativeError_nid_no_patch(int nativeError);

}

#endif
