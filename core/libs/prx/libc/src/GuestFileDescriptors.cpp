#include "prx/libc/include/GuestFileDescriptors.hpp"
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <mutex>
#include <new>
#include <utility>
#ifdef _WIN32
#include <io.h>
extern "C" _invalid_parameter_handler _set_thread_local_invalid_parameter_handler(_invalid_parameter_handler);
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace GuestFiles {
namespace {
#ifdef _WIN32
void IgnoreInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, std::uintptr_t) {}
#endif

bool NativePresent(int descriptor) {
#ifdef _WIN32
    const auto previous = _set_thread_local_invalid_parameter_handler(IgnoreInvalidParameter);
    const bool present = ::_get_osfhandle(descriptor) != -1;
    _set_thread_local_invalid_parameter_handler(previous);
    return present;
#else
    return ::fcntl(descriptor, F_GETFD) != -1;
#endif
}

int NativeClose(int descriptor) {
#ifdef _WIN32
    return ::_close(descriptor);
#else
    return ::close(descriptor);
#endif
}

int NativeDuplicate(int descriptor) {
#ifdef _WIN32
    return ::_dup(descriptor);
#else
    return ::fcntl(descriptor, F_DUPFD_CLOEXEC, 3);
#endif
}

void Dispose(int descriptor, NativeCleanup cleanup) noexcept {
    const int saved = errno;
    if (descriptor >= 0) {
        if (cleanup) cleanup(descriptor);
        NativeClose(descriptor);
    }
    errno = saved;
}

struct OwnedNative {
    int descriptor;
    NativeCleanup cleanup;
    ~OwnedNative() { Dispose(descriptor, cleanup); }
    int Release() { return std::exchange(descriptor, -1); }
};
}

class Entry {
    mutable int nativeDescriptor;
    int logicalDescriptor;
    int accessMode;
    NativeCleanup cleanup;

public:
    Entry(int nativeDescriptor, int logicalDescriptor, int accessMode, NativeCleanup cleanup)
        : nativeDescriptor(nativeDescriptor), logicalDescriptor(logicalDescriptor), accessMode(accessMode), cleanup(cleanup) {}
    ~Entry() { Dispose(std::exchange(nativeDescriptor, -1), cleanup); }
    int NativeDescriptor() const { return nativeDescriptor; }
    int LogicalDescriptor() const { return logicalDescriptor; }
    int AccessMode() const { return accessMode; }
    int CloseNative() const {
        const int saved = errno;
        const int descriptor = std::exchange(nativeDescriptor, -1);
        if (descriptor < 0) return 0;
        if (cleanup) cleanup(descriptor);
        const int result = NativeClose(descriptor);
        const int error = errno;
        errno = result == 0 ? saved : GuestFileNativeError_nid_no_patch(error);
        return result;
    }
};

namespace {
struct Table {
    std::mutex mutex;
    std::array<Lease, 32768> entries{};
    bool standardsInitialized = false;
    bool standardsSnapshotted = false;
    std::array<bool, 3> standardsPresent{};
};

Table& GetTable() {
    alignas(Table) static std::byte storage[sizeof(Table)];
    static Table* table = new (storage) Table;
    return *table;
}

int FreeDescriptor(const Table& table, int excluded = -1) {
    for (int descriptor = 3; descriptor != 32768; ++descriptor)
        if (descriptor != excluded && !table.entries[descriptor]) return descriptor;
    return -1;
}

int CloseRetired(Lease& retired) {
    if (retired.use_count() == 1) return retired->CloseNative();
    return 0;
}

Lease Create(OwnedNative& owned, int descriptor, int accessMode) {
    auto result = std::make_shared<Entry>(owned.descriptor, descriptor, accessMode, owned.cleanup);
    owned.Release();
    return result;
}
}

int GuestFileInitializeStandards_nid_no_patch() {
    const int saved = errno;
    auto& table = GetTable();
    std::array<Lease, 3> standards{};
    std::unique_lock lock(table.mutex);
    if (table.standardsInitialized) return 0;
    if (!table.standardsSnapshotted) {
        for (int descriptor = 0; descriptor != 3; ++descriptor)
            table.standardsPresent[descriptor] = NativePresent(descriptor);
        table.standardsSnapshotted = true;
    }
    try {
        for (int descriptor = 0; descriptor != 3; ++descriptor) {
            if (!table.standardsPresent[descriptor]) continue;
            OwnedNative owned{NativeDuplicate(descriptor), nullptr};
            if (owned.descriptor < 0) {
                const int error = GuestFileNativeError_nid_no_patch(errno);
                lock.unlock();
                standards = {};
                errno = error;
                return -1;
            }
            standards[descriptor] = Create(owned, descriptor, descriptor == 0 ? 0 : 1);
        }
    } catch (const std::bad_alloc&) { lock.unlock(); standards = {}; errno = 12; return -1; }
    for (int descriptor = 0; descriptor != 3; ++descriptor) table.entries[descriptor] = std::move(standards[descriptor]);
    table.standardsInitialized = true;
    errno = saved;
    return 0;
}

