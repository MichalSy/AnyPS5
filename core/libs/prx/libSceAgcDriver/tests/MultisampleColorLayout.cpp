#include "prx/libSceAgcDriver/Graphics/include/MultisampleColorLayout.hpp"
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

using AgcDriver::Graphics::MultisampleColorLayout;

void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

template<typename TAction>
void reject(TAction action) {
    try { action(); }
    catch (const std::runtime_error&) { return; }
    throw std::runtime_error("expected multisample color layout rejection");
}

std::size_t oracle(std::uint32_t width, std::uint32_t x, std::uint32_t y, std::uint32_t sample) {
    constexpr std::array<std::array<std::uint32_t, 3>, 16> sourceBits{{
        {0, 0, 0}, {0, 0, 0}, {1, 0, 0}, {2, 0, 0},
        {0, 1, 0}, {0, 2, 0}, {0, 4, 0}, {4, 0, 0},
        {8, 8, 0}, {16, 16, 0}, {64, 32, 0}, {32, 64, 4},
        {0, 8, 0}, {16, 0, 0}, {0, 64, 1}, {0, 128, 2}
    }};
    std::size_t offset = 0;
    for (std::size_t bit = 0; bit < sourceBits.size(); ++bit) {
        const auto& masks = sourceBits[bit];
        const auto parity = (std::popcount(x & masks[0]) + std::popcount(y & masks[1]) + std::popcount(sample & masks[2])) & 1;
        offset |= static_cast<std::size_t>(parity) << bit;
    }
    return (static_cast<std::size_t>(y / 64u) * ((width + 31u) / 32u) + x / 32u) * 65536u + offset;
}

void checkGolden() {
    const MultisampleColorLayout layout(129, 257, 4, 8);
    require(layout.Bytes() == 1638400u && layout.LinearBytes() == 1060896u && layout.Alignment() == 65536u, "multisample extent or alignment mismatch");
    constexpr std::array<std::array<std::size_t, 4>, 25> expected{{
        {0,0,0,0},
        {0,0,1,16384},
        {0,0,2,32768},
        {0,0,3,49152},
        {0,0,4,2048},
        {0,0,5,18432},
        {0,0,6,34816},
        {0,0,7,51200},
        {1,0,0,4},
        {0,1,0,16},
        {4,0,0,128},
        {8,0,0,256},
        {16,0,0,8704},
        {0,8,0,4352},
        {0,16,0,512},
        {0,32,0,1024},
        {32,0,0,67584},
        {64,0,0,132096},
        {96,0,0,199680},
        {0,64,0,346112},
        {0,128,0,688128},
        {0,192,0,1034240},
        {32,64,7,428032},
        {128,256,7,1624064},
        {127,255,3,1192188},
    }};
    for (const auto& point : expected) require(layout.Offset(static_cast<std::uint32_t>(point[0]), static_cast<std::uint32_t>(point[1]), static_cast<std::uint32_t>(point[2])) == point[3], "AMD multisample golden offset mismatch");
    const MultisampleColorLayout screen(3840, 2160, 4, 8);
    require(screen.Bytes() == 267386880u && screen.LinearBytes() == 265420800u, "multisample screen allocation mismatch");
}

void checkMapping(std::uint32_t width, std::uint32_t height) {
    const MultisampleColorLayout layout(width, height, 4, 8);
    std::vector<bool> visited(layout.Bytes() / 4u);
    std::vector<std::byte> bytes(layout.Bytes(), std::byte{0x5a});
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            for (std::uint32_t sample = 0; sample < 8u; ++sample) {
                const auto offset = layout.Offset(x, y, sample);
                require(offset == oracle(width, x, y, sample), "multisample layout differs from AMD pattern");
                require(offset % 4u == 0 && offset + 4u <= bytes.size() && !visited[offset / 4u], "multisample mapping aliases or escapes storage");
                visited[offset / 4u] = true;
                const auto value = (y * width + x) * 8u + sample + 1u;
                std::memcpy(bytes.data() + offset, &value, 4u);
            }
        }
    }
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            for (std::uint32_t sample = 0; sample < 8u; ++sample) {
                std::uint32_t value = 0;
                std::memcpy(&value, bytes.data() + oracle(width, x, y, sample), 4u);
                require(value == (y * width + x) * 8u + sample + 1u, "multisample guest payload or sample identity changed");
            }
        }
    }
    for (std::size_t i = 0; i < bytes.size(); ++i) if (!visited[i / 4u]) require(bytes[i] == std::byte{0x5a}, "multisample padding was overwritten");
    if (width % 32u == 0 && height % 64u == 0) {
        for (const bool seen : visited) require(seen, "multisample block has unreachable sample bytes");
    }
}

void checkRejections() {
    reject([] { MultisampleColorLayout(0, 1, 4, 8); });
    reject([] { MultisampleColorLayout(1, 0, 4, 8); });
    reject([] { MultisampleColorLayout(16385, 1, 4, 8); });
    reject([] { MultisampleColorLayout(1, 16385, 4, 8); });
    for (const auto element : {0u, 1u, 2u, 8u, 16u}) reject([&] { MultisampleColorLayout(1, 1, element, 8); });
    for (const auto samples : {0u, 1u, 2u, 4u, 16u}) reject([&] { MultisampleColorLayout(1, 1, 4, samples); });
    const MultisampleColorLayout layout(37, 69, 4, 8);
    require(layout.Bytes() == 262144u && layout.LinearBytes() == 81696u, "multisample non-block extent mismatch");
    reject([&] { layout.Offset(37, 0, 0); });
    reject([&] { layout.Offset(0, 69, 0); });
    reject([&] { layout.Offset(0, 0, 8); });
}

}

int main() {
    try {
        checkGolden();
        checkMapping(32, 64);
        checkMapping(96, 192);
        checkMapping(37, 69);
        checkRejections();
        std::cout << "AMD SW_64KB_R_X 8-fragment color layout passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
