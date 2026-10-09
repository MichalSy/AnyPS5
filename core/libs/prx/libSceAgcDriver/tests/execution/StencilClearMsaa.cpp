#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;
constexpr std::uint32_t Width = 64;
constexpr std::uint32_t Height = 16;
constexpr std::uint32_t WaveSize = 64;
constexpr std::byte Kept{0x40};
constexpr std::uint64_t TotalPixels = Width * Height;
constexpr std::uint64_t ClearedPixels = 32u * Height;
constexpr std::uint64_t NestedPixels = 8u * 4u;
alignas(16384) std::array<std::byte, 16384> Pixels{};
alignas(16384) std::array<std::byte, 65536 * 8> Depth{};
alignas(16384) std::array<std::byte, 65536 * 8> Stencil{};
alignas(256) std::array<std::array<float, 4>, 3> Vertices{};

alignas(256) constexpr std::array<std::uint32_t, 6> VertexCode{
    0xe0382000, 0x80000005, 0xbf8c3f70, 0xf80008cf, 0x03020100, 0xbf810000,
};
alignas(256) constexpr std::array<std::uint32_t, 1> PixelCode{0xbf810000u};
constexpr std::array<std::array<float, 2>, 3> Full{{{0.0f, 0.0f}, {Width * 2.0f, 0.0f}, {0.0f, Height * 2.0f}}};
constexpr std::array<std::array<float, 2>, 3> Partial{{{1.2f, 10.2f}, {4.6f, 10.2f}, {1.2f, 13.6f}}};
constexpr std::array<std::array<std::uint32_t, 2>, 8> SampleNumerators{{
    {1u, 5u}, {15u, 11u}, {9u, 3u}, {3u, 13u}, {5u, 1u}, {11u, 15u}, {13u, 7u}, {7u, 9u}
}};

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
        {0x00a, 3}, {0x00b, std::bit_cast<std::uint32_t>(0.5f)}, {0x010, 0xa000018fu}, {0x011, 0x20000181u},
        {0x10b, 0}, {0x10c, 0xffffff00u}, {0x10d, 0xffffff00u},
        {0x2d5, 0x2000}, {0x1b6, 0}, {0x207, 0}, {0x200, 0x007007f1}, {0x203, 0x800},
        {0x2dc, 0xaa00}, {0x2f8, 0}, {0x292, 2}, {0x293, 0},
        {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa},
        {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff},
        {0x206, 0x43f}, {0x204, 0x80000}, {0x205, 0x240},
        {0x8e, 0}, {0x8f, 0}, {0x202, 0xcc0010},
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

