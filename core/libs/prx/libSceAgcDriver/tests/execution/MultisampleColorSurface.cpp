#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleColorSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::Graphics::ColorTarget;
using ShaderRecompiler::ShaderStage;
constexpr std::uint32_t Width = 37;
constexpr std::uint32_t Height = 69;
constexpr std::size_t StorageBytes = 4u * 65536u;
constexpr std::uint32_t WaveSize = 64;
constexpr VkRect2D Whole{{0, 0}, {Width, Height}};
constexpr VkRect2D Partial{{2, 3}, {11, 13}};
constexpr VkRect2D OnePixel{{2, 2}, {1, 1}};
constexpr std::array<VkSampleLocationEXT, 8> Locations{{
    {1.0f / 16, 5.0f / 16}, {15.0f / 16, 11.0f / 16},
    {9.0f / 16, 3.0f / 16}, {3.0f / 16, 13.0f / 16},
    {5.0f / 16, 1.0f / 16}, {11.0f / 16, 15.0f / 16},
    {13.0f / 16, 7.0f / 16}, {7.0f / 16, 9.0f / 16}
}};
alignas(256) constexpr std::array<std::uint32_t, 6> VertexCode{
    0xe0382000, 0x80000005, 0xbf8c3f70, 0xf80008cf, 0x03020100, 0xbf810000
};
alignas(256) constexpr std::array<std::uint32_t, 11> PixelCode{
    0x7e0802ff, 0x3f800000, 0x7e0a02ff, 0x3f800000,
    0x7e0c02ff, 0x3f800000, 0x7e0e02ff, 0x3f800000,
    0xf800180f, 0x07060504, 0xbf810000
};
alignas(256) constexpr std::array<std::array<float, 4>, 3> Vertices{{
    {-1, -1, 0.5f, 1}, {3, -1, 0.5f, 1}, {-1, 3, 0.5f, 1}
}};

std::size_t Oracle(std::uint32_t x, std::uint32_t y, std::uint32_t sample) {
    constexpr std::array<std::string_view, 16> expressions{
        "0", "0", "X0", "X1", "Y0", "Y1", "Y2", "X2",
        "Z2^X3^Y3", "Z1^X4^Y4", "Z0^Y5^X6", "S2^X5^Y6",
        "Y3", "X4", "S0^Y6", "S1^Y7"
    };
    std::size_t offset = 0;
    for (std::size_t bit = 0; bit < expressions.size(); ++bit) {
        const auto expression = expressions[bit];
        std::uint32_t parity = 0;
        for (std::size_t term = 0; term < expression.size();) {
            if (expression[term] == '0') { ++term; continue; }
            const auto channel = expression[term];
            const auto value = channel == 'X' ? x : channel == 'Y' ? y : channel == 'S' ? sample : 0u;
            const auto sourceBit = static_cast<std::uint32_t>(expression[term + 1] - '0');
            parity ^= (value >> sourceBit) & 1u;
            term += 2;
            if (term < expression.size()) ++term;
        }
        offset |= static_cast<std::size_t>(parity) << bit;
    }
    return (static_cast<std::size_t>(y / 64u) * 2u + x / 32u) * 65536u + offset;
}

