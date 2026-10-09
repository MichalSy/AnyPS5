#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <span>

namespace {

using AgcDriver::Graphics::Require;
using namespace ShaderRecompiler;

constexpr std::uint32_t ImageCount = 32u;
constexpr std::uint32_t Threads = 32u;

struct alignas(4096) Texture {
    std::array<std::uint32_t, 1024> texels{};
};

struct alignas(4096) DescriptorTable {
    std::array<std::array<std::uint32_t, 8>, ImageCount> images{};
    std::array<std::uint32_t, 4> output{};
};

std::array<Texture, ImageCount> textures{};
DescriptorTable table{};
alignas(4096) std::array<std::uint32_t, ImageCount * Threads> output{};

constexpr auto BuildCode() {
    std::array<std::uint32_t, 7u + ImageCount * 8u> instructions{};
    std::size_t next = 0u;
    instructions[next++] = 0xf4080100u;
    instructions[next++] = 0xfa000000u | (ImageCount * 32u);
    instructions[next++] = 0xbf8cc07fu;
    instructions[next++] = 0x7e3c0300u;
    instructions[next++] = 0x7e3e0280u;
    instructions[next++] = 0x34060082u;
    for (std::uint32_t image = 0u; image < ImageCount; ++image) {
        instructions[next++] = 0xf40c0200u;
        instructions[next++] = 0xfa000000u | (image * 32u);
        instructions[next++] = 0xbf8cc07fu;
        instructions[next++] = 0xf0001108u;
        instructions[next++] = 0x00020a1eu;
        instructions[next++] = 0xbf8c3f70u;
        instructions[next++] = 0xe0701000u | (image * Threads * 4u);
        instructions[next++] = 0x80010a03u;
    }
    instructions[next++] = 0xbf810000u;
    return instructions;
}

alignas(256) constexpr auto code = BuildCode();

std::uint32_t SourceTexture(std::uint32_t image, std::uint32_t iteration) {
    return (image * (iteration == 0u ? 1u : 7u) + iteration * 3u) % ImageCount;
}

std::uint32_t Pixel(std::uint32_t texture, std::uint32_t lane, std::uint32_t iteration) {
    return std::bit_cast<std::uint32_t>(static_cast<float>(iteration * 100000u + texture * 100u + lane + 1u));
}

void Run(AgcDriver::VulkanDevice& device) {
    CompiledShaderArtifact artifact;
    for (std::uint32_t iteration = 0u; iteration < 3u; ++iteration) {
        for (std::uint32_t texture = 0u; texture < ImageCount; ++texture) {
            textures[texture].texels.fill(0u);
            for (std::uint32_t lane = 0u; lane < Threads; ++lane) textures[texture].texels[lane] = Pixel(texture, lane, iteration);
        }
        for (std::uint32_t image = 0u; image < ImageCount; ++image) {
            const auto address = reinterpret_cast<std::uintptr_t>(textures[SourceTexture(image, iteration)].texels.data());
            table.images[image] = {static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>(address >> 40u) | (22u << 20u) | (3u << 30u), 7u, 0x90000facu, 0u, 0u, 0u, 0u};
        }
        output.fill(0xdeadbeefu);
        const auto outputAddress = reinterpret_cast<std::uintptr_t>(output.data());
        table.output = {static_cast<std::uint32_t>(outputAddress), static_cast<std::uint32_t>(outputAddress >> 32u), static_cast<std::uint32_t>(sizeof(output)), 0x31016facu};
        const auto tableAddress = reinterpret_cast<std::uintptr_t>(&table);
        const std::array userData{static_cast<std::uint32_t>(tableAddress), static_cast<std::uint32_t>(tableAddress >> 32u)};
        const std::array regions{
            MemoryRegion{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(std::span(code))},
            MemoryRegion{tableAddress, std::as_bytes(std::span(&table, 1u))}
        };
        const ShaderComputeStageInfo compute{{Threads, 1u, 1u}, 0u, {false, false, false}, false, 1u};
        RecompileRequest request{{ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0u, {}}, {32u, 0u, userData, compute, std::nullopt, std::nullopt, regions}, device.Target(), {0u, 0u, 0u, 128u}};
        const auto shader = Recompile(request);
        const auto heap = std::ranges::find_if(shader.bindings, [](const DescriptorBinding& binding) { return binding.role == DescriptorRole::GuestImages; });
        Require(heap != shader.bindings.end() && heap->count == ImageCount, "32 sampled images did not share a complete typed heap");
        Require(shader.runtimeImageCount == 0u, "direct sampled images allocated bindless runtime metadata");
        if (artifact.variantId == 0u) artifact = shader;
        else Require(shader.cacheHit && shader.variantId == artifact.variantId, "sampled descriptor changes rebuilt the compiled artifact");
        device.Dispatch(shader, 1u, 1u, 1u);
        device.WaitIdle();
        for (std::uint32_t image = 0u; image < ImageCount; ++image) {
            for (std::uint32_t lane = 0u; lane < Threads; ++lane) {
                const auto expected = Pixel(SourceTexture(image, iteration), lane, iteration);
                const auto actual = output[image * Threads + lane];
                Require(actual == expected, "sampled heap execution failed: iteration=" + std::to_string(iteration) + " slot=" + std::to_string(image) + " lane=" + std::to_string(lane) + " value=" + std::to_string(actual) + " expected=" + std::to_string(expected));
            }
        }
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Run(*device);
        std::cout << "32 sampled heap execution tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