Lease GuestFileAcquire_nid_no_patch(int descriptor) {
    if (descriptor < 0 || descriptor >= 32768) { errno = 9; return {}; }
    if (GuestFileInitializeStandards_nid_no_patch() != 0) return {};
    auto& table = GetTable();
    std::lock_guard lock(table.mutex);
    auto result = table.entries[descriptor];
    if (!result) errno = 9;
    return result;
}

Lease GuestFileAdoptOwned_nid_no_patch(int nativeDescriptor, int accessMode, NativeCleanup cleanup) {
    OwnedNative owned{nativeDescriptor, cleanup};
    if (nativeDescriptor < 0) { errno = 9; return {}; }
    if (accessMode < 0 || accessMode > 2) { errno = 22; return {}; }
    if (GuestFileInitializeStandards_nid_no_patch() != 0) return {};
    auto& table = GetTable();
    std::lock_guard lock(table.mutex);
    const int descriptor = FreeDescriptor(table);
    if (descriptor < 0) { errno = 24; return {}; }
    try {
        auto result = Create(owned, descriptor, accessMode);
        table.entries[descriptor] = result;
        return result;
    } catch (const std::bad_alloc&) { errno = 12; return {}; }
}

std::array<Lease, 2> GuestFileAdoptPairOwned_nid_no_patch(int first, int second,
    int firstAccessMode, int secondAccessMode, NativeCleanup cleanup) {
    OwnedNative firstOwned{first, cleanup}, secondOwned{second == first ? -1 : second, cleanup};
    if (first < 0 || second < 0) { errno = 9; return {}; }
    if (first == second || firstAccessMode < 0 || firstAccessMode > 2 || secondAccessMode < 0 || secondAccessMode > 2) {
        errno = 22; return {};
    }
    if (GuestFileInitializeStandards_nid_no_patch() != 0) return {};
    auto& table = GetTable();
    std::array<Lease, 2> result;
    std::unique_lock lock(table.mutex);
    const int firstLogical = FreeDescriptor(table);
    const int secondLogical = FreeDescriptor(table, firstLogical);
    if (firstLogical < 0 || secondLogical < 0) { errno = 24; return {}; }
    try {
        result[0] = Create(firstOwned, firstLogical, firstAccessMode);
        result[1] = Create(secondOwned, secondLogical, secondAccessMode);
        table.entries[firstLogical] = result[0];
        table.entries[secondLogical] = result[1];
        return result;
    } catch (const std::bad_alloc&) { lock.unlock(); result = {}; errno = 12; return {}; }
}

Lease GuestFileReplaceOwnedMatching_nid_no_patch(const Identity& expected, int nativeDescriptor,
    int accessMode, NativeCleanup cleanup) {
    OwnedNative owned{nativeDescriptor, cleanup};
    if (nativeDescriptor < 0) { errno = 9; return {}; }
    if (accessMode < 0 || accessMode > 2) { errno = 22; return {}; }
    auto previous = expected.lock();
    if (!previous) { errno = 9; return {}; }
    const int descriptor = previous->LogicalDescriptor();
    auto& table = GetTable();
    Lease retired, replacement;
    {
        std::lock_guard lock(table.mutex);
        if (table.entries[descriptor] != previous) { errno = 9; return {}; }
        try { replacement = Create(owned, descriptor, accessMode); }
        catch (const std::bad_alloc&) { errno = 12; return {}; }
        retired = std::exchange(table.entries[descriptor], replacement);
    }
    previous.reset();
    return replacement;
}

int GuestFileClose_nid_no_patch(int descriptor) {
    if (descriptor < 0 || descriptor >= 32768) { errno = 9; return -1; }
    if (GuestFileInitializeStandards_nid_no_patch() != 0) return -1;
    auto& table = GetTable();
    Lease retired;
    {
        std::lock_guard lock(table.mutex);
        retired = std::exchange(table.entries[descriptor], {});
    }
    if (!retired) { errno = 9; return -1; }
    return CloseRetired(retired);
}

int GuestFileCloseMatching_nid_no_patch(const Identity& expected) {
    auto previous = expected.lock();
    if (!previous) { errno = 9; return -1; }
    const int descriptor = previous->LogicalDescriptor();
    auto& table = GetTable();
    Lease retired;
    {
        std::lock_guard lock(table.mutex);
        if (table.entries[descriptor] != previous) { errno = 9; return -1; }
        retired = std::exchange(table.entries[descriptor], {});
    }
    previous.reset();
    return CloseRetired(retired);
}