class Block {
public:
    Block() {
#ifdef _WIN32
        data = static_cast<std::byte*>(VirtualAlloc(nullptr, StorageBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        data = static_cast<std::byte*>(std::aligned_alloc(65536u, StorageBytes));
#endif
        Require(data != nullptr && Address() % 65536u == 0, "cannot allocate an aligned multisample guest surface");
        GuestAllocations::Mutation mutation;
        mutation.Add(data, StorageBytes, true, true, true);
    }
    ~Block() {
        {
            GuestAllocations::Mutation mutation;
            mutation.Remove(data);
        }
#ifdef _WIN32
        VirtualFree(data, 0, MEM_RELEASE);
#else
        std::free(data);
#endif
    }
    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;
    std::uint64_t Address() const { return reinterpret_cast<std::uintptr_t>(data); }
    void Store(std::size_t offset, std::byte value) {
        data[offset] = value;
        AgcDriver::GuestMemory::MarkWritten(Address() + offset, 1);
    }
    std::byte* data = nullptr;
};

std::vector<std::byte> Seed(Block& block) {
    std::vector<std::byte> bytes(StorageBytes, std::byte{0xa9});
    std::vector<bool> visited(StorageBytes / 4);
    for (std::uint32_t y = 0; y < Height; ++y) {
        for (std::uint32_t x = 0; x < Width; ++x) {
            for (std::uint32_t sample = 0; sample < 8; ++sample) {
                const auto offset = Oracle(x, y, sample);
                Require(offset + 4 <= bytes.size() && !visited[offset / 4], "independent AMD oracle aliases or escapes guest storage");
                visited[offset / 4] = true;
                for (std::uint32_t channel = 0; channel < 4; ++channel)
                    bytes[offset + channel] = static_cast<std::byte>((x * 3u + y * 5u + sample * 17u + channel * 41u) % 201u);
            }
        }
    }
    std::memcpy(block.data, bytes.data(), bytes.size());
    AgcDriver::GuestMemory::MarkWritten(block.Address(), StorageBytes);
    return bytes;
}

ColorTarget Target(const Block& block, VkFormat format, VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_8_BIT) {
    ColorTarget target{};
    target.address = target.surfaceAddress = block.Address();
    target.extent = target.surfaceExtent = {Width, Height};
    target.format = format;
    target.bytes = samples == VK_SAMPLE_COUNT_8_BIT ? StorageBytes : AgcDriver::Graphics::ColorTargetLayout(Width, Height, AgcDriver::Graphics::ColorTileMode::RenderTarget, 4).Bytes();
    target.componentMapping = 0xe4u;
    target.tileMode = AgcDriver::Graphics::ColorTileMode::RenderTarget;
    target.elementBytes = 4;
    target.samples = samples;
    return target;
}

bool Bgra(VkFormat format) {
    return format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
}

std::uint32_t ByteChannel(VkFormat format, std::uint32_t channel) {
    return Bgra(format) && (channel == 0 || channel == 2) ? 2u - channel : channel;
}

void ExpectWrite(std::vector<std::byte>& expected, VkFormat format, VkRect2D region, VkColorComponentFlags mask, std::uint32_t sampleMask = 0xffffffffu) {
    for (std::uint32_t y = 0; y < region.extent.height; ++y) {
        for (std::uint32_t x = 0; x < region.extent.width; ++x) {
            for (std::uint32_t sample = 0; sample < 8; ++sample) {
                if ((sampleMask & (1u << sample)) == 0) continue;
                const auto offset = Oracle(static_cast<std::uint32_t>(region.offset.x) + x, static_cast<std::uint32_t>(region.offset.y) + y, sample);
                for (std::uint32_t channel = 0; channel < 4; ++channel)
                    if ((mask & (1u << channel)) != 0) expected[offset + ByteChannel(format, channel)] = std::byte{0xff};
            }
        }
    }
}

void Draw(AgcDriver::VulkanDevice& device, const ColorTarget& color, VkRect2D region, VkColorComponentFlags mask,
    std::uint32_t sampleMask = 0xffffffffu, bool keepBlend = false) {
    const auto target = device.Target();
    const auto address = reinterpret_cast<std::uintptr_t>(Vertices.data());
    const std::array<std::uint32_t, 4> buffer{
        static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (16u << 16u), 3, 0x01016facu
    };
    const std::vector<std::uint32_t> vertexUser(buffer.begin(), buffer.end());
    const std::array<ShaderRecompiler::MemoryRegion, 1> vertexMemory{{{reinterpret_cast<std::uintptr_t>(VertexCode.data()), std::as_bytes(std::span(VertexCode))}}};
    ShaderRecompiler::RecompileRequest vertex{
        {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(VertexCode.data()), VertexCode, 0, {}},
        {WaveSize, 0, vertexUser, std::nullopt, std::nullopt, ShaderRecompiler::ShaderVertexStageInfo{}, vertexMemory}, target, {0, 0, 0, 64}
    };
    vertex.useCache = false;
    const auto vertexResult = ShaderRecompiler::Recompile(vertex);
    const auto vertexPush = static_cast<std::uint32_t>(vertexResult.pushConstants.size());
    ShaderRecompiler::ShaderPixelStageInfo pixel{};
    pixel.targetOutputMode[0] = 9;
    pixel.targetExportMapping.fill(0xe4u);
    const std::vector<std::uint32_t> pixelUser(8, 0);
    const std::array<ShaderRecompiler::MemoryRegion, 1> pixelMemory{{{reinterpret_cast<std::uintptr_t>(PixelCode.data()), std::as_bytes(std::span(PixelCode))}}};
    ShaderRecompiler::RecompileRequest fragment{
        {ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(PixelCode.data()), PixelCode, 0, {}},
        {WaveSize, 0, pixelUser, std::nullopt, pixel, std::nullopt, pixelMemory}, target, {0, 0, vertexPush, 128 - vertexPush}
    };
    fragment.useCache = false;
    const auto pixelResult = ShaderRecompiler::Recompile(fragment);
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{
        {ShaderStage::Vertex, &vertexResult, 0}, {ShaderStage::Fragment, &pixelResult, vertexPush}
    }};
    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0, WaveSize, WaveSize, std::nullopt, std::nullopt};
    state.colors = {color};
    state.color = color;
    state.hasColorTarget = true;
    state.rasterizationSamples = color.samples;
    state.sampleMask = sampleMask;
    if (color.samples == VK_SAMPLE_COUNT_8_BIT) state.sampleLocations.assign(Locations.begin(), Locations.end());
    state.renderExtent = {Width, Height};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(Height), static_cast<float>(Width), -static_cast<float>(Height), 0, 1};
    state.scissor = region;
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = mask;
    blend.blendEnable = keepBlend ? VK_TRUE : VK_FALSE;
    blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstColorBlendFactor = blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.colorBlendOp = blend.alphaBlendOp = VK_BLEND_OP_ADD;
    state.blends = {blend};
    state.blend = blend;
    const AgcDriver::Pm4::DrawParameters draw{0, 3, 0, 1, 0, false};
    device.Draw(state, draw, shaders);
}

