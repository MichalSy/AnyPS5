#include "prx/libkernel/System/include/PosixSystem.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>
#ifndef _WIN32
#include <sys/resource.h>
#include <unistd.h>
#endif

extern "C" {
int* APS5_VABI __error_nid_postfix();
int APS5_VABI getpid_nid_postfix();
}

static void Require(bool value, const char* message) {
    if (value) return;
    std::fprintf(stderr, "guest POSIX system test failed: %s\n", message);
    std::abort();
}

int main() {
    const auto marker = std::filesystem::temp_directory_path() / ("anyps5-system-marker-" + std::to_string(getpid_nid_postfix()));
    std::filesystem::remove(marker);
    const std::string command = "echo executed > \"" + marker.string() + "\"";
    *__error_nid_postfix() = 13;
    Require(system_nid_postfix(nullptr) == 0 && *__error_nid_postfix() == 13, "shell capability is unavailable without altering errno");
    Require(system_nid_postfix(command.c_str()) == -1 && *__error_nid_postfix() == 45, "shell command is explicitly unsupported");
    Require(!std::filesystem::exists(marker), "unsupported command must not execute");

    *__error_nid_postfix() = 13;
    Require(getuid_nid_postfix() == 0 && geteuid_nid_postfix() == 0, "guest user identities");
    Require(getgid_nid_postfix() == 0 && getegid_nid_postfix() == 0, "guest group identities");
    Require(issetugid_nid_postfix() == 0, "guest credentials have not changed");
    Require(*__error_nid_postfix() == 13, "identity queries preserve errno");
#ifndef _WIN32
    rlimit native{};
    Require(::getrlimit(RLIMIT_NOFILE, &native) == 0, "native descriptor limit");
    GuestResourceLimit limit{};
    Require(getrlimit_nid_postfix(8, &limit) == 0, "FreeBSD descriptor resource maps to native resource");
    const auto expected = [](rlim_t value) {
        constexpr auto infinity = std::numeric_limits<std::int64_t>::max();
        return value == RLIM_INFINITY || value >= static_cast<rlim_t>(infinity) ? infinity : static_cast<std::int64_t>(value);
    };
    Require(limit.current == expected(native.rlim_cur) && limit.maximum == expected(native.rlim_max), "guest limits preserve values and translate infinity");
    Require(*__error_nid_postfix() == 13, "resource query preserves errno");
#else
    GuestResourceLimit limit{};
#endif
    Require(getrlimit_nid_postfix(0, nullptr) == -1 && *__error_nid_postfix() == 14, "null limit rejected");
    Require(getrlimit_nid_postfix(-1, &limit) == -1 && *__error_nid_postfix() == 22, "invalid resource rejected");
    Require(getrlimit_nid_postfix(9, &limit) == -1 && *__error_nid_postfix() == 45, "unsupported socket buffer resource rejected");

    const int ncpu[]{6, 3};
    int cpus = 0;
    std::size_t size = sizeof(cpus);
    *__error_nid_postfix() = 13;
    Require(sysctl_nid_postfix(ncpu, 2, &cpus, &size, nullptr, 0) == 0 && cpus > 0 && size == sizeof(cpus), "numeric CPU query");
    Require(*__error_nid_postfix() == 13, "sysctl success preserves errno");
    int namedCpus = 0;
    size = sizeof(namedCpus);
    Require(sysctlbyname_nid_postfix("hw.ncpu", &namedCpus, &size, nullptr, 0) == 0 && namedCpus == cpus, "named CPU query matches numeric query");
    size = 0;
    Require(sysctl_nid_postfix(ncpu, 2, nullptr, &size, nullptr, 0) == 0 && size == sizeof(cpus), "size-only query");
    std::array<unsigned char, sizeof(cpus)> shortValue{};
    size = 2;
    Require(sysctl_nid_postfix(ncpu, 2, shortValue.data(), &size, nullptr, 0) == -1 && *__error_nid_postfix() == 12 && size == 2, "short buffer reports copied length and ENOMEM");
    Require(std::memcmp(shortValue.data(), &cpus, 2) == 0, "short buffer copies available data");
    Require(sysctl_nid_postfix(ncpu, 2, &cpus, nullptr, nullptr, 0) == -1 && *__error_nid_postfix() == 14, "output requires a length pointer");
    Require(sysctlbyname_nid_postfix("hw.unsupported", &cpus, &size, nullptr, 0) == -1 && *__error_nid_postfix() == 2, "unknown name is ENOENT");
    const int unknown[]{6, 999};
    Require(sysctl_nid_postfix(unknown, 2, &cpus, &size, nullptr, 0) == -1 && *__error_nid_postfix() == 2, "unknown MIB is ENOENT");
    Require(sysctl_nid_postfix(nullptr, 2, &cpus, &size, nullptr, 0) == -1 && *__error_nid_postfix() == 14, "null MIB rejected");
    Require(sysctl_nid_postfix(ncpu, 0, &cpus, &size, nullptr, 0) == -1 && *__error_nid_postfix() == 22, "empty MIB rejected");
    const int replacement = 1;
    Require(sysctl_nid_postfix(ncpu, 2, &cpus, &size, &replacement, sizeof(replacement)) == -1 && *__error_nid_postfix() == 45 && cpus == namedCpus, "write cannot change hardware or copy output");

    int pageSize = 0;
    size = sizeof(pageSize);
    Require(sysctlbyname_nid_postfix("hw.pagesize", &pageSize, &size, nullptr, 0) == 0 && pageSize == 0x4000, "guest page size");
    std::uint64_t memory = 0;
    size = sizeof(memory);
    Require(sysctlbyname_nid_postfix("hw.realmem", &memory, &size, nullptr, 0) == 0 && memory > 0 && size == sizeof(memory), "64-bit physical memory");

    const int pathname[]{1, 14, 12, -1};
    size = 0;
    Require(sysctl_nid_postfix(pathname, 4, nullptr, &size, nullptr, 0) == 0 && size > 1, "own process pathname size");
    std::vector<char> path(size);
    Require(sysctl_nid_postfix(pathname, 4, path.data(), &size, nullptr, 0) == 0 && path.back() == '\0', "pathname has final NUL");
    Require(std::filesystem::is_regular_file(path.data()), "pathname identifies a real executable");
    const int arguments[]{1, 14, 7, getpid_nid_postfix()};
    size = 0;
    Require(sysctl_nid_postfix(arguments, 4, nullptr, &size, nullptr, 0) == 0 && size > 1, "own argv size");
    std::vector<char> argv(size);
    Require(sysctl_nid_postfix(arguments, 4, argv.data(), &size, nullptr, 0) == 0 && argv.back() == '\0', "argv is a NUL-separated buffer");
    const int foreignProcess[]{1, 14, 12, 0};
    Require(sysctl_nid_postfix(foreignProcess, 4, nullptr, &size, nullptr, 0) == -1 && *__error_nid_postfix() == 2, "foreign process query unavailable");
    return 0;
}
