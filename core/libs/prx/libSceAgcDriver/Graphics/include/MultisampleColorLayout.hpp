#pragma once
#include <cstddef>
#include <cstdint>

namespace AgcDriver::Graphics {

class MultisampleColorLayout {
public:
    MultisampleColorLayout(std::uint32_t width, std::uint32_t height, std::uint32_t elementBytes, std::uint32_t sampleCount);
    std::size_t Bytes() const { return bytes; }
    std::size_t LinearBytes() const { return linearBytes; }
    std::size_t Alignment() const { return 65536u; }
    std::size_t Offset(std::uint32_t x, std::uint32_t y, std::uint32_t sample) const;

private:
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t blocksPerRow;
    std::size_t bytes;
    std::size_t linearBytes;
};

}