void Check(const Block& block, const std::vector<std::byte>& expected, const std::string& name) {
    Require(expected.size() == StorageBytes, "expected multisample storage must cover visible samples and padding");
    for (std::size_t offset = 0; offset < expected.size(); ++offset) {
        if (block.data[offset] == expected[offset]) continue;
        throw std::runtime_error(name + ": guest byte " + std::to_string(offset) + " is " +
            std::to_string(std::to_integer<unsigned>(block.data[offset])) + ", expected " + std::to_string(std::to_integer<unsigned>(expected[offset])));
    }
}

void Flush(AgcDriver::VulkanDevice& device, const Block& block) {
    device.WaitIdle();
    AgcDriver::Graphics::FlushMultisampleColors(block.Address(), StorageBytes);
    AgcDriver::Graphics::StorageTexture::FlushPending(block.Address(), StorageBytes, nullptr, "multisample test readback");
    device.WaitIdle();
}

std::size_t Padding() {
    std::vector<bool> occupied(StorageBytes / 4);
    for (std::uint32_t y = 0; y < Height; ++y)
        for (std::uint32_t x = 0; x < Width; ++x)
            for (std::uint32_t sample = 0; sample < 8; ++sample) occupied[Oracle(x, y, sample) / 4] = true;
    const auto found = std::find(occupied.begin(), occupied.end(), false);
    Require(found != occupied.end(), "the non-block-aligned surface must contain real padding");
    return static_cast<std::size_t>(found - occupied.begin()) * 4;
}

void FormatTests(AgcDriver::VulkanDevice& device, Block& block, VkFormat format, const std::string& name) {
    auto expected = Seed(block);
    const auto color = Target(block, format);
    Draw(device, color, Whole, 0);
    Flush(device, block);
    Check(block, expected, name + " distinct eight-sample upload/flush and untouched padding");
    Draw(device, color, Partial, VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT);
    const auto conflict = Oracle(4, 5, 3) + ByteChannel(format, 0);
    Require(block.data[conflict] == expected[conflict] && block.data[conflict] != std::byte{0xff}, "the real GPU write must remain pending before the CPU conflict");
    ExpectWrite(expected, format, Partial, VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT);
    block.Store(conflict, std::byte{0x2a});
    expected[conflict] = std::byte{0x2a};
    const auto padding = Padding();
    block.Store(padding, std::byte{0xc7});
    expected[padding] = std::byte{0xc7};
    Flush(device, block);
    Check(block, expected, name + " one-byte CPU conflict preserves independent GPU channels and padding");
    const bool srgb = format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_B8G8R8A8_SRGB;
    Draw(device, color, Whole, srgb ? 0u : 0xfu, 0xffffffffu, !srgb);
    Flush(device, block);
    Check(block, expected, name + " merged CPU byte is reuploaded before the next native draw");
    const auto changed = Oracle(Width - 1u, Height - 1u, 5) + 3;
    block.Store(changed, std::byte{0x34});
    expected[changed] = std::byte{0x34};
    Draw(device, color, Whole, 0);
    Flush(device, block);
    Check(block, expected, name + " cache refresh uploads a new CPU sample value");
    Draw(device, color, OnePixel, VK_COLOR_COMPONENT_B_BIT, 0x55u);
    ExpectWrite(expected, format, OnePixel, VK_COLOR_COMPONENT_B_BIT, 0x55u);
    Flush(device, block);
    Check(block, expected, name + " real per-sample mask and partial component write");
}

