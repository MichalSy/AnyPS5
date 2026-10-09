#ifndef ANYPS5_TESTS_ALIGNED_BYTE_ARRAY_HPP
#define ANYPS5_TESTS_ALIGNED_BYTE_ARRAY_HPP

#include <array>
#include <cstddef>
#include <memory>

template <std::size_t TSize, std::size_t TAlignment>
auto MakeAlignedByteArray() {
    struct alignas(TAlignment) Storage : std::array<std::byte, TSize> {};
    return std::make_unique<Storage>();
}

#endif
