#include "prx/libc/include/general/VabiMacros.hpp"
#include <cstddef>
#include <cstdint>

extern "C" int* APS5_VABI __error_nid_postfix();

struct GuestSignalSet {
    std::uint32_t bits[4];
};
static_assert(sizeof(GuestSignalSet) == 16);

namespace {
constexpr int MaximumSignal = 128;
constexpr int InvalidArgument = 22;
constexpr int BadAddress = 14;

bool ValidSignal(int signal) { return signal > 0 && signal <= MaximumSignal; }
std::size_t WordIndex(int signal) { return static_cast<std::size_t>(signal - 1) >> 5; }
std::uint32_t Bit(int signal) { return 1u << ((signal - 1) & 31); }
int RejectSignal(int error = InvalidArgument) {
    *__error_nid_postfix() = error;
    return -1;
}
}

extern "C" {
int APS5_VABI sigemptyset_nid_postfix(GuestSignalSet* set) {
    if (set == nullptr) return RejectSignal(BadAddress);
    for (auto& word : set->bits) word = 0;
    return 0;
}

int APS5_VABI sigfillset_nid_postfix(GuestSignalSet* set) {
    if (set == nullptr) return RejectSignal(BadAddress);
    for (auto& word : set->bits) word = ~0u;
    return 0;
}

int APS5_VABI sigaddset_nid_postfix(GuestSignalSet* set, int signal) {
    if (set == nullptr) return RejectSignal(BadAddress);
    if (!ValidSignal(signal)) return RejectSignal();
    set->bits[WordIndex(signal)] |= Bit(signal);
    return 0;
}

int APS5_VABI sigdelset_nid_postfix(GuestSignalSet* set, int signal) {
    if (set == nullptr) return RejectSignal(BadAddress);
    if (!ValidSignal(signal)) return RejectSignal();
    set->bits[WordIndex(signal)] &= ~Bit(signal);
    return 0;
}

int APS5_VABI sigismember_nid_postfix(const GuestSignalSet* set, int signal) {
    if (set == nullptr) return RejectSignal(BadAddress);
    if (!ValidSignal(signal)) return RejectSignal();
    return (set->bits[WordIndex(signal)] & Bit(signal)) != 0 ? 1 : 0;
}
}
