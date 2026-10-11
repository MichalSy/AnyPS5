#include "prx/libSceAgcDriver/Graphics/include/MultisampleColorLayout.hpp"
#include <bit>
#include <limits>
#include <stdexcept>

namespace AgcDriver::Graphics {
namespace {

void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

}

MultisampleColorLayout::MultisampleColorLayout(std::uint32_t width, std::uint32_t height, std::uint32_t elementBytes, std::uint32_t sampleCount, std::uint32_t pipeBankXor)
    : width(width), height(height), elementBytes(elementBytes), sampleCount(sampleCount), pipeBankXor(pipeBankXor), bytes(0), linearBytes(0) {
    require(width != 0 && height != 0 && width <= 16384u && height <= 16384u, "AGC graphics: invalid multisample color extent");
    require((elementBytes == 4u || elementBytes == 8u) && (sampleCount == 2u || sampleCount == 4u || sampleCount == 8u), "AGC graphics: unsupported multisample color layout");
    require(pipeBankXor < 65536u && pipeBankXor % elementBytes == 0, "AGC graphics: invalid multisample pipe/bank XOR");
    const auto samplesLog2 = static_cast<std::uint32_t>(std::countr_zero(sampleCount));
    const auto elementsLog2 = 16u - static_cast<std::uint32_t>(std::countr_zero(elementBytes)) - samplesLog2;
    const auto widthLog2 = (elementsLog2 + ((samplesLog2 & 1u) == 0u ? 1u : 0u)) / 2u;
    blockWidth = 1u << widthLog2;
    blockHeight = 1u << (elementsLog2 - widthLog2);
    blocksPerRow = (width + blockWidth - 1u) / blockWidth;
    const auto paddedBytes = static_cast<std::uint64_t>(blocksPerRow) * ((height + blockHeight - 1u) / blockHeight) * 65536u;
    const auto visibleBytes = static_cast<std::uint64_t>(width) * height * elementBytes * sampleCount;
    require(paddedBytes <= std::numeric_limits<std::size_t>::max() && visibleBytes <= std::numeric_limits<std::size_t>::max(), "AGC graphics: multisample color size overflow");
    bytes = static_cast<std::size_t>(paddedBytes);
    linearBytes = static_cast<std::size_t>(visibleBytes);
}

std::size_t MultisampleColorLayout::Offset(std::uint32_t x, std::uint32_t y, std::uint32_t sample) const {
    require(x < width && y < height && sample < sampleCount, "AGC graphics: multisample color coordinate out of range");
    std::uint32_t xOffset;
    std::uint32_t yOffset;
    std::uint32_t sampleOffset;
    if (elementBytes == 4u) {
        xOffset = ((x << 2u) & 0x000cu) ^ ((x << 5u) & 0x0380u) ^ ((x << 4u) & 0x0400u) ^ ((x << 6u) & 0x0800u) ^ ((x << 9u) & 0x2000u);
        yOffset = ((y << 4u) & 0x0070u) ^ ((y << 5u) & 0x0f00u) ^ ((y << 9u) & 0x1000u) ^ ((y << 8u) & 0xc000u);
        sampleOffset = sampleCount == 2u ? (sample << 15u) : ((sample << 14u) & 0xc000u) ^ ((sample << 9u) & 0x0800u);
    } else {
        xOffset = ((x << 3u) & 0x0008u) ^ ((x << 4u) & 0x0460u) ^ ((x << 5u) & 0x0300u) ^ ((x << 6u) & 0x0800u) ^ ((x << 10u) & 0x2000u);
        yOffset = ((y << 4u) & 0x0010u) ^ ((y << 5u) & 0x0f00u) ^ ((y << 6u) & 0x0080u) ^ ((y << 9u) & 0x8000u) ^ ((y << 10u) & 0x5000u);
        sampleOffset = ((sample << 15u) & 0x8000u) ^ (sampleCount == 8u ? ((sample << 8u) & 0x0400u) ^ ((sample << 10u) & 0x0800u) : ((sample << 9u) & 0x0400u));
    }
    const auto block = static_cast<std::size_t>(y / blockHeight) * blocksPerRow + x / blockWidth;
    return block * 65536u + (xOffset ^ yOffset ^ sampleOffset ^ pipeBankXor);
}

}
