#include "DepthFastClearHarness.hpp"
#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cmath>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using AgcDriver::Graphics::Require;
using DepthFastClearHarness::Height;
using DepthFastClearHarness::Width;

alignas(4096) std::array<float, 65536> Depth{};
alignas(4096) std::array<std::uint8_t, 65536> Stencil{};
alignas(256) std::array<std::uint32_t, 64> Output{};
alignas(256) constexpr std::array<std::uint32_t, 13> LoadCode{
    0x7e3c0280u, 0x7e3e0280u, 0x7e140280u, 0xf0001108u, 0x00010a1eu,
    0xbf8c3f70u, 0xf0201108u, 0x00010a1eu, 0x34060082u, 0xe0701000u, 0x80000a03u, 0xbf810000u, 0xbf810000u,
};
alignas(256) constexpr std::array<std::uint32_t, 10> SampleCode{
    0x7e3c02ffu, 0x3f000000u, 0x7e3e02ffu, 0x3f000000u, 0xf09c0108u,
    0x00610a1eu, 0x34060082u, 0xe0701000u, 0x80000a03u, 0xbf810000u,
};

void Read(AgcDriver::VulkanDevice& device, std::uint32_t format, std::uint32_t width, bool sampled, float expected, std::uint64_t plane = 0, bool zeroStore = false) {
    Output.fill(0xdeadbeefu);
    const auto output = reinterpret_cast<std::uintptr_t>(Output.data());
    const auto depth = plane != 0 ? plane : reinterpret_cast<std::uintptr_t>(Depth.data());
    const std::array<std::uint32_t, 16> data{
        static_cast<std::uint32_t>(output), static_cast<std::uint32_t>(output >> 32u), sizeof(Output), 0x31016facu,
        static_cast<std::uint32_t>(depth >> 8u), static_cast<std::uint32_t>(depth >> 40u) | (format << 20u) | (((width - 1u) & 3u) << 30u),
        ((width - 1u) >> 2u) | ((Height - 1u) << 14u), 0x90000facu, 0u, 0u, 0u, 0u,
        0x92u, 0u, 0u, 0u,
    };
    const auto source = sampled ? std::span<const std::uint32_t>(SampleCode) : std::span<const std::uint32_t>(LoadCode);
    std::vector<std::uint32_t> code(source.begin(), source.end());
    if (zeroStore) code.insert(code.begin() + 6, 0x7e140280u);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(std::span(code))}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{32, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderRecompiler::ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, data, compute, std::nullopt, std::nullopt, memory}, device.Target(), {0, 0, 0, 128},
    };
    request.useCache = false;
    const auto shader = ShaderRecompiler::Recompile(request);
    device.Dispatch(shader, 1, 1, 1);
    device.WaitIdle();
    for (std::size_t lane = 0; lane < 32; ++lane) {
        Require((format == 5u ? Output[lane] == static_cast<std::uint32_t>(expected) : sampled && format == 7u ? std::abs(std::bit_cast<float>(Output[lane]) - expected) <= 1.0f / 65535.0f : Output[lane] == std::bit_cast<std::uint32_t>(expected)), "depth reuse returned the wrong GPU value at lane " + std::to_string(lane) + ": got " + std::to_string(Output[lane]) + ", expected " + std::to_string(std::bit_cast<std::uint32_t>(expected)));
    }
    Require(std::all_of(Depth.begin(), Depth.end(), [](float value) { return value == 0.25f; }), "depth reuse changed the guest bytes");
}

}

int main(int argc, char** argv) {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const auto shaders = DepthFastClearHarness::Compile(*device);
        const auto address = reinterpret_cast<std::uintptr_t>(Depth.data());
        const bool stencilOnly = argc == 2 && std::string_view(argv[1]) == "--stencil-only";
        if (!stencilOnly) for (const auto format : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM}) {
            AgcDriver::Graphics::ClearDepthSurfaces(device->Device());
            Depth.fill(0.25f);
            const DepthFastClearHarness::Surface surface{address, 0, 0, false, format};
            Require(DepthFastClearHarness::Draw(*device, shaders, surface, {.depthWrite = true}) == DepthFastClearHarness::Covered, "depth producer did not cover the surface");
            Read(*device, format == VK_FORMAT_D16_UNORM ? 7u : 22u, Width, true, 0.5f);
            Read(*device, 22u, format == VK_FORMAT_D16_UNORM ? Width : Width / 2u, false, 0.25f);
        }
        AgcDriver::Graphics::ClearDepthSurfaces(device->Device());
        Depth.fill(0.25f);
        Stencil.fill(0x11u);
        const auto stencilAddress = reinterpret_cast<std::uintptr_t>(Stencil.data());
        const DepthFastClearHarness::Surface combined{address, stencilAddress, 0, false, VK_FORMAT_D32_SFLOAT_S8_UINT};
        VkStencilOpState replace{};
        replace.failOp = replace.passOp = replace.depthFailOp = VK_STENCIL_OP_REPLACE;
        replace.compareOp = VK_COMPARE_OP_ALWAYS;
        replace.compareMask = replace.writeMask = 0xffu;
        replace.reference = 0x7fu;
        Require(DepthFastClearHarness::Draw(*device, shaders, combined, {.depthWrite = true, .stencilTest = true, .stencil = replace}) == DepthFastClearHarness::Covered, "stencil producer did not cover its surface");
        Read(*device, 5u, Width, false, 127.0f, stencilAddress);
        Read(*device, 5u, Width, false, 0.0f, stencilAddress, true);
        VkStencilOpState equal{};
        equal.failOp = equal.passOp = equal.depthFailOp = VK_STENCIL_OP_KEEP;
        equal.compareOp = VK_COMPARE_OP_EQUAL;
        equal.compareMask = 0xffu;
        equal.reference = 0u;
        Require(DepthFastClearHarness::Draw(*device, shaders, combined, {.depthTest = false, .stencilTest = true, .stencil = equal}) == 1u, "storage writes did not reach the native stencil plane or changed other stencil pixels");
        std::puts("depth storage reuse GPU tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
