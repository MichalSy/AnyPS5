#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <iostream>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;
using Pixel = std::pair<std::uint32_t, std::uint32_t>;
constexpr std::uint32_t Width = 64;
constexpr std::uint32_t Height = 16;
constexpr std::uint32_t WaveSize = 64;
constexpr std::byte Kept{0x40};
alignas(16384) std::array<std::byte, 16384> Pixels{};
alignas(16384) std::array<std::byte, 65536> Depth{};
alignas(16384) std::array<std::byte, 65536> Stencil{};
alignas(256) std::array<std::array<float, 4>, 3> Vertices{};

alignas(256) constexpr std::array<std::uint32_t, 6> VertexCode{
    0xe0382000, 0x80000005, 0xbf8c3f70, 0xf80008cf, 0x03020100, 0xbf810000,
};
alignas(256) constexpr std::array<std::uint32_t, 4> PixelCode{
    0x7e0e02f2, 0xf800180f, 0x07070707, 0xbf810000,
};
constexpr std::array<std::array<float, 2>, 3> Partial{{{1.2f, 10.2f}, {4.6f, 10.2f}, {1.2f, 13.6f}}};
constexpr std::array<std::array<float, 2>, 3> Full{{{0.0f, 0.0f}, {Width * 2.0f, 0.0f}, {0.0f, Height * 2.0f}}};
const std::set<Pixel> Covered{{1, 10}, {2, 10}, {3, 10}, {1, 11}, {2, 11}, {1, 12}};
const std::set<Pixel> Scissored{{1, 10}, {1, 11}, {1, 12}};

class GuestBackings {
public:
    GuestBackings() {
        GuestAllocations::Mutation mutation;
        mutation.Add(Pixels.data(), Pixels.size(), true, true);
        mutation.Add(Depth.data(), Depth.size(), true, true);
        mutation.Add(Stencil.data(), Stencil.size(), true, true);
    }

    ~GuestBackings() {
        GuestAllocations::Mutation mutation;
        mutation.Remove(Stencil.data());
        mutation.Remove(Depth.data());
        mutation.Remove(Pixels.data());
    }

    GuestBackings(const GuestBackings&) = delete;
    GuestBackings& operator=(const GuestBackings&) = delete;
};

AgcDriver::QueueState Queue() {
    AgcDriver::QueueState queue;
    queue.userConfig[0x242] = 4;
    queue.context = {
        {0x000, 0}, {0x002, 0}, {0x007, ((Height - 1u) << 16u) | (Width - 1u)},
        {0x00a, 3}, {0x00b, std::bit_cast<std::uint32_t>(0.25f)}, {0x010, 3}, {0x011, 1},
        {0x10b, 0}, {0x10c, 0x00ffff00}, {0x10d, 0x00ffff00},
        {0x2d5, 0x2000}, {0x1b6, 0}, {0x207, 0}, {0x200, 0x007007f1}, {0x203, 0x800},
        {0x2dc, 0xaa00}, {0x2f8, 0}, {0x292, 2}, {0x293, 0},
        {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa},
        {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff},
        {0x206, 0x43f}, {0x204, 0x80000}, {0x205, 0x240},
        {0x8e, 0xf}, {0x8f, 0xf}, {0x202, 0xcc0010},
        {0x1c4, 0}, {0x1c5, 9}, {0x1c3, 4}, {0x31c, 0x28028},
        {0x31b, 0}, {0x31d, 0}, {0x3b0, ((Width - 1u) << 14u) | (Height - 1u)},
        {0x3b8, 0x9000000}, {0x1e0, 0}, {0x1b3, 2}, {0x1b4, 2},
        {0xc, 0}, {0xd, (Height << 16u) | Width},
        {0x81, 0x80000000}, {0x82, (Height << 16u) | Width},
        {0x90, 0x80000000}, {0x91, (Height << 16u) | Width},
        {0x94, 0x80000000}, {0x95, (Height << 16u) | Width}
    };
    const auto bind = [&](const void* memory, std::uint32_t low, std::uint32_t high) {
        const auto address = reinterpret_cast<std::uintptr_t>(memory);
        queue.context[low] = static_cast<std::uint32_t>(address >> 8u);
        queue.context[high] = static_cast<std::uint32_t>(address >> 40u);
    };
    bind(Pixels.data(), 0x318, 0x390);
    bind(Depth.data(), 0x012, 0x01a);
    bind(Depth.data(), 0x014, 0x01c);
    bind(Stencil.data(), 0x013, 0x01b);
    bind(Stencil.data(), 0x015, 0x01d);
    queue.context[0x10f] = std::bit_cast<std::uint32_t>(Width * 0.5f);
    queue.context[0x110] = std::bit_cast<std::uint32_t>(Width * 0.5f);
    queue.context[0x111] = std::bit_cast<std::uint32_t>(Height * -0.5f);
    queue.context[0x112] = std::bit_cast<std::uint32_t>(Height * 0.5f);
    queue.context[0x113] = std::bit_cast<std::uint32_t>(1.0f);
    queue.context[0x114] = 0;
    queue.context[0xb4] = 0;
    queue.context[0xb5] = std::bit_cast<std::uint32_t>(1.0f);
    return queue;
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t stride, std::uint32_t count) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (stride << 16u), count, 0x01016facu};
}

