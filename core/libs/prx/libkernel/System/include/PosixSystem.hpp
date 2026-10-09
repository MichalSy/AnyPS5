#ifndef CORE_LIBS_PRX_LIBKERNEL_SYSTEM_INCLUDE_POSIXSYSTEM_HPP
#define CORE_LIBS_PRX_LIBKERNEL_SYSTEM_INCLUDE_POSIXSYSTEM_HPP

#include "prx/libc/include/general/VabiMacros.hpp"
#include <cstddef>
#include <cstdint>

struct GuestResourceLimit {
    std::int64_t current;
    std::int64_t maximum;
};

static_assert(sizeof(GuestResourceLimit) == 16);

extern "C" {
int APS5_VABI system_nid_postfix(const char* command);
std::uint32_t APS5_VABI getuid_nid_postfix();
std::uint32_t APS5_VABI geteuid_nid_postfix();
std::uint32_t APS5_VABI getgid_nid_postfix();
std::uint32_t APS5_VABI getegid_nid_postfix();
int APS5_VABI getrlimit_nid_postfix(int resource, GuestResourceLimit* limit);
int APS5_VABI sysctl_nid_postfix(const int* name, unsigned nameLength, void* oldValue, std::size_t* oldLength, const void* newValue, std::size_t newLength);
int APS5_VABI sysctlbyname_nid_postfix(const char* name, void* oldValue, std::size_t* oldLength, const void* newValue, std::size_t newLength);
}

#endif
