#include "SceTypes.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <array>
#include <limits>
#include <stdexcept>

struct NpWebApi2MemoryPoolStats {
    std::size_t pool_size;
    std::size_t max_inuse_size;
    std::size_t current_inuse_size;
    std::int32_t reserved;
};

extern "C" {
int APS5_VABI sceNpWebApi2PushEventCreateHandle(int);
int APS5_VABI sceNpWebApi2PushEventAbortHandle(int);
int* APS5_VABI __error_nid_postfix();
int APS5_VABI sceNpWebApi2Initialize(int, std::size_t);
int APS5_VABI sceNpWebApi2Terminate(int);
int APS5_VABI sceNpWebApi2GetMemoryPoolStats(int, NpWebApi2MemoryPoolStats*);
}

static void Require(bool value) { if (!value) std::abort(); }

template <typename TCall>
static bool Throws(TCall call) {
    try {
        call();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

int main() {
    const int handle = sceNpWebApi2PushEventCreateHandle(1);
    Require(handle > 0);
    constexpr int unavailable = static_cast<int>(0x80553406u);
    static_assert(sizeof(int) == sizeof(std::int32_t) && unavailable < 0);
    for (int input : std::array<int, 4>{handle, 0, -1, std::numeric_limits<int>::max()}) {
        *__error_nid_postfix() = 13;
        Require(sceNpWebApi2PushEventAbortHandle(input) == unavailable);
        Require(*__error_nid_postfix() == 13);
    }
    const int context = sceNpWebApi2Initialize(1, 0x20000);
    const int other = sceNpWebApi2Initialize(1, 0x8000);
    Require(context > 0 && other > 0 && context != other);
    NpWebApi2MemoryPoolStats stats{1, 1, 1, 1};
    Require(sceNpWebApi2GetMemoryPoolStats(context, &stats) == 0);
    Require(stats.pool_size == 0x20000 && stats.max_inuse_size == 0 && stats.current_inuse_size == 0 && stats.reserved == 0);
    Require(sceNpWebApi2GetMemoryPoolStats(other, &stats) == 0 && stats.pool_size == 0x8000);
    Require(Throws([&] { sceNpWebApi2GetMemoryPoolStats(context, nullptr); }));
    Require(sceNpWebApi2Terminate(context) == 0);
    Require(Throws([&] { sceNpWebApi2GetMemoryPoolStats(context, &stats); }));
    Require(sceNpWebApi2Terminate(other) == 0);
    return 0;
}
