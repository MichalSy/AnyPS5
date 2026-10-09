#include "prx/libSceAgcDriver/Graphics/include/MultisampleColorLayout.hpp"
#include <limits>
#include <stdexcept>

namespace AgcDriver::Graphics {
namespace {

void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

}

MultisampleColorLayout::MultisampleColorLayout(std::uint32_t width, std::uint32_t height, std::uint32_t elementBytes, std::uint32_t sampleCount)
    : width(width), height(height), blocksPerRow((width + 31u) / 32u), bytes(0), linearBytes(0) {
    require(width != 0 && height != 0 && width <= 16384u && height <= 16384u, "AGC graphics: invalid multisample color extent");
    require(elementBytes == 4u && sampleCount == 8u, "AGC graphics: unsupported multisample color layout");
    const auto paddedBytes = static_cast<std::uint64_t>(blocksPerRow) * ((height + 63u) / 64u) * 65536u;
    const auto visibleBytes = static_cast<std::uint64_t>(width) * height * 32u;
    require(paddedBytes <= std::numeric_limits<std::size_t>::max() && visibleBytes <= std::numeric_limits<std::size_t>::max(), "AGC graphics: multisample color size overflow");
    bytes = static_cast<std::size_t>(paddedBytes);
    linearBytes = static_cast<std::size_t>(visibleBytes);
}

std::size_t MultisampleColorLayout::Offset(std::uint32_t x, std::uint32_t y, std::uint32_t sample) const {
    require(x < width && y < height && sample < 8u, "AGC graphics: multisample color coordinate out of range");
    const auto xOffset = ((x << 2u) & 0x000cu) ^ ((x << 5u) & 0x0380u) ^ ((x << 4u) & 0x0400u) ^ ((x << 6u) & 0x0800u) ^ ((x << 9u) & 0x2000u);
    const auto yOffset = ((y << 4u) & 0x0070u) ^ ((y << 5u) & 0x0f00u) ^ ((y << 9u) & 0x1000u) ^ ((y << 8u) & 0xc000u);
    const auto sampleOffset = ((sample << 14u) & 0xc000u) ^ ((sample << 9u) & 0x0800u);
    const auto block = static_cast<std::size_t>(y / 64u) * blocksPerRow + x / 32u;
    return block * 65536u + (xOffset ^ yOffset ^ sampleOffset);
}

}
