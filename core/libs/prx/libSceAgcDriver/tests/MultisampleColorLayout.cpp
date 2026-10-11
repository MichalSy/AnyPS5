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

std::size_t oracle(std::uint32_t width, std::uint32_t x, std::uint32_t y, std::uint32_t sample, std::uint32_t elementBytes = 4u, std::uint32_t samples = 8u, std::uint32_t pipeBankXor = 0u) {
    constexpr std::array<std::array<std::uint32_t, 16>, 6> sourceBits{{
        {0,0,1,2,0x1000,0x2000,0x4000,4,0x8008,0x10010,0x20040,0x40020,0x8000,16,0x40000,0x1080000},
        {0,0,1,2,0x1000,0x2000,0x4000,4,0x8008,0x10010,0x20040,0x40020,0x8000,16,0x1040000,0x2080000},
        {0,0,1,2,0x1000,0x2000,0x4000,4,0x8008,0x10010,0x20040,0x4040020,0x8000,16,0x1040000,0x2080000},
        {0,0,0,1,0x1000,2,4,0x2000,0x8008,0x10010,0x20040,0x40020,0x4000,8,0x10000,0x1040000},
        {0,0,0,1,0x1000,2,4,0x2000,0x8008,0x10010,0x2020040,0x40020,0x4000,8,0x10000,0x1040000},
        {0,0,0,1,0x1000,2,4,0x2000,0x8008,0x10010,0x4020040,0x2040020,0x4000,8,0x10000,0x1040000}
    }};
    constexpr std::array<std::array<std::uint32_t, 2>, 6> blocks{{{64,128},{64,64},{32,64},{64,64},{64,32},{32,32}}};
    const auto index = (elementBytes == 8u ? 3u : 0u) + static_cast<std::uint32_t>(std::countr_zero(samples)) - 1u;
    const auto& masks = sourceBits[index];
    const auto input = (x & 0xfffu) | ((y & 0xfffu) << 12u) | (sample << 24u);
    std::size_t offset = pipeBankXor;
    for (std::size_t bit = 0; bit < masks.size(); ++bit) offset ^= static_cast<std::size_t>(std::popcount(input & masks[bit]) & 1u) << bit;
    const auto block = blocks[index];
    return (static_cast<std::size_t>(y / block[1]) * ((width + block[0] - 1u) / block[0]) + x / block[0]) * 65536u + offset;
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

void checkMapping(std::uint32_t width, std::uint32_t height, std::uint32_t elementBytes = 4u, std::uint32_t samples = 8u, std::uint32_t pipeBankXor = 0u) {
    const MultisampleColorLayout layout(width, height, elementBytes, samples, pipeBankXor);
    std::vector<bool> visited(layout.Bytes() / elementBytes);
    std::vector<std::byte> bytes(layout.Bytes(), std::byte{0x5a});
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            for (std::uint32_t sample = 0; sample < samples; ++sample) {
                const auto offset = layout.Offset(x, y, sample);
                require(offset == oracle(width, x, y, sample, elementBytes, samples, pipeBankXor), "multisample layout differs from AMD pattern");
                require(offset % elementBytes == 0 && offset + elementBytes <= bytes.size() && !visited[offset / elementBytes], "multisample mapping aliases or escapes storage");
                visited[offset / elementBytes] = true;
                const std::uint64_t value = (static_cast<std::uint64_t>(y) * width + x) * samples + sample + 1u;
                std::memcpy(bytes.data() + offset, &value, elementBytes);
            }
        }
    }
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            for (std::uint32_t sample = 0; sample < samples; ++sample) {
                std::uint64_t value = 0;
                std::memcpy(&value, bytes.data() + oracle(width, x, y, sample, elementBytes, samples, pipeBankXor), elementBytes);
                require(value == (static_cast<std::uint64_t>(y) * width + x) * samples + sample + 1u, "multisample guest payload or sample identity changed");
            }
        }
    }
    for (std::size_t i = 0; i < bytes.size(); ++i) if (!visited[i / elementBytes]) require(bytes[i] == std::byte{0x5a}, "multisample padding was overwritten");
    if (layout.Bytes() == layout.LinearBytes()) {
        for (const bool seen : visited) require(seen, "multisample block has unreachable sample bytes");
    }
}

void checkRejections() {
    reject([] { MultisampleColorLayout(0, 1, 4, 8); });
    reject([] { MultisampleColorLayout(1, 0, 4, 8); });
    reject([] { MultisampleColorLayout(16385, 1, 4, 8); });
    reject([] { MultisampleColorLayout(1, 16385, 4, 8); });
    for (const auto element : {0u, 1u, 2u, 16u}) reject([&] { MultisampleColorLayout(1, 1, element, 8); });
    for (const auto samples : {0u, 1u, 16u}) reject([&] { MultisampleColorLayout(1, 1, 4, samples); });
    reject([] { MultisampleColorLayout(1, 1, 4, 4, 65536u); });
    reject([] { MultisampleColorLayout(1, 1, 8, 4, 4u); });
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
        for (const auto element : {4u, 8u}) {
            for (const auto samples : {2u, 4u, 8u}) {
                checkMapping(128, 128, element, samples);
                checkMapping(73, 139, element, samples, 0x5600u);
            }
        }
        require(MultisampleColorLayout(1536, 1536, 4, 4).Bytes() == 0x2400000u, "Hades four-sample RGBA8 footprint differs from addrlib");
        require(MultisampleColorLayout(960, 540, 8, 4).Bytes() == 0xff0000u, "Hades four-sample RGBA16F footprint differs from addrlib");
        checkRejections();
        std::cout << "AMD SW_64KB_R_X two, four and eight-fragment color layouts passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