void Draw(AgcDriver::VulkanDevice& device, const AgcDriver::Graphics::State& state, const std::array<std::array<float, 2>, 3>& corners) {
    for (std::size_t index = 0; index < corners.size(); ++index) {
        Vertices[index] = {corners[index][0] * 2.0f / Width - 1.0f, 1.0f - corners[index][1] * 2.0f / Height, 0.25f, 1.0f};
    }
    const auto target = device.Target();
    std::vector<std::uint32_t> vertexUserData(4, 0u);
    const auto vertexBuffer = BufferDescriptor(Vertices.data(), 16u, static_cast<std::uint32_t>(Vertices.size()));
    std::copy(vertexBuffer.begin(), vertexBuffer.end(), vertexUserData.begin());
    const std::array<ShaderRecompiler::MemoryRegion, 1> vertexMemory{{{reinterpret_cast<std::uintptr_t>(VertexCode.data()), std::as_bytes(std::span(VertexCode))}}};
    ShaderRecompiler::RecompileRequest vertex{
        {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(VertexCode.data()), VertexCode, 0, {}},
        {WaveSize, 0, vertexUserData, std::nullopt, std::nullopt, ShaderRecompiler::ShaderVertexStageInfo{}, vertexMemory},
        target, {0, 0, 0, 64}
    };
    vertex.useCache = false;
    const auto vertexResult = ShaderRecompiler::Recompile(vertex);
    const auto vertexPush = static_cast<std::uint32_t>(vertexResult.pushConstants.size());
    ShaderRecompiler::ShaderPixelStageInfo pixel{};
    pixel.inputAddr = ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionX) | ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionY);
    pixel.posX = pixel.posY = true;
    pixel.targetOutputMode[0] = 9;
    pixel.targetExportMapping.fill(0xe4u);
    const std::vector<std::uint32_t> pixelUserData(8, 0u);
    const std::array<ShaderRecompiler::MemoryRegion, 1> pixelMemory{{{reinterpret_cast<std::uintptr_t>(PixelCode.data()), std::as_bytes(std::span(PixelCode))}}};
    ShaderRecompiler::RecompileRequest fragment{
        {ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(PixelCode.data()), PixelCode, 0, {}},
        {WaveSize, 0, pixelUserData, std::nullopt, pixel, std::nullopt, pixelMemory},
        target, {0, 0, vertexPush, 128 - vertexPush}
    };
    fragment.useCache = false;
    const auto pixelResult = ShaderRecompiler::Recompile(fragment);
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{
        {ShaderStage::Vertex, &vertexResult, 0}, {ShaderStage::Fragment, &pixelResult, vertexPush}
    }};
    const AgcDriver::Pm4::DrawParameters draw{0, static_cast<std::uint32_t>(Vertices.size()), 0, 1, 0, false};
    device.Draw(state, draw, shaders);
    device.WaitIdle();
}

void Check(AgcDriver::VulkanDevice& device, const AgcDriver::QueueState& queue, std::uint32_t reference) {
    auto verify = queue;
    verify.context[0x000] = 0;
    verify.context[0x200] = 0x002002a3;
    verify.context[0x10c] = verify.context[0x10d] = 0x00ffff00u | reference;
    Pixels.fill(Kept);
    Draw(device, AgcDriver::Graphics::DecodeState(verify), Full);
    for (std::uint32_t y = 0; y < Height; ++y) {
        for (std::uint32_t x = 0; x < Width; ++x) {
            const Pixel pixel{x, y};
            const auto stencil = Scissored.contains(pixel) ? 9u : Covered.contains(pixel) ? 7u : 3u;
            const auto expected = stencil == reference ? std::byte{255} : Kept;
            for (std::uint32_t channel = 0; channel < 4; ++channel) {
                const auto actual = Pixels[(y * Width + x) * 4u + channel];
                Require(actual == expected, "stencil " + std::to_string(reference) + " and preserved depth: pixel (" + std::to_string(x) + ", " + std::to_string(y) + ") channel " + std::to_string(channel) + " is " + std::to_string(static_cast<unsigned>(actual)) + ", expected " + std::to_string(static_cast<unsigned>(expected)));
            }
        }
    }
}

}

int main() {
    try {
        const GuestBackings backings;
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        auto queue = Queue();
        Pixels.fill(Kept);
        auto initial = AgcDriver::Graphics::DecodeState(queue);
        Require(initial.color.extent.width == Width && initial.color.extent.height == Height && initial.color.bytes == Width * Height * 4u, "stencil clear fixture color layout differs from its pixel array");
        Require(initial.depth && initial.depth->extent.width == Width && initial.depth->extent.height == Height, "stencil clear fixture depth extent differs from its color extent");
        Require(Depth.size() == AgcDriver::Graphics::DepthSliceBytes(initial.depth->extent, 4u) && Stencil.size() == AgcDriver::Graphics::DepthSliceBytes(initial.depth->extent, 1u), "stencil clear fixture does not cover its padded depth/stencil slices");
        initial.blend.colorWriteMask = initial.blends[0].colorWriteMask = 0;
        Draw(*device, initial, Full);
        queue.context[0x000] = 0x22;
        queue.context[0x00a] = 7;
        queue.context[0x00b] = std::bit_cast<std::uint32_t>(0.875f);
        auto clear = AgcDriver::Graphics::DecodeState(queue);
        clear.blend.colorWriteMask = clear.blends[0].colorWriteMask = 0;
        Draw(*device, clear, Partial);
        queue.context[0x00a] = 9;
        queue.context[0x91] = (Height << 16u) | 2u;
        clear = AgcDriver::Graphics::DecodeState(queue);
        clear.blend.colorWriteMask = clear.blends[0].colorWriteMask = 0;
        Draw(*device, clear, Partial);
        queue.context[0x91] = (Height << 16u) | Width;
        for (const auto reference : {3u, 7u, 9u}) Check(*device, queue, reference);
        std::puts("stencil clear cached target, triangle coverage, scissor and depth preservation tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