void Draw(AgcDriver::VulkanDevice& device, const AgcDriver::Graphics::State& state, float depth = 0.25f,
    const std::array<std::array<float, 2>, 3>& corners = Full) {
    for (std::size_t index = 0; index < corners.size(); ++index) {
        Vertices[index] = {corners[index][0] * 2.0f / Width - 1.0f, 1.0f - corners[index][1] * 2.0f / Height, depth, 1.0f};
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

void ApplySamples(AgcDriver::QueueState& queue, VkSampleCountFlagBits samples) {
    Require(samples == VK_SAMPLE_COUNT_1_BIT || samples == VK_SAMPLE_COUNT_8_BIT, "fixture sample count must be one or eight");
    const bool multisampled = samples == VK_SAMPLE_COUNT_8_BIT;
    queue.context[0x010] = multisampled ? 0xa000018fu : 0xa0000183u;
    queue.context[0x201] = multisampled ? 0x00103033u : 0u;
    queue.context[0x292] = multisampled ? 3u : 2u;
    queue.context[0x293] = multisampled ? 0x06020000u : 0u;
    queue.context[0x2f8] = multisampled ? 0x0030e003u : 0u;
    queue.context[0x30e] = queue.context[0x30f] = 0xffffffffu;
    for (std::uint32_t pixel = 0; pixel < 4; ++pixel) {
        queue.context[0x2fe + pixel * 4u] = multisampled ? 0x5bb137d9u : 0u;
        queue.context[0x2ff + pixel * 4u] = multisampled ? 0x1ff5739du : 0u;
        queue.context[0x300 + pixel * 4u] = 0u;
        queue.context[0x301 + pixel * 4u] = 0u;
    }
}

void CheckState(const AgcDriver::Graphics::State& state, VkSampleCountFlagBits samples) {
    Require(!state.hasColorTarget && state.colors.empty(), "MSAA stencil fixture must have no single-sample color attachments");
    Require(state.depth && state.depth->format == VK_FORMAT_D32_SFLOAT_S8_UINT && state.depth->samples == samples,
        "decoded combined depth/stencil image must use the actual requested sample count");
    Require(state.rasterizationSamples == samples && state.sampleMask == 0xffffffffu,
        "decoded rasterization samples and full sample mask must match the attachment");
    if (samples == VK_SAMPLE_COUNT_8_BIT) {
        Require(state.sampleLocations.size() == 8u && state.sampleLocationsGrid.width == 1u && state.sampleLocationsGrid.height == 1u,
            "the captured quad-identical sample pattern must use eight locations on the supported one-pixel grid");
        Require(state.depth->sampleLocations.size() == state.sampleLocations.size() &&
            state.depth->sampleLocationsGrid.width == 1u && state.depth->sampleLocationsGrid.height == 1u,
            "the cached depth image must retain its compatible sample-location pattern");
        for (std::size_t index = 0; index < SampleNumerators.size(); ++index) {
            const auto x = SampleNumerators[index][0] / 16.0f;
            const auto y = SampleNumerators[index][1] / 16.0f;
            Require(state.sampleLocations[index].x == x && state.sampleLocations[index].y == y &&
                state.depth->sampleLocations[index].x == x && state.depth->sampleLocations[index].y == y,
                "decoded custom sample locations must preserve the captured framebuffer-relative coordinates");
        }
    }
    Require(state.depth->extent.width == Width && state.depth->extent.height == Height,
        "MSAA depth/stencil extent must match the full-cover geometry");
}

std::uint64_t Samples() {
    auto* recorder = AgcDriver::Graphics::Recorder::Active();
    Require(recorder != nullptr, "the actual Vulkan command recorder must be active");
    return recorder->SamplesTotal();
}

void CountDraw(AgcDriver::VulkanDevice& device, const AgcDriver::QueueState& queue, VkSampleCountFlagBits samples,
    std::uint64_t expected, const std::string& name, float depth = 0.25f,
    const std::array<std::array<float, 2>, 3>& corners = Full) {
    const auto state = AgcDriver::Graphics::DecodeState(queue);
    CheckState(state, samples);
    const auto before = Samples();
    Draw(device, state, depth, corners);
    const auto after = Samples();
    Require(after >= before && after - before == expected, name + ": actual precise sample delta " +
        std::to_string(after - before) + ", expected " + std::to_string(expected));
    for (const auto pixel : Pixels) Require(pixel == Kept, "depth/stencil-only draws must preserve sentinel color bytes");
}

AgcDriver::QueueState Probe(const AgcDriver::QueueState& original, std::uint32_t stencil) {
    auto queue = original;
    queue.context[0x000] = 0;
    queue.context[0x200] = 0x002002a3u;
    queue.context[0x10c] = queue.context[0x10d] = 0x00ffff00u | stencil;
    queue.context[0x90] = 0x80000000u;
    queue.context[0x91] = (Height << 16u) | Width;
    return queue;
}

void CheckContents(AgcDriver::VulkanDevice& device, const AgcDriver::QueueState& queue) {
    constexpr auto samples = VK_SAMPLE_COUNT_8_BIT;
    CountDraw(device, Probe(queue, 3u), samples, (TotalPixels - ClearedPixels) * 8u, "preserved original stencil3 and depth0.25");
    CountDraw(device, Probe(queue, 7u), samples, (ClearedPixels - NestedPixels) * 8u, "scissored stencil7 and depth0.25");
    CountDraw(device, Probe(queue, 9u), samples, NestedPixels * 8u, "nested stencil9 and depth0.25");
    for (const auto reference : {3u, 7u, 9u})
        CountDraw(device, Probe(queue, reference), samples, 0u, "stencil clear must preserve every depth sample", 0.875f);
    auto nested = Probe(queue, 9u);
    nested.context[0x90] = 0x80000000u | (4u << 16u) | 8u;
    nested.context[0x91] = (8u << 16u) | 16u;
    CountDraw(device, nested, samples, NestedPixels * 8u, "all eight samples in the exact nested scissor");
    nested.context[0x10c] = nested.context[0x10d] = 0x00ffff07u;
    CountDraw(device, nested, samples, 0u, "wrong stencil reference must reject the nested scissor");
}

double Edge(const std::array<float, 2>& first, const std::array<float, 2>& second, double x, double y) {
    return (static_cast<double>(second[0]) - first[0]) * (y - first[1]) -
        (static_cast<double>(second[1]) - first[1]) * (x - first[0]);
}

std::uint64_t CoveredSamples(std::uint32_t x, std::uint32_t y) {
    std::uint64_t covered = 0;
    for (const auto& offset : SampleNumerators) {
        const auto sampleX = x + offset[0] / 16.0;
        const auto sampleY = y + offset[1] / 16.0;
        const std::array<double, 3> edges{
            Edge(Partial[0], Partial[1], sampleX, sampleY), Edge(Partial[1], Partial[2], sampleX, sampleY),
            Edge(Partial[2], Partial[0], sampleX, sampleY)
        };
        for (const auto edge : edges) Require(std::abs(edge) > 0.00001, "fixture samples must avoid ambiguous triangle edge ties");
        covered += std::all_of(edges.begin(), edges.end(), [](double edge) { return edge > 0; });
    }
    return covered;
}

void CheckSampleLocations(AgcDriver::VulkanDevice& device, const AgcDriver::QueueState& original) {
    auto queue = original;
    queue.context[0x000] = 0;
    queue.context[0x200] = 0x00700773u;
    queue.context[0x10c] = queue.context[0x10d] = 0x0000ff00u;
    queue.context[0x90] = 0x80000000u;
    queue.context[0x91] = (Height << 16u) | Width;
    std::uint64_t total = 0;
    for (std::uint32_t y = 0; y < Height; ++y)
        for (std::uint32_t x = 0; x < Width; ++x) total += CoveredSamples(x, y);
    Require(total == 45u && CoveredSamples(1u, 13u) == 2u, "captured sample-location oracle must retain its independent boundary counts");
    CountDraw(device, queue, VK_SAMPLE_COUNT_8_BIT, total, "captured eight-sample partial-edge pattern", 0.25f, Partial);
    for (std::uint32_t y = 9; y < 15; ++y) {
        for (std::uint32_t x = 0; x < 6; ++x) {
            const auto expected = CoveredSamples(x, y);
            queue.context[0x90] = 0x80000000u | (y << 16u) | x;
            queue.context[0x91] = ((y + 1u) << 16u) | (x + 1u);
            CountDraw(device, queue, VK_SAMPLE_COUNT_8_BIT, expected,
                "captured per-pixel sample positions at " + std::to_string(x) + "," + std::to_string(y), 0.25f, Partial);
        }
    }
}

}

int main() {
    try {
        const GuestBackings backings;
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Require(device->PreciseOcclusionQueries(), "the MSAA numeric regression requires enabled precise occlusion queries");
        std::lock_guard gpuLock(AgcDriver::GuestMemory::GpuMutex());
        auto queue = Queue();
        ApplySamples(queue, VK_SAMPLE_COUNT_8_BIT);
        queue.context[0x200] = 0x007007f7u;
        Pixels.fill(Kept);
        auto initial = AgcDriver::Graphics::DecodeState(queue);
        CheckState(initial, VK_SAMPLE_COUNT_8_BIT);
        Require(Depth.size() == AgcDriver::Graphics::DepthSliceBytes(initial.depth->extent, 4u) * 8u &&
            Stencil.size() == AgcDriver::Graphics::DepthSliceBytes(initial.depth->extent, 1u) * 8u,
            "registered MSAA backings must cover all padded samples");
        Draw(*device, initial);
        auto* recorder = AgcDriver::Graphics::Recorder::Active();
        Require(recorder != nullptr, "the device must own a real command recorder");
        recorder->CountSamples();
        CountDraw(*device, queue, VK_SAMPLE_COUNT_8_BIT, TotalPixels * 8u, "real eight-sample depth seed draw");
        queue.context[0x000] = 0x22u;
        queue.context[0x200] = 0x771u;
        queue.context[0x00a] = 7u;
        queue.context[0x00b] = std::bit_cast<std::uint32_t>(0.875f);
        queue.context[0x91] = (Height << 16u) | 32u;
        CountDraw(*device, queue, VK_SAMPLE_COUNT_8_BIT, ClearedPixels * 8u, "actual eight-sample scissored stencil clear");
        queue.context[0x00a] = 9u;
        queue.context[0x90] = 0x80000000u | (4u << 16u) | 8u;
        queue.context[0x91] = (8u << 16u) | 16u;
        CountDraw(*device, queue, VK_SAMPLE_COUNT_8_BIT, NestedPixels * 8u, "actual eight-sample nested stencil clear");
        CheckContents(*device, queue);
        CheckSampleLocations(*device, queue);
        auto single = Queue();
        ApplySamples(single, VK_SAMPLE_COUNT_1_BIT);
        single.context[0x000] = 0;
        single.context[0x00a] = 11u;
        single.context[0x00b] = std::bit_cast<std::uint32_t>(0.5f);
        single.context[0x200] = 0x007007f7u;
        const auto singleState = AgcDriver::Graphics::DecodeState(single);
        CheckState(singleState, VK_SAMPLE_COUNT_1_BIT);
        Draw(*device, singleState, 0.5f);
        Samples();
        CountDraw(*device, Probe(single, 11u), VK_SAMPLE_COUNT_1_BIT, TotalPixels,
            "same-address single-sample cache identity", 0.5f);
        CheckContents(*device, queue);
        CheckSampleLocations(*device, queue);
        std::puts("eight-sample stencil clear, precise query, scissor, depth preservation and cache identity tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