bool GuestFileMatches_nid_no_patch(const Identity& expected) {
    const auto entry = expected.lock();
    if (!entry) { errno = 9; return false; }
    auto& table = GetTable();
    std::lock_guard lock(table.mutex);
    if (table.entries[entry->LogicalDescriptor()] == entry) return true;
    errno = 9;
    return false;
}

int GuestFileNativeDescriptor_nid_no_patch(const Lease& lease) { return lease ? lease->NativeDescriptor() : -1; }
int GuestFileLogicalDescriptor_nid_no_patch(const Lease& lease) { return lease ? lease->LogicalDescriptor() : -1; }
int GuestFileAccessMode_nid_no_patch(const Lease& lease) { return lease ? lease->AccessMode() : -1; }

int GuestFileDuplicateNative_nid_no_patch(const Lease& lease) {
    if (!lease) { errno = 9; return -1; }
    const int result = NativeDuplicate(lease->NativeDescriptor());
    if (result < 0) errno = GuestFileNativeError_nid_no_patch(errno);
    return result;
}

int GuestFileNativeError_nid_no_patch(int nativeError) {
    if (nativeError == 0) return 0;
#ifdef EPERM
    if (nativeError == EPERM) return 1;
#endif
#ifdef ENOENT
    if (nativeError == ENOENT) return 2;
#endif
#ifdef ESRCH
    if (nativeError == ESRCH) return 3;
#endif
#ifdef EINTR
    if (nativeError == EINTR) return 4;
#endif
#ifdef EIO
    if (nativeError == EIO) return 5;
#endif
#ifdef ENXIO
    if (nativeError == ENXIO) return 6;
#endif
#ifdef E2BIG
    if (nativeError == E2BIG) return 7;
#endif
#ifdef ENOEXEC
    if (nativeError == ENOEXEC) return 8;
#endif
#ifdef EBADF
    if (nativeError == EBADF) return 9;
#endif
#ifdef ECHILD
    if (nativeError == ECHILD) return 10;
#endif
#ifdef EDEADLK
    if (nativeError == EDEADLK) return 11;
#endif
#ifdef ENOMEM
    if (nativeError == ENOMEM) return 12;
#endif
#ifdef EACCES
    if (nativeError == EACCES) return 13;
#endif
#ifdef EFAULT
    if (nativeError == EFAULT) return 14;
#endif
#ifdef ENOTBLK
    if (nativeError == ENOTBLK) return 15;
#endif
#ifdef EBUSY
    if (nativeError == EBUSY) return 16;
#endif
#ifdef EEXIST
    if (nativeError == EEXIST) return 17;
#endif
#ifdef EXDEV
    if (nativeError == EXDEV) return 18;
#endif
#ifdef ENODEV
    if (nativeError == ENODEV) return 19;
#endif
#ifdef ENOTDIR
    if (nativeError == ENOTDIR) return 20;
#endif
#ifdef EISDIR
    if (nativeError == EISDIR) return 21;
#endif
#ifdef EINVAL
    if (nativeError == EINVAL) return 22;
#endif
#ifdef ENFILE
    if (nativeError == ENFILE) return 23;
#endif
#ifdef EMFILE
    if (nativeError == EMFILE) return 24;
#endif
#ifdef ENOTTY
    if (nativeError == ENOTTY) return 25;
#endif
#ifdef ETXTBSY
    if (nativeError == ETXTBSY) return 26;
#endif
#ifdef EFBIG
    if (nativeError == EFBIG) return 27;
#endif
#ifdef ENOSPC
    if (nativeError == ENOSPC) return 28;
#endif
#ifdef ESPIPE
    if (nativeError == ESPIPE) return 29;
#endif
#ifdef EROFS
    if (nativeError == EROFS) return 30;
#endif
#ifdef EMLINK
    if (nativeError == EMLINK) return 31;
#endif
#ifdef EPIPE
    if (nativeError == EPIPE) return 32;
#endif
#ifdef EDOM
    if (nativeError == EDOM) return 33;
#endif
#ifdef ERANGE
    if (nativeError == ERANGE) return 34;
#endif
#ifdef EAGAIN
    if (nativeError == EAGAIN) return 35;
#endif
#ifdef EWOULDBLOCK
    if (nativeError == EWOULDBLOCK) return 35;
#endif
#ifdef EINPROGRESS
    if (nativeError == EINPROGRESS) return 36;
#endif
#ifdef EALREADY
    if (nativeError == EALREADY) return 37;
#endif
#ifdef ENOTSOCK
    if (nativeError == ENOTSOCK) return 38;
#endif
#ifdef EDESTADDRREQ
    if (nativeError == EDESTADDRREQ) return 39;
#endif
#ifdef EMSGSIZE
    if (nativeError == EMSGSIZE) return 40;
#endif
#ifdef EPROTOTYPE
    if (nativeError == EPROTOTYPE) return 41;
#endif
#ifdef ENOPROTOOPT
    if (nativeError == ENOPROTOOPT) return 42;
#endif
#ifdef EPROTONOSUPPORT
    if (nativeError == EPROTONOSUPPORT) return 43;
#endif
#ifdef ESOCKTNOSUPPORT
    if (nativeError == ESOCKTNOSUPPORT) return 44;
#endif
#ifdef EOPNOTSUPP
    if (nativeError == EOPNOTSUPP) return 45;
#endif
#ifdef ENOTSUP
    if (nativeError == ENOTSUP) return 45;
#endif
#ifdef EPFNOSUPPORT
    if (nativeError == EPFNOSUPPORT) return 46;
#endif
#ifdef EAFNOSUPPORT
    if (nativeError == EAFNOSUPPORT) return 47;
#endif
#ifdef EADDRINUSE
    if (nativeError == EADDRINUSE) return 48;
#endif
#ifdef EADDRNOTAVAIL
    if (nativeError == EADDRNOTAVAIL) return 49;
#endif
#ifdef ENETDOWN
    if (nativeError == ENETDOWN) return 50;
#endif
#ifdef ENETUNREACH
    if (nativeError == ENETUNREACH) return 51;
#endif
#ifdef ENETRESET
    if (nativeError == ENETRESET) return 52;
#endif
#ifdef ECONNABORTED
    if (nativeError == ECONNABORTED) return 53;
#endif
#ifdef ECONNRESET
    if (nativeError == ECONNRESET) return 54;
#endif
#ifdef ENOBUFS
    if (nativeError == ENOBUFS) return 55;
#endif
#ifdef EISCONN
    if (nativeError == EISCONN) return 56;
#endif
#ifdef ENOTCONN
    if (nativeError == ENOTCONN) return 57;
#endif
#ifdef ESHUTDOWN
    if (nativeError == ESHUTDOWN) return 58;
#endif
#ifdef ETOOMANYREFS
    if (nativeError == ETOOMANYREFS) return 59;
#endif
#ifdef ETIMEDOUT
    if (nativeError == ETIMEDOUT) return 60;
#endif
#ifdef ECONNREFUSED
    if (nativeError == ECONNREFUSED) return 61;
#endif
#ifdef ELOOP
    if (nativeError == ELOOP) return 62;
#endif
#ifdef ENAMETOOLONG
    if (nativeError == ENAMETOOLONG) return 63;
#endif
#ifdef EHOSTDOWN
    if (nativeError == EHOSTDOWN) return 64;
#endif
#ifdef EHOSTUNREACH
    if (nativeError == EHOSTUNREACH) return 65;
#endif
#ifdef ENOTEMPTY
    if (nativeError == ENOTEMPTY) return 66;
#endif
#ifdef EUSERS
    if (nativeError == EUSERS) return 68;
#endif
#ifdef EDQUOT
    if (nativeError == EDQUOT) return 69;
#endif
#ifdef ESTALE
    if (nativeError == ESTALE) return 70;
#endif
#ifdef EREMOTE
    if (nativeError == EREMOTE) return 71;
#endif
#ifdef ENOLCK
    if (nativeError == ENOLCK) return 77;
#endif
#ifdef ENOSYS
    if (nativeError == ENOSYS) return 78;
#endif
#ifdef EIDRM
    if (nativeError == EIDRM) return 82;
#endif
#ifdef ENOMSG
    if (nativeError == ENOMSG) return 83;
#endif
#ifdef EOVERFLOW
    if (nativeError == EOVERFLOW) return 84;
#endif
#ifdef ECANCELED
    if (nativeError == ECANCELED) return 85;
#endif
#ifdef EILSEQ
    if (nativeError == EILSEQ) return 86;
#endif
#ifdef ENODATA
    if (nativeError == ENODATA) return 87;
#endif
#ifdef EBADMSG
    if (nativeError == EBADMSG) return 89;
#endif
#ifdef EMULTIHOP
    if (nativeError == EMULTIHOP) return 90;
#endif
#ifdef ENOLINK
    if (nativeError == ENOLINK) return 91;
#endif
#ifdef EPROTO
    if (nativeError == EPROTO) return 92;
#endif
#ifdef ENOTRECOVERABLE
    if (nativeError == ENOTRECOVERABLE) return 95;
#endif
#ifdef EOWNERDEAD
    if (nativeError == EOWNERDEAD) return 96;
#endif
    return 5;
}

}