void AliasTests(AgcDriver::VulkanDevice& device, Block& block) {
    auto expected = Seed(block);
    const auto rgba = Target(block, VK_FORMAT_R8G8B8A8_UNORM);
    const auto bgra = Target(block, VK_FORMAT_B8G8R8A8_UNORM);
    const VkRect2D second{{6, 8}, {7, 11}};
    Draw(device, rgba, Partial, VK_COLOR_COMPONENT_R_BIT);
    ExpectWrite(expected, rgba.format, Partial, VK_COLOR_COMPONENT_R_BIT);
    Draw(device, bgra, second, VK_COLOR_COMPONENT_G_BIT);
    ExpectWrite(expected, bgra.format, second, VK_COLOR_COMPONENT_G_BIT);
    Flush(device, block);
    Check(block, expected, "format alias flushes the prior image before a new native view");
    Draw(device, rgba, Whole, 0xfu, 0xffffffffu, true);
    Flush(device, block);
    Check(block, expected, "returning to a cached alias refreshes all eight sample payloads");
    const auto single = Target(block, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT);
    Draw(device, single, OnePixel, VK_COLOR_COMPONENT_A_BIT);
    const AgcDriver::Graphics::ColorTargetLayout oneLayout(Width, Height, AgcDriver::Graphics::ColorTileMode::RenderTarget, 4);
    expected[oneLayout.Offset(2, 2) + 3] = std::byte{0xff};
    Flush(device, block);
    Check(block, expected, "same-address single-sample image writes its own layout without erasing the eight-sample buffer");
    Draw(device, rgba, Whole, 0xfu, 0xffffffffu, true);
    Flush(device, block);
    Check(block, expected, "eight-sample cache refresh after a single-sample alias preserves sample identity");
    const VkRect2D pendingPixel{{17, 23}, {1, 1}};
    const auto pendingOffset = oneLayout.Offset(17, 23) + 3;
    Require(expected[pendingOffset] != std::byte{0xff}, "the pending single-sample alias must change a seeded byte");
    Draw(device, single, pendingPixel, VK_COLOR_COMPONENT_A_BIT);
    Require(block.data[pendingOffset] == expected[pendingOffset], "the single-sample alias must remain pending before returning to the eight-sample surface");
    expected[pendingOffset] = std::byte{0xff};
    Draw(device, rgba, Whole, 0xfu, 0xffffffffu, true);
    Flush(device, block);
    Check(block, expected, "pending single-sample alias is synchronized before eight-sample cache refresh");
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Require(device->ProgrammableSampleLocations(VK_SAMPLE_COUNT_8_BIT), "the actual device must enable programmable eight-sample locations");
        std::lock_guard gpuLock(AgcDriver::GuestMemory::GpuMutex());
        Block block;
        FormatTests(*device, block, VK_FORMAT_R8G8B8A8_UNORM, "RGBA8 UNORM");
        FormatTests(*device, block, VK_FORMAT_B8G8R8A8_UNORM, "BGRA8 UNORM");
        FormatTests(*device, block, VK_FORMAT_R8G8B8A8_SRGB, "RGBA8 SRGB");
        FormatTests(*device, block, VK_FORMAT_B8G8R8A8_SRGB, "BGRA8 SRGB");
        AliasTests(*device, block);
        Flush(*device, block);
        std::puts("real eight-sample color upload, draw, masks, byte conflicts, refresh, padding and aliases passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
