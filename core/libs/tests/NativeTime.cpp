#include "SceTypes.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <limits>

extern "C" {
int APS5_VABI sceKernelConvertLocaltimeToUtc(std::int64_t, std::int64_t, std::int64_t*, KernelTimesec*, std::int32_t*);
int APS5_VABI sceKernelConvertUtcToLocaltime(std::int64_t, std::int64_t*, KernelTimesec*, std::uint64_t*);
}

template <typename TValue>
struct GuardedOutput {
    std::array<std::uint8_t, 8> before;
    TValue value;
    std::array<std::uint8_t, 8> after;

    explicit GuardedOutput(TValue initial) : value(initial) {
        before.fill(0xa5);
        after.fill(0x3c);
    }
};

using TimeOutput = GuardedOutput<std::int64_t>;
using InfoOutput = GuardedOutput<KernelTimesec>;
using LocalDstOutput = GuardedOutput<std::int32_t>;
using UtcDstOutput = GuardedOutput<std::uint64_t>;

static_assert(sizeof(KernelTimesec) == 16);
static_assert(offsetof(KernelTimesec, t) == 0);
static_assert(offsetof(KernelTimesec, west_sec) == 8);
static_assert(offsetof(KernelTimesec, dst_sec) == 12);
static_assert(offsetof(TimeOutput, value) == 8);
static_assert(offsetof(TimeOutput, after) == offsetof(TimeOutput, value) + sizeof(std::int64_t));
static_assert(offsetof(InfoOutput, value) == 8);
static_assert(offsetof(InfoOutput, after) == offsetof(InfoOutput, value) + sizeof(KernelTimesec));
static_assert(offsetof(LocalDstOutput, value) == 8);
static_assert(offsetof(LocalDstOutput, after) == offsetof(LocalDstOutput, value) + sizeof(std::int32_t));
static_assert(offsetof(UtcDstOutput, value) == 8);
static_assert(offsetof(UtcDstOutput, after) == offsetof(UtcDstOutput, value) + sizeof(std::uint64_t));

static constexpr std::int64_t PoisonTime = 0x1122334455667788LL;
static constexpr KernelTimesec PoisonInfo{PoisonTime, 0xffff8800u, 32767u};
static constexpr std::int32_t PoisonLocalDst = 0x31415926;
static constexpr std::uint64_t PoisonUtcDst = 0x7fffaaaaffff8888ULL;

static void Require(bool condition, const char* message, std::int64_t seconds, std::int64_t dstFlag, unsigned outputMask) {
    if (!condition) {
        std::fprintf(stderr, "%s (seconds=%lld, dstFlag=%lld, outputMask=%u)\n", message,
            static_cast<long long>(seconds), static_cast<long long>(dstFlag), outputMask);
        std::abort();
    }
}

template <typename TValue>
static bool CanariesMatch(const GuardedOutput<TValue>& output) {
    for (const auto byte : output.before)
        if (byte != 0xa5) return false;
    for (const auto byte : output.after)
        if (byte != 0x3c) return false;
    return true;
}

static bool Equal(const KernelTimesec& left, const KernelTimesec& right) {
    return left.t == right.t && left.west_sec == right.west_sec && left.dst_sec == right.dst_sec;
}

static void CheckLocalToUtc(std::int64_t seconds, std::int64_t dstFlag, unsigned outputMask) {
    TimeOutput utc(PoisonTime);
    InfoOutput info(PoisonInfo);
    LocalDstOutput dst(PoisonLocalDst);
    Require(sceKernelConvertLocaltimeToUtc(seconds, dstFlag,
        (outputMask & 1u) ? &utc.value : nullptr,
        (outputMask & 2u) ? &info.value : nullptr,
        (outputMask & 4u) ? &dst.value : nullptr) == 0,
        "LocaltimeToUtc failed", seconds, dstFlag, outputMask);
    Require(CanariesMatch(utc) && CanariesMatch(info) && CanariesMatch(dst),
        "LocaltimeToUtc crossed an output boundary", seconds, dstFlag, outputMask);
    Require(utc.value == ((outputMask & 1u) ? seconds : PoisonTime),
        "LocaltimeToUtc returned the wrong UTC seconds or changed an omitted output", seconds, dstFlag, outputMask);
    const KernelTimesec expected = (outputMask & 2u) ? KernelTimesec{seconds, 0u, 0u} : PoisonInfo;
    Require(Equal(info.value, expected),
        "LocaltimeToUtc did not write all 16 time-info bytes or changed an omitted output", seconds, dstFlag, outputMask);
    Require(dst.value == ((outputMask & 4u) ? 0 : PoisonLocalDst),
        "LocaltimeToUtc returned the wrong four-byte DST value or changed an omitted output", seconds, dstFlag, outputMask);
}

