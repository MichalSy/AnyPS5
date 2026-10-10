#include "prx/libkernel/File/include/FileLock.hpp"

#ifdef _WIN32

#include <cerrno>
#include <io.h>
#include <map>
#include <mutex>
#include <new>
#include <windows.h>

namespace {

constexpr int LockShared = 1;
constexpr int LockExclusive = 2;
constexpr int LockNonblocking = 4;
constexpr int LockUnlock = 8;

struct LockState {
    explicit LockState(const GuestFiles::Lease& lease) : owner(lease) {}
    GuestFiles::Identity owner;
    std::mutex transition;
    int held = 0;
};

std::mutex g_mutex;
std::map<int, std::shared_ptr<LockState>> g_locks;

std::shared_ptr<LockState> FindLockState(int fd, const GuestFiles::Lease& owner) {
    std::lock_guard lock(g_mutex);
    const auto found = g_locks.find(fd);
    if (found != g_locks.end() && found->second->owner.lock() == owner) return found->second;
    auto state = std::make_shared<LockState>(owner);
    g_locks.insert_or_assign(fd, state);
    return state;
}

}

namespace File {

int Flock(const GuestFiles::Lease& owner, int operation) {
    const int mode = operation & (LockShared | LockExclusive | LockUnlock);
    if (mode != LockShared && mode != LockExclusive && mode != LockUnlock) {
        errno = EINVAL;
        ::SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }
    if (!owner) {
        errno = EBADF;
        ::SetLastError(ERROR_INVALID_HANDLE);
        return -1;
    }
    const int fd = GuestFiles::GuestFileNativeDescriptor_nid_no_patch(owner);
    const auto handle = reinterpret_cast<HANDLE>(::_get_osfhandle(fd));
    if (handle == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        ::SetLastError(ERROR_INVALID_HANDLE);
        return -1;
    }
    std::shared_ptr<LockState> state;
    try {
        state = FindLockState(fd, owner);
    } catch (const std::bad_alloc&) {
        errno = ENOMEM;
        ::SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return -1;
    }
    std::lock_guard transition(state->transition);
    if (state->held == mode) return 0;
    if (state->held != 0) {
        OVERLAPPED overlapped{};
        if (!::UnlockFileEx(handle, 0, MAXDWORD, MAXDWORD, &overlapped)) return -1;
        state->held = 0;
    }
    if (mode == LockUnlock) return 0;
    DWORD flags = mode == LockExclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0;
    if (operation & LockNonblocking) flags |= LOCKFILE_FAIL_IMMEDIATELY;
    OVERLAPPED overlapped{};
    if (!::LockFileEx(handle, flags, 0, MAXDWORD, MAXDWORD, &overlapped)) return -1;
    state->held = mode;
    return 0;
}

void ForgetFileLock(int fd) {
    std::lock_guard lock(g_mutex);
    g_locks.erase(fd);
}

}

#endif
