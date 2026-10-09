#include "prx/libkernel/System/include/PosixSystem.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

extern "C" {
int* APS5_VABI __error_nid_postfix();
int APS5_VABI getpid_nid_postfix();
int APS5_VABI getargc_nid_postfix();
const char** APS5_VABI getargv_nid_postfix();
std::int64_t APS5_VABI sysconf_nid_postfix(int name);
}

namespace {

int Failure(int error) {
    *__error_nid_postfix() = error;
    return -1;
}

int CopyOut(const void* value, std::size_t size, void* oldValue, std::size_t* oldLength) {
    if (!oldLength) return oldValue ? Failure(14) : 0;
    if (!oldValue) {
        *oldLength = size;
        return 0;
    }
    const auto copied = std::min(size, *oldLength);
    if (copied) std::memcpy(oldValue, value, copied);
    *oldLength = copied;
    return copied == size ? 0 : Failure(12);
}

template <typename TValue>
int CopyScalar(TValue value, void* oldValue, std::size_t* oldLength) {
    return CopyOut(&value, sizeof(value), oldValue, oldLength);
}

int PhysicalMemory(bool available, std::uint64_t& bytes) {
#ifdef _WIN32
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (!GlobalMemoryStatusEx(&memory)) return Failure(5);
    bytes = available ? memory.ullAvailPhys : memory.ullTotalPhys;
#else
    const auto pages = ::sysconf(available ? _SC_AVPHYS_PAGES : _SC_PHYS_PAGES);
    const auto pageSize = ::sysconf(_SC_PAGESIZE);
    if (pages < 0 || pageSize <= 0) return Failure(5);
    if (static_cast<std::uint64_t>(pages) > std::numeric_limits<std::uint64_t>::max() / static_cast<std::uint64_t>(pageSize)) return Failure(84);
    bytes = static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(pageSize);
#endif
    return 0;
}

std::string ExecutablePath() {
#ifdef _WIN32
    std::array<char, 32768> path{};
    const auto size = GetModuleFileNameA(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!size || size >= path.size()) throw std::system_error(std::make_error_code(std::errc::io_error));
    return std::string(path.data(), size);
#else
    return std::filesystem::read_symlink("/proc/self/exe").string();
#endif
}

std::string ProcessArguments() {
    std::string arguments;
    const auto count = getargc_nid_postfix();
    const auto values = getargv_nid_postfix();
    for (int i = 0; i < count; ++i) {
        arguments.append(values[i]);
        arguments.push_back('\0');
    }
    return arguments;
}

int ReadHardware(int key, void* oldValue, std::size_t* oldLength) {
    switch (key) {
        case 1:
        case 11:
            return CopyOut("amd64", 6, oldValue, oldLength);
        case 3: {
            const auto cpus = sysconf_nid_postfix(58);
            if (cpus <= 0 || cpus > std::numeric_limits<int>::max()) return Failure(5);
            return CopyScalar(static_cast<int>(cpus), oldValue, oldLength);
        }
        case 4:
            return CopyScalar(1234, oldValue, oldLength);
        case 5:
        case 6:
        case 12: {
            std::uint64_t bytes = 0;
            if (PhysicalMemory(key == 6, bytes) != 0) return -1;
            return CopyScalar(bytes, oldValue, oldLength);
        }
        case 7:
            return CopyScalar(0x4000, oldValue, oldLength);
        default:
            return Failure(2);
    }
}

int ReadProcess(const int* name, void* oldValue, std::size_t* oldLength) {
    if (name[3] != -1 && name[3] != getpid_nid_postfix()) return Failure(2);
    std::string value;
    if (name[2] == 12) {
        value = ExecutablePath();
        value.push_back('\0');
    } else if (name[2] == 7) {
        value = ProcessArguments();
    } else {
        return Failure(2);
    }
    return CopyOut(value.data(), value.size(), oldValue, oldLength);
}

int ReadSysctl(const int* name, unsigned nameLength, void* oldValue, std::size_t* oldLength) {
    if (nameLength == 2 && name[0] == 6) return ReadHardware(name[1], oldValue, oldLength);
    if (nameLength == 4 && name[0] == 1 && name[1] == 14) return ReadProcess(name, oldValue, oldLength);
    return Failure(2);
}

#ifndef _WIN32
int NativeResource(int resource) {
    switch (resource) {
        case 0: return RLIMIT_CPU;
        case 1: return RLIMIT_FSIZE;
        case 2: return RLIMIT_DATA;
        case 3: return RLIMIT_STACK;
        case 4: return RLIMIT_CORE;
        case 5: return RLIMIT_RSS;
        case 6: return RLIMIT_MEMLOCK;
        case 7: return RLIMIT_NPROC;
        case 8: return RLIMIT_NOFILE;
        case 10: return RLIMIT_AS;
        default: return -1;
    }
}

std::int64_t GuestLimit(rlim_t value) {
    constexpr auto infinity = std::numeric_limits<std::int64_t>::max();
    return value == RLIM_INFINITY || value >= static_cast<rlim_t>(infinity) ? infinity : static_cast<std::int64_t>(value);
}
#endif

}

extern "C" {

int APS5_VABI system_nid_postfix(const char* command) {
    return command ? Failure(45) : 0;
}

int APS5_VABI getrlimit_nid_postfix(int resource, GuestResourceLimit* limit) {
    if (!limit) return Failure(14);
    if (resource < 0 || resource > 14) return Failure(22);
#ifdef _WIN32
    return Failure(45);
#else
    const int nativeResource = NativeResource(resource);
    if (nativeResource < 0) return Failure(45);
    const int saved = *__error_nid_postfix();
    rlimit native{};
    if (::getrlimit(nativeResource, &native) != 0) return Failure(errno == EINVAL ? 22 : errno == EFAULT ? 14 : 5);
    *limit = {GuestLimit(native.rlim_cur), GuestLimit(native.rlim_max)};
    *__error_nid_postfix() = saved;
    return 0;
#endif
}

int APS5_VABI sysctl_nid_postfix(const int* name, unsigned nameLength, void* oldValue, std::size_t* oldLength, const void* newValue, std::size_t newLength) {
    if (!name) return Failure(14);
    if (nameLength < 2 || nameLength > 24) return Failure(22);
    if (newValue && nameLength == 2 && name[0] == 6 && name[1] == 3) return Failure(1);
    if (newValue || newLength) return Failure(45);
    const int saved = *__error_nid_postfix();
    try {
        const int result = ReadSysctl(name, nameLength, oldValue, oldLength);
        if (result == 0) *__error_nid_postfix() = saved;
        return result;
    } catch (const std::bad_alloc&) { return Failure(12); }
      catch (const std::system_error&) { return Failure(5); }
}

int APS5_VABI sysctlbyname_nid_postfix(const char* name, void* oldValue, std::size_t* oldLength, const void* newValue, std::size_t newLength) {
    if (!name) return Failure(14);
    const std::string_view text(name);
    int key = 0;
    if (text == "hw.machine") key = 1;
    else if (text == "hw.machine_arch") key = 11;
    else if (text == "hw.ncpu") key = 3;
    else if (text == "hw.byteorder") key = 4;
    else if (text == "hw.physmem") key = 5;
    else if (text == "hw.usermem") key = 6;
    else if (text == "hw.realmem") key = 12;
    else if (text == "hw.pagesize") key = 7;
    else return Failure(2);
    const int mib[]{6, key};
    return sysctl_nid_postfix(mib, 2, oldValue, oldLength, newValue, newLength);
}

}
