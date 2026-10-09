#include "prx/libc/include/general/VabiMacros.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <limits>

extern "C" {
int APS5_VABI sceNpWebApi2PushEventCreateHandle(int);
int APS5_VABI sceNpWebApi2PushEventAbortHandle(int);
int* APS5_VABI __error_nid_postfix();
}

static void Require(bool value, const char* message) {
    if (value) return;
    std::fprintf(stderr, "guest NP WebApi2 test failed: %s\n", message);
    std::abort();
}

int main() {
    const int handle = sceNpWebApi2PushEventCreateHandle(1);
    Require(handle > 0, "create local push-event handle");
    constexpr int unavailable = static_cast<int>(0x80553406u);
    static_assert(sizeof(int) == sizeof(std::int32_t) && unavailable < 0);
    for (int input : std::array<int, 4>{handle, 0, -1, std::numeric_limits<int>::max()}) {
        *__error_nid_postfix() = 13;
        Require(sceNpWebApi2PushEventAbortHandle(input) == unavailable, "abort explicitly reports unavailable PSN service");
        Require(*__error_nid_postfix() == 13, "NP service status preserves POSIX errno");
    }
}