static void CheckUtcToLocal(std::int64_t seconds, unsigned outputMask) {
    TimeOutput local(PoisonTime);
    InfoOutput info(PoisonInfo);
    UtcDstOutput dst(PoisonUtcDst);
    Require(sceKernelConvertUtcToLocaltime(seconds,
        (outputMask & 1u) ? &local.value : nullptr,
        (outputMask & 2u) ? &info.value : nullptr,
        (outputMask & 4u) ? &dst.value : nullptr) == 0,
        "UtcToLocaltime failed", seconds, 0, outputMask);
    Require(CanariesMatch(local) && CanariesMatch(info) && CanariesMatch(dst),
        "UtcToLocaltime crossed an output boundary", seconds, 0, outputMask);
    Require(local.value == ((outputMask & 1u) ? seconds : PoisonTime),
        "UtcToLocaltime returned the wrong local seconds or changed an omitted output", seconds, 0, outputMask);
    const KernelTimesec expected = (outputMask & 2u) ? KernelTimesec{seconds, 0u, 0u} : PoisonInfo;
    Require(Equal(info.value, expected),
        "UtcToLocaltime did not write all 16 time-info bytes or changed an omitted output", seconds, 0, outputMask);
    Require(dst.value == ((outputMask & 4u) ? 0u : PoisonUtcDst),
        "UtcToLocaltime did not write the full eight-byte DST value or changed an omitted output", seconds, 0, outputMask);
}

static void CheckRoundTrip(std::int64_t seconds, std::int64_t dstFlag) {
    TimeOutput utc(PoisonTime);
    InfoOutput localInfo(PoisonInfo);
    LocalDstOutput localDst(PoisonLocalDst);
    Require(sceKernelConvertLocaltimeToUtc(seconds, dstFlag, &utc.value, &localInfo.value, &localDst.value) == 0,
        "Local-to-UTC round trip failed", seconds, dstFlag, 7);
    TimeOutput local(PoisonTime);
    InfoOutput utcInfo(PoisonInfo);
    UtcDstOutput utcDst(PoisonUtcDst);
    Require(sceKernelConvertUtcToLocaltime(utc.value, &local.value, &utcInfo.value, &utcDst.value) == 0,
        "UTC-to-local round trip failed", seconds, dstFlag, 7);
    Require(CanariesMatch(utc) && CanariesMatch(local) && CanariesMatch(localInfo)
        && CanariesMatch(utcInfo) && CanariesMatch(localDst) && CanariesMatch(utcDst),
        "Round trip crossed an output boundary", seconds, dstFlag, 7);
    Require(utc.value == seconds && local.value == seconds,
        "UTC round trip changed signed seconds", seconds, dstFlag, 7);
    Require(Equal(localInfo.value, KernelTimesec{seconds, 0u, 0u}) && Equal(localInfo.value, utcInfo.value)
        && localDst.value == 0 && utcDst.value == 0,
        "Round trip returned inconsistent time-info or DST state", seconds, dstFlag, 7);
    InfoOutput reverseInfo(PoisonInfo);
    LocalDstOutput reverseDst(PoisonLocalDst);
    TimeOutput reverseUtc(PoisonTime);
    Require(sceKernelConvertLocaltimeToUtc(local.value, dstFlag, &reverseUtc.value, &reverseInfo.value, &reverseDst.value) == 0
        && reverseUtc.value == seconds && Equal(reverseInfo.value, KernelTimesec{seconds, 0u, 0u}) && reverseDst.value == 0,
        "Repeated conversion changed UTC seconds or DST state", seconds, dstFlag, 7);
    Require(CanariesMatch(reverseInfo) && CanariesMatch(reverseDst) && CanariesMatch(reverseUtc),
        "Repeated conversion crossed an output boundary", seconds, dstFlag, 7);
}

int main() {
#ifdef _WIN32
    Require(_putenv_s("TZ", "UTC-2") == 0, "Failed to set the host time zone", 0, 0, 0);
    _tzset();
#else
    Require(setenv("TZ", "UTC-2", 1) == 0, "Failed to set the host time zone", 0, 0, 0);
    tzset();
#endif
    constexpr std::array<std::int64_t, 12> seconds{
        std::numeric_limits<std::int64_t>::min(), -4294967297LL, -86401, -1, 0, 1,
        946684800, 1709164800, 2147483648LL, 4294967297LL, 0x005ca3ff1b64d310LL,
        std::numeric_limits<std::int64_t>::max()
    };
    constexpr std::array<std::int64_t, 3> dstFlags{-1, 0, 1};
    for (const auto value : seconds) {
        for (unsigned outputMask = 0; outputMask < 8; ++outputMask) {
            CheckUtcToLocal(value, outputMask);
            for (const auto dstFlag : dstFlags)
                CheckLocalToUtc(value, dstFlag, outputMask);
        }
        for (const auto dstFlag : dstFlags)
            CheckRoundTrip(value, dstFlag);
    }
    std::puts("Native kernel time ABI: complete outputs, boundaries, signed seconds, optional outputs and round trips passed");
}
