#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorResolve.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleColorSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::Graphics::ColorTarget;
using ShaderRecompiler::ShaderStage;
constexpr std::uint32_t Width = 133;
constexpr std::uint32_t Height = 69;
constexpr std::size_t StorageBytes = 10u * 65536u;
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
alignas(256) constexpr std::array<std::uint32_t, 11> FractionalPixelCode{
    0x7e0802ff, 0x3f400000, 0x7e0a02ff, 0x3f400000,
    0x7e0c02ff, 0x3e800000, 0x7e0e02ff, 0x3f400000,
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
    return (static_cast<std::size_t>(y / 64u) * 5u + x / 32u) * 65536u + offset;
}

class Block {
public:
    explicit Block(std::size_t extraBytes = 0) : allocationBytes(StorageBytes + extraBytes) {
        Require(AgcDriver::GuestMemory::WriteWatched(), "color resolve conflict coverage requires actual guest write watching");
#ifdef _WIN32
        data = static_cast<std::byte*>(GuestArena::GuestArenaAllocate_nid_postfix(allocationBytes, 65536u));
        Require(data != nullptr, "cannot allocate a watched guest color surface");
        GuestArena::GuestArenaCommit_nid_postfix(data, allocationBytes, PAGE_READWRITE, allocationBytes);
#else
        void* raw = mmap(nullptr, allocationBytes + 65536u, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        Require(raw != MAP_FAILED, "cannot map a watched guest color surface");
        const auto begin = reinterpret_cast<std::uintptr_t>(raw);
        const auto aligned = (begin + 65535u) & ~std::uintptr_t{65535u};
        if (aligned != begin) munmap(raw, aligned - begin);
        const auto end = begin + allocationBytes + 65536u;
        if (aligned + allocationBytes != end) munmap(reinterpret_cast<void*>(aligned + allocationBytes), end - aligned - allocationBytes);
        data = reinterpret_cast<std::byte*>(aligned);
        GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(data, allocationBytes);
#endif
        Require(Address() % 65536u == 0 && AgcDriver::GuestMemory::Watched(Address(), StorageBytes), "the color surface must be aligned and genuinely write-watched");
        {
            GuestAllocations::Mutation mutation;
            mutation.Add(data, allocationBytes, true, true);
        }
        Require(AgcDriver::GuestMemory::CollectWritesUncached(Address(), StorageBytes) != 0, "the watched color surface must have a nonzero tracker generation");
    }
    ~Block() {
        {
            GuestAllocations::Mutation mutation;
            mutation.Unmap(data, allocationBytes, [](const void*, std::size_t, const void*, bool) {});
        }
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(data, allocationBytes);
        GuestArena::GuestArenaRelease_nid_postfix(data, allocationBytes);
#else
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(data, allocationBytes);
        munmap(data, allocationBytes);
#endif
    }
    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;
    std::uint64_t Address() const { return reinterpret_cast<std::uintptr_t>(data); }
    std::size_t Size() const { return allocationBytes; }
    void Store(std::size_t offset, std::byte value) {
        data[offset] = value;
        AgcDriver::GuestMemory::MarkWritten(Address() + offset, 1);
    }
    std::byte* data = nullptr;
private:
    std::size_t allocationBytes;
};

std::vector<std::byte> SeedSamples(Block& block) {
    std::vector<std::byte> bytes(StorageBytes, std::byte{0xa9});
    std::vector<bool> visited(StorageBytes / 4);
    for (std::uint32_t y = 0; y < Height; ++y) {
        for (std::uint32_t x = 0; x < Width; ++x) {
            for (std::uint32_t sample = 0; sample < 8; ++sample) {
                const auto offset = Oracle(x, y, sample);
                Require(offset + 4 <= bytes.size() && !visited[offset / 4], "independent AMD oracle aliases or escapes guest storage");
                visited[offset / 4] = true;
                for (std::uint32_t channel = 0; channel < 4; ++channel)
                    bytes[offset + channel] = static_cast<std::byte>(17u + 2u * ((x * 3u + y * 5u + channel * 11u) % 80u) + sample * 8u);
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
    std::uint32_t sampleMask = 0xffffffffu, bool keepBlend = false, std::span<const std::uint32_t> pixelCode = PixelCode) {
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
    const std::array<ShaderRecompiler::MemoryRegion, 1> pixelMemory{{{reinterpret_cast<std::uintptr_t>(pixelCode.data()), std::as_bytes(pixelCode)}}};
    ShaderRecompiler::RecompileRequest fragment{
        {ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(pixelCode.data()), pixelCode, 0, {}},
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

std::vector<std::byte> SeedDestination(Block& block) {
    const AgcDriver::Graphics::ColorTargetLayout layout(Width, Height, AgcDriver::Graphics::ColorTileMode::RenderTarget, 4);
    Require(layout.Bytes() == 2u * 65536u, "the destination must contain two independently tracked units");
    std::vector<std::byte> bytes(StorageBytes, std::byte{0xb6});
    for (std::uint32_t y = 0; y < Height; ++y) {
        for (std::uint32_t x = 0; x < Width; ++x) {
            const auto offset = layout.Offset(x, y);
            for (std::uint32_t channel = 0; channel < 4; ++channel)
                bytes[offset + channel] = static_cast<std::byte>((x * 7u + y * 3u + channel * 29u) % 173u);
        }
    }
    std::memcpy(block.data, bytes.data(), bytes.size());
    AgcDriver::GuestMemory::MarkWritten(block.Address(), StorageBytes);
    return bytes;
}

void ExpectSingleWrite(std::vector<std::byte>& expected, VkFormat format, VkRect2D region, VkColorComponentFlags mask) {
    const AgcDriver::Graphics::ColorTargetLayout layout(Width, Height, AgcDriver::Graphics::ColorTileMode::RenderTarget, 4);
    for (std::uint32_t y = 0; y < region.extent.height; ++y) {
        for (std::uint32_t x = 0; x < region.extent.width; ++x) {
            const auto offset = layout.Offset(static_cast<std::uint32_t>(region.offset.x) + x, static_cast<std::uint32_t>(region.offset.y) + y);
            for (std::uint32_t channel = 0; channel < 4; ++channel)
                if ((mask & (1u << channel)) != 0) expected[offset + ByteChannel(format, channel)] = std::byte{0xff};
        }
    }
}

void ExpectResolve(std::vector<std::byte>& expected, const std::vector<std::byte>& source, VkRect2D region) {
    const AgcDriver::Graphics::ColorTargetLayout layout(Width, Height, AgcDriver::Graphics::ColorTileMode::RenderTarget, 4);
    for (std::uint32_t y = 0; y < region.extent.height; ++y) {
        for (std::uint32_t x = 0; x < region.extent.width; ++x) {
            const auto px = static_cast<std::uint32_t>(region.offset.x) + x;
            const auto py = static_cast<std::uint32_t>(region.offset.y) + y;
            const auto offset = layout.Offset(px, py);
            for (std::uint32_t channel = 0; channel < 4; ++channel) {
                std::uint32_t sum = 0;
                for (std::uint32_t sample = 0; sample < 8; ++sample)
                    sum += std::to_integer<std::uint32_t>(source[Oracle(px, py, sample) + channel]);
                Require(sum % 8u == 0, "the resolve oracle must use an exact integer average");
                expected[offset + channel] = static_cast<std::byte>(sum / 8u);
            }
        }
    }
}

void Resolve(AgcDriver::VulkanDevice& device, const ColorTarget& source, const ColorTarget& destination, VkRect2D region) {
    device.ResolveColor(AgcDriver::Graphics::ColorResolvePass{source, destination, region});
}

std::size_t FirstUnitPadding() {
    const AgcDriver::Graphics::ColorTargetLayout layout(Width, Height, AgcDriver::Graphics::ColorTileMode::RenderTarget, 4);
    std::vector<bool> occupied(65536u / 4u);
    for (std::uint32_t y = 0; y < Height; ++y) {
        for (std::uint32_t x = 0; x < Width; ++x) {
            const auto offset = layout.Offset(x, y);
            if (offset < 65536u) occupied[offset / 4u] = true;
        }
    }
    const auto found = std::find(occupied.begin(), occupied.end(), false);
    Require(found != occupied.end(), "the first destination unit must contain padding");
    return static_cast<std::size_t>(found - occupied.begin()) * 4u;
}

void BasicTests(AgcDriver::VulkanDevice& device, Block& sourceBlock, Block& destinationBlock, VkFormat format, const std::string& name) {
    auto source = SeedSamples(sourceBlock);
    auto expected = SeedDestination(destinationBlock);
    const auto sourceTarget = Target(sourceBlock, format);
    const auto destinationTarget = Target(destinationBlock, format, VK_SAMPLE_COUNT_1_BIT);
    Draw(device, sourceTarget, Whole, 0);
    Flush(device, sourceBlock);
    Check(sourceBlock, source, name + " independent eight-sample source upload");
    Draw(device, destinationTarget, Whole, 0);
    Flush(device, destinationBlock);
    Check(destinationBlock, expected, name + " destination seed and padding");
    Resolve(device, sourceTarget, destinationTarget, Whole);
    ExpectResolve(expected, source, Whole);
    Flush(device, destinationBlock);
    Check(destinationBlock, expected, name + " exact eight-sample average over the full region");
    Check(sourceBlock, source, name + " resolve preserves the source samples and padding");
    source = SeedSamples(sourceBlock);
    expected = SeedDestination(destinationBlock);
    Resolve(device, sourceTarget, destinationTarget, Partial);
    ExpectResolve(expected, source, Partial);
    Flush(device, destinationBlock);
    Check(destinationBlock, expected, name + " partial resolve preserves uncovered pixels and padding");
    const VkRect2D second{{127, 30}, {6, 12}};
    Resolve(device, sourceTarget, destinationTarget, second);
    ExpectResolve(expected, source, second);
    Resolve(device, sourceTarget, destinationTarget, Partial);
    ExpectResolve(expected, source, Partial);
    Flush(device, destinationBlock);
    Check(destinationBlock, expected, name + " repeated cached regions preserve both destination units");
}

void PendingTests(AgcDriver::VulkanDevice& device, Block& sourceBlock, Block& destinationBlock, VkFormat format, const std::string& name) {
    auto source = SeedSamples(sourceBlock);
    auto expected = SeedDestination(destinationBlock);
    const auto sourceTarget = Target(sourceBlock, format);
    const auto destinationTarget = Target(destinationBlock, format, VK_SAMPLE_COUNT_1_BIT);
    const AgcDriver::Graphics::ColorTargetLayout layout(Width, Height, AgcDriver::Graphics::ColorTileMode::RenderTarget, 4);
    const VkRect2D outside{{17, 23}, {1, 1}};
    const auto outsideOffset = layout.Offset(17, 23) + 3;
    Draw(device, destinationTarget, outside, VK_COLOR_COMPONENT_A_BIT);
    Require(destinationBlock.data[outsideOffset] == expected[outsideOffset] && expected[outsideOffset] != std::byte{0xff}, "the cached single-sample destination write must remain pending");
    ExpectSingleWrite(expected, format, outside, VK_COLOR_COMPONENT_A_BIT);
    Draw(device, sourceTarget, Partial, VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT, 0x55u);
    const auto pendingOffset = Oracle(4, 5, 0) + ByteChannel(format, 0);
    Require(sourceBlock.data[pendingOffset] == source[pendingOffset] && source[pendingOffset] != std::byte{0xff}, "the eight-sample source draw must remain pending");
    ExpectWrite(source, format, Partial, VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT, 0x55u);
    Resolve(device, sourceTarget, destinationTarget, Partial);
    ExpectResolve(expected, source, Partial);
    Flush(device, destinationBlock);
    Flush(device, sourceBlock);
    Check(destinationBlock, expected, name + " resolve sees the pending source draw and preserves the pending destination draw");
    Check(sourceBlock, source, name + " selected source samples retain their own values");
}

void ConflictTests(AgcDriver::VulkanDevice& device, Block& sourceBlock, Block& destinationBlock, VkFormat format, const std::string& name) {
    const auto source = SeedSamples(sourceBlock);
    auto expected = SeedDestination(destinationBlock);
    const auto sourceTarget = Target(sourceBlock, format);
    const auto destinationTarget = Target(destinationBlock, format, VK_SAMPLE_COUNT_1_BIT);
    const AgcDriver::Graphics::ColorTargetLayout layout(Width, Height, AgcDriver::Graphics::ColorTileMode::RenderTarget, 4);
    Resolve(device, sourceTarget, destinationTarget, Whole);
    const auto conflict = layout.Offset(4, 5) + ByteChannel(format, 0);
    Require(conflict < 65536u && destinationBlock.data[conflict] == expected[conflict], "the resolved destination must remain pending before the CPU conflict");
    Require(AgcDriver::GuestMemory::Watched(destinationBlock.Address(), destinationTarget.bytes), "the destination must remain watched after its native import");
    const auto beforeCpu = AgcDriver::GuestMemory::CollectWritesUncached(destinationBlock.Address(), destinationTarget.bytes);
    Require(beforeCpu != 0, "the pending resolve must retain a valid destination generation");
    destinationBlock.Store(conflict, std::byte{0x2a});
    expected[conflict] = std::byte{0x2a};
    const auto padding = FirstUnitPadding();
    destinationBlock.Store(padding, std::byte{0xc7});
    expected[padding] = std::byte{0xc7};
    AgcDriver::GuestMemory::CollectWritesUncached(destinationBlock.Address(), destinationTarget.bytes);
    const std::array<std::uint64_t, 2> generations{beforeCpu, beforeCpu};
    std::array<std::uint8_t, 2> changed{};
    Require(AgcDriver::GuestMemory::ChangedBlocks(destinationBlock.Address(), destinationTarget.bytes, generations, changed) &&
        changed[0] == AgcDriver::GuestMemory::BlockWritten && changed[1] == AgcDriver::GuestMemory::BlockUnchanged,
        "the real CPU conflict must stamp only the first destination unit");
    const auto cpuUnit = expected;
    ExpectResolve(expected, source, Whole);
    std::copy_n(cpuUnit.begin(), 65536u, expected.begin());
    Flush(device, destinationBlock);
    Check(destinationBlock, expected, name + " CPU-written unit and padding survive while the untouched unit receives resolved pixels");
    Draw(device, destinationTarget, Whole, 0xfu, 0xffffffffu, true);
    Flush(device, destinationBlock);
    Check(destinationBlock, expected, name + " the cached destination refreshes the preserved CPU unit before a real blend draw");
    Resolve(device, sourceTarget, destinationTarget, Partial);
    ExpectResolve(expected, source, Partial);
    Flush(device, destinationBlock);
    Check(destinationBlock, expected, name + " repeated resolve uses the latest CPU baseline without changing padding");
}

void AliasTests(AgcDriver::VulkanDevice& device, Block& sourceBlock, Block& destinationBlock, VkFormat format, const std::string& name) {
    auto source = SeedSamples(sourceBlock);
    auto expected = SeedDestination(destinationBlock);
    const auto sourceTarget = Target(sourceBlock, format);
    const auto destinationTarget = Target(destinationBlock, format, VK_SAMPLE_COUNT_1_BIT);
    const auto aliasFormat = format == VK_FORMAT_R8G8B8A8_UNORM ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
    const auto sourceAlias = Target(sourceBlock, aliasFormat);
    const auto destinationAlias = Target(destinationBlock, aliasFormat, VK_SAMPLE_COUNT_1_BIT);
    const VkRect2D outside{{17, 23}, {1, 1}};
    Draw(device, sourceAlias, Partial, VK_COLOR_COMPONENT_B_BIT);
    ExpectWrite(source, aliasFormat, Partial, VK_COLOR_COMPONENT_B_BIT);
    Draw(device, destinationAlias, outside, VK_COLOR_COMPONENT_R_BIT);
    ExpectSingleWrite(expected, aliasFormat, outside, VK_COLOR_COMPONENT_R_BIT);
    Resolve(device, sourceTarget, destinationTarget, Partial);
    ExpectResolve(expected, source, Partial);
    Flush(device, destinationBlock);
    Flush(device, sourceBlock);
    Check(destinationBlock, expected, name + " resolve synchronizes pending source and destination format aliases");
    Check(sourceBlock, source, name + " resolving through a source alias preserves every sample");
    Draw(device, destinationAlias, Whole, 0xfu, 0xffffffffu, true);
    Flush(device, destinationBlock);
    Check(destinationBlock, expected, name + " returning to a cached destination alias preserves resolved bytes");
}

using QuantizedBytes = std::map<std::size_t, std::array<std::byte, 2>>;

void ExpectFractionalWrite(std::vector<std::byte>& expected, QuantizedBytes& quantized, VkFormat format, VkRect2D region, VkColorComponentFlags mask, std::uint32_t sampleMask) {
    const bool srgb = format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_B8G8R8A8_SRGB;
    const std::array<std::byte, 4> encoded = srgb ? std::array{std::byte{225}, std::byte{225}, std::byte{137}, std::byte{191}} :
        std::array{std::byte{191}, std::byte{191}, std::byte{64}, std::byte{191}};
    const std::array<std::byte, 4> lower = srgb ? std::array{std::byte{224}, std::byte{224}, std::byte{136}, std::byte{191}} :
        std::array{std::byte{191}, std::byte{191}, std::byte{63}, std::byte{191}};
    for (std::uint32_t y = 0; y < region.extent.height; ++y) {
        for (std::uint32_t x = 0; x < region.extent.width; ++x) {
            for (std::uint32_t sample = 0; sample < 8; ++sample) {
                if ((sampleMask & (1u << sample)) == 0) continue;
                const auto offset = Oracle(static_cast<std::uint32_t>(region.offset.x) + x, static_cast<std::uint32_t>(region.offset.y) + y, sample);
                for (std::uint32_t channel = 0; channel < 4; ++channel) {
                    if ((mask & (1u << channel)) == 0) continue;
                    const auto byte = offset + ByteChannel(format, channel);
                    expected[byte] = encoded[channel];
                    quantized[byte] = {lower[channel], static_cast<std::byte>(std::to_integer<unsigned>(lower[channel]) + 1u)};
                }
            }
        }
    }
}

void CheckQuantized(const Block& block, const std::vector<std::byte>& expected, const QuantizedBytes& quantized, const std::string& name) {
    Require(expected.size() == StorageBytes, "expected quantized storage must cover visible samples and padding");
    auto limit = quantized.begin();
    for (std::size_t offset = 0; offset < expected.size(); ++offset) {
        const auto actual = std::to_integer<unsigned>(block.data[offset]);
        auto lower = std::to_integer<unsigned>(expected[offset]);
        auto upper = lower;
        if (limit != quantized.end() && limit->first == offset) {
            lower = std::to_integer<unsigned>(limit->second[0]);
            upper = std::to_integer<unsigned>(limit->second[1]);
            ++limit;
        }
        if (actual >= lower && actual <= upper) continue;
        throw std::runtime_error(name + ": guest byte " + std::to_string(offset) + " is " + std::to_string(actual) +
            ", expected " + std::to_string(lower) + ".." + std::to_string(upper));
    }
    Require(limit == quantized.end(), "quantized byte limits escape guest storage");
}

void ExpectQuantizedResolve(std::vector<std::byte>& expected, QuantizedBytes& destinationLimits, const std::vector<std::byte>& source, const QuantizedBytes& sourceLimits, VkRect2D region) {
    const AgcDriver::Graphics::ColorTargetLayout layout(Width, Height, AgcDriver::Graphics::ColorTileMode::RenderTarget, 4);
    for (std::uint32_t y = 0; y < region.extent.height; ++y) {
        for (std::uint32_t x = 0; x < region.extent.width; ++x) {
            const auto px = static_cast<std::uint32_t>(region.offset.x) + x;
            const auto py = static_cast<std::uint32_t>(region.offset.y) + y;
            const auto offset = layout.Offset(px, py);
            for (std::uint32_t channel = 0; channel < 4; ++channel) {
                std::uint32_t lower = 0;
                std::uint32_t upper = 0;
                for (std::uint32_t sample = 0; sample < 8; ++sample) {
                    const auto byte = Oracle(px, py, sample) + channel;
                    if (const auto found = sourceLimits.find(byte); found != sourceLimits.end()) {
                        lower += std::to_integer<unsigned>(found->second[0]);
                        upper += std::to_integer<unsigned>(found->second[1]);
                    } else {
                        const auto value = std::to_integer<unsigned>(source[byte]);
                        lower += value;
                        upper += value;
                    }
                }
                if (lower == upper) Require(lower % 8u == 0, "an exact resolve component must have an integer average");
                const auto first = static_cast<std::byte>(lower / 8u);
                const auto last = static_cast<std::byte>((upper + 7u) / 8u);
                expected[offset + channel] = first;
                if (first == last) destinationLimits.erase(offset + channel);
                else destinationLimits[offset + channel] = {first, last};
            }
        }
    }
}

void ExpectExactWrite(std::vector<std::byte>& expected, QuantizedBytes& quantized, VkFormat format, VkRect2D region, VkColorComponentFlags mask, std::uint32_t sampleMask) {
    ExpectWrite(expected, format, region, mask, sampleMask);
    for (std::uint32_t y = 0; y < region.extent.height; ++y) {
        for (std::uint32_t x = 0; x < region.extent.width; ++x) {
            for (std::uint32_t sample = 0; sample < 8; ++sample) {
                if ((sampleMask & (1u << sample)) == 0) continue;
                const auto offset = Oracle(static_cast<std::uint32_t>(region.offset.x) + x, static_cast<std::uint32_t>(region.offset.y) + y, sample);
                for (std::uint32_t channel = 0; channel < 4; ++channel)
                    if ((mask & (1u << channel)) != 0) quantized.erase(offset + ByteChannel(format, channel));
            }
        }
    }
}

void SharedFormatTests(AgcDriver::VulkanDevice& device, Block& sourceBlock, Block& destinationBlock, VkFormat format, const std::string& name) {
    auto source = SeedSamples(sourceBlock);
    QuantizedBytes sourceLimits;
    QuantizedBytes destinationLimits;
    const auto baseline = source;
    auto expected = SeedDestination(destinationBlock);
    const auto sourceTarget = Target(sourceBlock, format);
    const auto destinationTarget = Target(destinationBlock, format, VK_SAMPLE_COUNT_1_BIT);
    const auto srgbFormat = format == VK_FORMAT_R8G8B8A8_UNORM ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_B8G8R8A8_SRGB;
    const auto srgbAlias = Target(sourceBlock, srgbFormat);
    const auto swapped = Target(sourceBlock, format == VK_FORMAT_R8G8B8A8_UNORM ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM);
    Draw(device, srgbAlias, Partial, VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_B_BIT, 0x55u, false, FractionalPixelCode);
    ExpectFractionalWrite(source, sourceLimits, srgbFormat, Partial, VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_B_BIT, 0x55u);
    const auto reject = [&](const ColorTarget& invalid, std::string_view reason) {
        AgcDriver::Graphics::Context lookup{};
        lookup.device = device.Device();
        bool rejected = false;
        try {
            AgcDriver::Graphics::CachedMultisampleColorSurface(lookup, invalid);
        } catch (const std::runtime_error& error) {
            rejected = std::string_view(error.what()).find(reason) != std::string_view::npos;
        }
        Require(rejected, name + " invalid shared target must be rejected: " + std::string(reason));
        Check(sourceBlock, baseline, name + " rejecting an invalid shared target preserves pending source samples");
    };
    auto invalid = sourceTarget;
    invalid.format = VK_FORMAT_R32_UINT;
    reject(invalid, "format is unsupported");
    invalid = sourceTarget;
    invalid.elementBytes = 8;
    reject(invalid, "unsupported multisample color geometry");
    invalid = sourceTarget;
    invalid.depth = 2;
    reject(invalid, "arrays, volumes and mipmaps are unsupported");
    invalid = sourceTarget;
    invalid.bytes -= 65536u;
    reject(invalid, "byte range disagrees with its layout");
    Resolve(device, sourceTarget, destinationTarget, Partial);
    ExpectQuantizedResolve(expected, destinationLimits, source, sourceLimits, Partial);
    Check(sourceBlock, baseline, name + " sRGB-to-UNORM resolve keeps the source resident without a guest write-back");
    Flush(device, destinationBlock);
    CheckQuantized(destinationBlock, expected, destinationLimits, name + " sRGB attachment encoding survives raw UNORM resolve");
    Draw(device, swapped, Partial, VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_A_BIT, 0xaau, false, FractionalPixelCode);
    ExpectFractionalWrite(source, sourceLimits, swapped.format, Partial, VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_A_BIT, 0xaau);
    Resolve(device, sourceTarget, destinationTarget, Partial);
    ExpectQuantizedResolve(expected, destinationLimits, source, sourceLimits, Partial);
    Check(sourceBlock, baseline, name + " channel-order view changes preserve all pending source samples");
    Flush(device, destinationBlock);
    CheckQuantized(destinationBlock, expected, destinationLimits, name + " UNORM view restores linear attachment encoding");
    Draw(device, srgbAlias, OnePixel, 0xfu, 0xffffffffu, false, FractionalPixelCode);
    ExpectFractionalWrite(source, sourceLimits, srgbFormat, OnePixel, 0xfu, 0xffffffffu);
    Resolve(device, sourceTarget, destinationTarget, OnePixel);
    ExpectQuantizedResolve(expected, destinationLimits, source, sourceLimits, OnePixel);
    Check(sourceBlock, baseline, name + " returning to the cached sRGB view still avoids source publication");
    Flush(device, destinationBlock);
    CheckQuantized(destinationBlock, expected, destinationLimits, name + " cached sRGB view preserves color conversion and alpha");
    const auto conflict = Oracle(4, 5, 0) + ByteChannel(srgbFormat, 0);
    sourceBlock.Store(conflict, std::byte{41});
    source[conflict] = std::byte{41};
    sourceLimits.erase(conflict);
    Draw(device, sourceTarget, OnePixel, VK_COLOR_COMPONENT_A_BIT, 0x55u);
    ExpectExactWrite(source, sourceLimits, format, OnePixel, VK_COLOR_COMPONENT_A_BIT, 0x55u);
    Resolve(device, sourceTarget, destinationTarget, Partial);
    ExpectQuantizedResolve(expected, destinationLimits, source, sourceLimits, Partial);
    Flush(device, destinationBlock);
    Flush(device, sourceBlock);
    CheckQuantized(destinationBlock, expected, destinationLimits, name + " CPU source conflict is refreshed before resolving across format views");
    CheckQuantized(sourceBlock, source, sourceLimits, name + " every source sample, CPU conflict, and padding survives shared format views");
}

void GeometryAliasTests(AgcDriver::VulkanDevice& device, Block& sourceBlock, VkFormat format, const std::string& name) {
    auto expected = SeedSamples(sourceBlock);
    const auto baseline = expected;
    const auto original = Target(sourceBlock, format);
    auto wider = original;
    ++wider.extent.width;
    wider.surfaceExtent = wider.extent;
    const AgcDriver::Graphics::MultisampleColorLayout resized(wider.extent.width, wider.extent.height, wider.elementBytes, 8u);
    Require(resized.Bytes() == original.bytes, "the geometry alias must retain the same padded guest byte range");
    Draw(device, original, Partial, VK_COLOR_COMPONENT_R_BIT, 0x55u);
    ExpectWrite(expected, format, Partial, VK_COLOR_COMPONENT_R_BIT, 0x55u);
    Check(sourceBlock, baseline, name + " original geometry draw remains pending before the valid extent change");
    Draw(device, wider, Whole, 0);
    Check(sourceBlock, expected, name + " a valid extent change publishes the original geometry despite identical padded bytes");
    Draw(device, original, Partial, VK_COLOR_COMPONENT_G_BIT, 0xaau);
    ExpectWrite(expected, format, Partial, VK_COLOR_COMPONENT_G_BIT, 0xaau);
    Flush(device, sourceBlock);
    Check(sourceBlock, expected, name + " returning to the original geometry preserves sample masks and padding");
}

void LifetimeTests(AgcDriver::VulkanDevice& device, std::unique_lock<AgcDriver::GuestMemory::GpuMutexType>& gpuLock) {
    constexpr std::size_t pieceBytes = 65536u;
    {
        Block clean(pieceBytes);
        const auto baseline = SeedSamples(clean);
        Draw(device, Target(clean, VK_FORMAT_R8G8B8A8_UNORM), Whole, 0);
        Flush(device, clean);
        GuestAllocations::Mutation mutation;
        mutation.Protect(clean.data + StorageBytes, pieceBytes, true, false, [&] {
            Check(clean, baseline, "a clean multisample lease releases for a mutation outside its target in the same backing range");
#ifdef _WIN32
            GuestArena::GuestArenaSetProtection_nid_postfix(clean.Address() + StorageBytes, pieceBytes, PAGE_READONLY);
#else
            Require(mprotect(clean.data + StorageBytes, pieceBytes, PROT_READ) == 0, "cannot protect the clean multisample backing fragment");
#endif
        });
    }
    Block block(pieceBytes);
    auto expected = SeedSamples(block);
    const auto target = Target(block, VK_FORMAT_B8G8R8A8_SRGB);
    const auto protect = [&](std::size_t offset, bool writable) {
        GuestAllocations::Mutation mutation;
        mutation.Protect(block.data + offset, pieceBytes, true, writable, [&] {
            Check(block, expected, "pending multisample samples are published before backing protection changes");
            Require(!AgcDriver::Graphics::AnyPendingMultisampleColors(block.Address(), StorageBytes), "backing protection must release the pending multisample lease after publication");
#ifdef _WIN32
            GuestArena::GuestArenaSetProtection_nid_postfix(block.Address() + offset, pieceBytes, writable ? PAGE_READWRITE : PAGE_READONLY);
#else
            Require(mprotect(block.data + offset, pieceBytes, PROT_READ | (writable ? PROT_WRITE : 0)) == 0, "cannot change the multisample backing protection");
#endif
        });
    };
    Draw(device, target, Partial, VK_COLOR_COMPONENT_R_BIT, 0x55u);
    ExpectWrite(expected, target.format, Partial, VK_COLOR_COMPONENT_R_BIT, 0x55u);
    protect(StorageBytes, false);
    protect(StorageBytes, true);
    Draw(device, target, OnePixel, VK_COLOR_COMPONENT_B_BIT, 0xaau);
    ExpectWrite(expected, target.format, OnePixel, VK_COLOR_COMPONENT_B_BIT, 0xaau);
    protect(0, false);
    protect(0, true);

    const auto unmap = [&](std::size_t offset, std::size_t bytes) {
        GuestAllocations::Mutation mutation;
        mutation.Unmap(block.data + offset, bytes, [&](const void* piece, std::size_t count, const void*, bool) {
            Check(block, expected, "pending multisample samples are published before host unmap");
            Require(!AgcDriver::Graphics::AnyPendingMultisampleColors(block.Address(), StorageBytes), "host unmap must not retain pending multisample results for the old backing");
#ifdef _WIN32
            GuestArena::GuestArenaReset_nid_postfix(const_cast<void*>(piece), count);
#else
            GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(piece, count);
            Require(mmap(const_cast<void*>(piece), count, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == piece, "cannot reset the unmapped multisample backing");
#endif
        });
    };
    const auto remap = [&](std::size_t offset, std::size_t bytes) {
        auto* pointer = block.data + offset;
#ifdef _WIN32
        GuestArena::GuestArenaCommit_nid_postfix(pointer, bytes, PAGE_READWRITE, bytes);
#else
        Require(mmap(pointer, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == pointer, "cannot replace the multisample backing");
        GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(pointer, bytes);
#endif
        GuestAllocations::Mutation mutation;
        mutation.Add(pointer, bytes, true, true);
    };
    Draw(device, target, Partial, VK_COLOR_COMPONENT_G_BIT, 0x55u);
    ExpectWrite(expected, target.format, Partial, VK_COLOR_COMPONENT_G_BIT, 0x55u);
    unmap(pieceBytes, pieceBytes);
    Require(!AgcDriver::GuestMemory::Accessible(block.data + pieceBytes, pieceBytes), "the removed multisample fragment must actually be inaccessible");
    AgcDriver::Graphics::FlushMultisampleColors(0, std::numeric_limits<std::size_t>::max());
    remap(pieceBytes, pieceBytes);
    std::fill_n(block.data + pieceBytes, pieceBytes, std::byte{0xc3});
    std::fill_n(expected.begin() + pieceBytes, pieceBytes, std::byte{0xc3});
    AgcDriver::GuestMemory::MarkWritten(block.Address() + pieceBytes, pieceBytes);
    AgcDriver::Graphics::FlushMultisampleColors(0, std::numeric_limits<std::size_t>::max());
    Check(block, expected, "a general flush cannot overwrite a replacement fragment with old multisample results");

    Draw(device, target, OnePixel, VK_COLOR_COMPONENT_A_BIT, 0xaau);
    ExpectWrite(expected, target.format, OnePixel, VK_COLOR_COMPONENT_A_BIT, 0xaau);
    std::exception_ptr mutationFailure;
    gpuLock.unlock();
    {
        std::jthread mutator([&] {
            try { unmap(0, block.Size()); }
            catch (...) { mutationFailure = std::current_exception(); }
        });
    }
    gpuLock.lock();
    if (mutationFailure) std::rethrow_exception(mutationFailure);
    Require(!AgcDriver::GuestMemory::Accessible(block.data, StorageBytes), "the old multisample backing must actually be inaccessible");
    AgcDriver::Graphics::FlushMultisampleColors(0, std::numeric_limits<std::size_t>::max());
    remap(0, block.Size());
    expected.assign(StorageBytes, std::byte{0x6b});
    std::memcpy(block.data, expected.data(), expected.size());
    AgcDriver::GuestMemory::MarkWritten(block.Address(), StorageBytes);
    AgcDriver::Graphics::FlushMultisampleColors(0, std::numeric_limits<std::size_t>::max());
    Check(block, expected, "a general flush cannot write an old multisample allocation into a new mapping at the same address");
    Draw(device, target, Partial, VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT, 0x55u);
    ExpectWrite(expected, target.format, Partial, VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT, 0x55u);
    Flush(device, block);
    Check(block, expected, "a cached multisample image refreshes from replacement backing before a new draw");
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Require(device->ProgrammableSampleLocations(VK_SAMPLE_COUNT_8_BIT), "the actual device must enable programmable eight-sample locations");
        std::unique_lock gpuLock(AgcDriver::GuestMemory::GpuMutex());
        Block source;
        Block destination;
        for (const auto format : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM}) {
            const std::string name = format == VK_FORMAT_R8G8B8A8_UNORM ? "RGBA8 UNORM" : "BGRA8 UNORM";
            BasicTests(*device, source, destination, format, name);
            PendingTests(*device, source, destination, format, name);
            ConflictTests(*device, source, destination, format, name);
            AliasTests(*device, source, destination, format, name);
            SharedFormatTests(*device, source, destination, format, name);
            GeometryAliasTests(*device, source, format, name);
        }
        Flush(*device, source);
        Flush(*device, destination);
        LifetimeTests(*device, gpuLock);
        std::puts("real eight-to-one color averages, partial regions, pending draws, CPU units, padding and aliases passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
