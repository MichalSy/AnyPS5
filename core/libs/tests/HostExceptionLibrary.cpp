#include <ext/stdio_filebuf.h>
#include <cerrno>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {
int destroyed;
struct Guard {
    ~Guard() { ++destroyed; }
};

[[gnu::noinline]] void HostLibraryThrow() {
    Guard guard;
    const std::string value("a");
    static_cast<void>(value.at(2));
}
}

extern "C" int RunHostIoFailure() {
    int descriptors[2];
    if (::pipe(descriptors) != 0) return 10;
    ::close(descriptors[1]);
    __gnu_cxx::stdio_filebuf<char> buffer(descriptors[0], std::ios::in, 1);
    std::istream input(&buffer);
    if (input.exceptions() != std::ios::goodbit) return 11;
    if (::close(descriptors[0]) != 0) return 12;
    std::string line;
    std::puts("IO: entering std::getline on a deliberately invalid private fd");
    std::fflush(stdout);
    try {
        std::getline(input, line);
    } catch (const std::exception& error) {
        std::printf("IO: unexpected propagated exception: %s\n", error.what());
        return 13;
    } catch (...) {
        std::puts("IO: unexpected propagated foreign exception");
        return 14;
    }
    std::printf("IO: bad=%d fail=%d mask=%d errno=%d\n", input.bad(), input.fail(),
                static_cast<int>(input.exceptions()), errno);
    return input.bad() && input.fail() ? 0 : 15;
}

extern "C" int RunHostTypedException() {
    destroyed = 0;
    std::puts("TYPED: entering host std::string::at out-of-range throw");
    std::fflush(stdout);
    try {
        HostLibraryThrow();
    } catch (const std::out_of_range& error) {
        std::printf("TYPED: caught std::out_of_range, destroyed=%d, what=%s\n", destroyed, error.what());
        return destroyed == 1 && *error.what() ? 0 : 20;
    } catch (...) {
        std::printf("TYPED: wrong catch, destroyed=%d\n", destroyed);
        return 21;
    }
    return 22;
}
