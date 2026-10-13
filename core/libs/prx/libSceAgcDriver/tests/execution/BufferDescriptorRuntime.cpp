#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include <cstdlib>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include "Optimization/ResourceProgram.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Lanes = 32;
constexpr std::uint32_t InputBytes = 256;
constexpr std::uint32_t Fill = 0xdeadbeefu;
alignas(256) std::array<std::uint8_t, InputBytes> Input{};
alignas(256) std::array<std::uint32_t, Lanes> Output{};

alignas(256) constexpr std::array<std::uint32_t, 7> Code{
    0x34020082, 0xe0301000, 0x80000201, 0xbf8c3f70, 0xe0701000, 0x80010201, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 7> FormattedCode{
    0x34020082, 0xe0001000, 0x80000201, 0xbf8c3f70, 0xe0701000, 0x80010201, 0xbf810000,
};

struct Case {
    const char* name;
    std::uint32_t baseOffset;
    std::uint32_t records;
    bool present;
    bool unboundFormat = false;
    bool unmapped = false;
};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

ShaderRecompiler::RecompileRequest Request(std::span<const std::uint32_t> userData, const ShaderRecompiler::SpirvTarget& target, std::span<const ShaderRecompiler::MemoryRegion> memory, std::span<const std::uint32_t> code) {
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Lanes, 1, 1}, 0u, {false, false, false}, false, 1};
    return {
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {Lanes, 0, userData, compute, std::nullopt, std::nullopt, memory},
        target,
        {0, 0, 0, 128}
    };
}

void Run(AgcDriver::VulkanDevice& device, bool registrationModules, bool formatted = false) {
    const std::string mode = registrationModules ? "registration modules" : "specialized modules";
    const auto code = formatted ? std::span<const std::uint32_t>(FormattedCode) : std::span<const std::uint32_t>(Code);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    std::array<std::uint32_t, 8> userData{};
    auto target = device.ComputeTarget(Lanes);
    target.registrationModules = registrationModules;
    const auto request = Request(userData, target, memory, code);
    const auto handle = ShaderRecompiler::PrepareShader(request);
    const std::array<Case, 9> cases{{
        {"aligned", 0, 128, true},
        {"base +1", 1, 128, true},
        {"base +2", 2, 128, true},
        {"base +3", 3, 128, true},
        {"records 37", 0, 37, true},
        {"records 64 base +2", 2, 64, true},
        {"absent", 0, 128, false},
        {"unbound format with backing memory", 0, 128, true, true},
        {"unbound format with unmapped nonzero fields", 0, 18, true, true, true},
    }};
    for (const auto& test : cases) {
        if (formatted && test.baseOffset != 0) continue;
        Output.fill(Fill);
        auto input = test.present ? BufferDescriptor(Input.data() + test.baseOffset, test.records) : std::array<std::uint32_t, 4>{0u, 0u, 0u, 0x31016facu};
        if (test.unboundFormat) input[3] &= ~(0x7fu << 12u);
        if (test.unmapped) input = {0x11u, 0x20000180u, 0x12u, 0u};
        const auto output = BufferDescriptor(Output.data(), sizeof(Output));
        std::copy(input.begin(), input.end(), userData.begin());
        std::copy(output.begin(), output.end(), userData.begin() + 4);
        ShaderRecompiler::SrtRuntime runtime{};
        runtime.userData = userData;
        const auto capture = ShaderRecompiler::CaptureResources(request, runtime, *handle);
        const auto result = ShaderRecompiler::MaterializeShader(request, *capture, *handle);
        Require(!result->registrationModule, "buffer descriptor runtime, " + mode + ", " + test.name + ": a compute shader was emitted in registration form");
        device.Dispatch(*result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(Code.data()));
        device.WaitIdle();
        for (std::uint32_t lane = 0; lane < Lanes; ++lane) {
            std::uint32_t expected = 0;
            if (test.present && !test.unboundFormat && lane * 4u + 4u <= test.records) std::memcpy(&expected, Input.data() + test.baseOffset + lane * 4u, sizeof(expected));
            Require(Output[lane] == expected, "buffer descriptor runtime, " + mode + ", " + test.name + ": lane " + std::to_string(lane) + " is " + Hex(Output[lane]) + ", expected " + Hex(expected));
        }
    }
}

void ConditionalDescriptor(AgcDriver::VulkanDevice& device) {
    alignas(256) static constexpr std::array<std::uint32_t, 13> code{
        0x34020082u, 0x7e040280u, 0xbf06800au, 0xbf850006u,
        0xf4080004u, 0xfa000000u, 0xbf8cc07fu, 0xe0001000u, 0x80000201u,
        0xbf8c3f70u, 0xe0701000u, 0x80010201u, 0xbf810000u,
    };
#ifdef _WIN32
    auto* block = static_cast<std::byte*>(VirtualAlloc(nullptr, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
    auto* block = static_cast<std::byte*>(std::aligned_alloc(65536, 65536));
#endif
    Require(block != nullptr, "conditional buffer descriptor allocation failed");
    GuestAllocations::Mutation().Add(block, 65536, true, true, true);
    struct Cleanup {
        void* block;
        ~Cleanup() {
            GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
            VirtualFree(block, 0, MEM_RELEASE);
#else
            std::free(block);
#endif
        }
    } cleanup{block};
    auto* input = reinterpret_cast<std::uint32_t*>(block);
    auto* output = reinterpret_cast<std::uint32_t*>(block + 8192);
    auto* table = reinterpret_cast<std::uint32_t*>(block + 16384);
    for (std::uint32_t lane = 0; lane < Lanes; ++lane) input[lane] = 0x3f800000u + lane * 0x10000u;
    std::array<std::uint32_t, 12> userData{};
    const auto outputDescriptor = BufferDescriptor(output, Lanes * 4u);
    std::copy(outputDescriptor.begin(), outputDescriptor.end(), userData.begin() + 4);
    const auto tableAddress = reinterpret_cast<std::uintptr_t>(table);
    userData[8] = static_cast<std::uint32_t>(tableAddress);
    userData[9] = static_cast<std::uint32_t>(tableAddress >> 32u);
    const std::array<ShaderRecompiler::MemoryRegion, 2> memory{{
        {reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(std::span(code))},
        {tableAddress, std::as_bytes(std::span(table, 4))},
    }};
    std::vector<ShaderRecompiler::PipelineSpecializationConstant> skippedSpecialization;
    for (const auto mode : {0u, 1u, 4u, 2u, 3u}) {
        std::fill_n(output, Lanes, Fill);
        auto descriptor = BufferDescriptor(input, Lanes * 4u);
        if (mode == 1u || mode == 3u || mode == 4u) descriptor = {0x25f72f5du, 0x3c00345fu, 0x25f22f55u, 0x3c00345du};
        if (mode == 4u) descriptor[3] ^= 0x00400000u;
        if (mode == 3u) descriptor[3] = (descriptor[3] & ~7u) | 2u;
        if (mode == 2u) descriptor = {0x11u, 0x20000180u, 0x12u, 0u};
        std::copy(descriptor.begin(), descriptor.end(), table);
        userData[10] = mode == 1u || mode == 4u ? 0u : 1u;
        auto request = Request(userData, device.ComputeTarget(Lanes), memory, code);
        request.useCache = false;
        const auto shader = ShaderRecompiler::Recompile(request);
        if (mode == 1u) skippedSpecialization = shader.specialization;
        if (mode == 4u) Require(shader.specialization == skippedSpecialization,
            "unused conditional descriptor fields created a new specialization");
        bool faulted = false;
        try {
            device.Dispatch(shader, 1, 1, 1);
            device.WaitIdle();
        } catch (const std::runtime_error& error) {
            const std::string message(error.what());
            if (mode != 3u || message.find("BDA access failed") == std::string::npos || message.find("reason=8") == std::string::npos) throw;
            faulted = true;
        }
        if (mode == 3u) {
            Require(faulted || std::all_of(output, output + Lanes, [](auto value) { return value == Fill; }),
                "an executed invalid conditional descriptor continued to write after its GPU fault");
            continue;
        }
        for (std::uint32_t lane = 0; lane < Lanes; ++lane) {
            Require(output[lane] == (mode == 0u ? input[lane] : 0u),
                "conditional descriptor produced an incorrect value or touched its skipped resource");
        }
    }
}

void PackedGpuStores(AgcDriver::VulkanDevice& device) {
    constexpr std::size_t BlockBytes = 131072;
    alignas(256) static constexpr std::array<std::uint32_t, 18> code{
        0x34020084u, 0xe0301000u, 0x80010801u, 0xe0301004u, 0x80010901u,
        0xe0301008u, 0x80010a01u, 0xe030100cu, 0x80010b01u, 0xbf8c3f70u,
        0xbf06800au, 0xbf850005u, 0xf4080004u, 0xfa000000u, 0xbf8cc07fu,
        0xe01c2000u, 0x80000800u, 0xbf810000u,
    };
#ifdef _WIN32
    auto* block = static_cast<std::byte*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
    auto* block = static_cast<std::byte*>(std::aligned_alloc(65536, BlockBytes));
#endif
    Require(block != nullptr, "packed GPU stores: allocation failed");
    GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    struct Cleanup {
        std::byte* block;
        bool split = false;
        ~Cleanup() {
            GuestAllocations::Mutation().Remove(block);
            if (split) GuestAllocations::Mutation().Remove(block + 65536);
#ifdef _WIN32
            VirtualFree(block, 0, MEM_RELEASE);
#else
            std::free(block);
#endif
        }
    } cleanup{block};
    auto* input = reinterpret_cast<std::uint32_t*>(block);
    auto* table = reinterpret_cast<std::uint32_t*>(block + 16384);
    const std::array<std::uint32_t, 4> values{0x3f800000u, 0xbf000000u, 0x3e800000u, 0xbf800000u};
    for (std::uint32_t lane = 0; lane < Lanes; ++lane) std::copy(values.begin(), values.end(), input + lane * 4u);
    std::array<std::uint32_t, 11> userData{};
    const auto source = BufferDescriptor(input, Lanes * 16u);
    std::copy(source.begin(), source.end(), userData.begin() + 4);
    const auto tableAddress = reinterpret_cast<std::uintptr_t>(table);
    userData[8] = static_cast<std::uint32_t>(tableAddress);
    userData[9] = static_cast<std::uint32_t>(tableAddress >> 32u);
    const std::array<ShaderRecompiler::MemoryRegion, 2> memory{{
        {reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(std::span(code))},
        {tableAddress, std::as_bytes(std::span(table, 4))},
    }};
    auto request = Request(userData, device.ComputeTarget(Lanes), memory, code);
    request.useCache = false;
    const auto handle = ShaderRecompiler::PrepareShader(request);
    struct Format {
        std::uint32_t format, bytes;
        std::array<std::uint32_t, 4> words;
    };
    const std::array<Format, 6> formats{{
        {56, 4, {0x004000ffu}},
        {66, 8, {0xc0007fffu, 0x80012000u}},
        {71, 8, {0xb8003c00u, 0xbc003400u}},
        {64, 8, {0x3f800000u, 0xbf000000u}},
        {74, 12, {0x3f800000u, 0xbf000000u, 0x3e800000u}},
        {77, 16, values},
    }};
    for (const bool split : {false, true}) {
        if (split) {
            GuestAllocations::Mutation().Remove(block);
            GuestAllocations::Mutation().Add(block, 65536, true, true, true);
            GuestAllocations::Mutation().Add(block + 65536, 65536, true, true, true);
            cleanup.split = true;
        }
        for (const auto& format : formats) {
            for (const std::uint32_t offset : {0u, 1u, 2u, 3u, 4u}) {
                auto* origin = block + (split ? 65536 - 32 : 8192);
                auto* destination = origin + offset;
                std::fill_n(origin, 1024, std::byte{0xa5});
                const auto address = reinterpret_cast<std::uintptr_t>(destination);
                const std::array<std::uint32_t, 4> descriptor{
                    static_cast<std::uint32_t>(address),
                    static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (format.bytes << 16u),
                    Lanes - 1u, (format.format << 12u) | 0xfacu,
                };
                std::copy(descriptor.begin(), descriptor.end(), table);
                userData[10] = 1;
                ShaderRecompiler::SrtRuntime runtime{};
                runtime.userData = userData;
                const auto capture = ShaderRecompiler::CaptureResources(request, runtime, *handle);
                const auto shader = ShaderRecompiler::MaterializeShader(request, *capture, *handle);
                Require(std::any_of(shader->bindings.begin(), shader->bindings.end(), [](const auto& binding) { return binding.role == ShaderRecompiler::DescriptorRole::BdaPagetable; }), "packed GPU stores did not exercise address-based descriptors");
                device.Dispatch(*shader, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
                device.WaitIdle();
                for (std::uint32_t lane = 0; lane < Lanes - 1u; ++lane) {
                    if (std::memcmp(destination + lane * format.bytes, format.words.data(), format.bytes) != 0) {
                        std::uint32_t actual;
                        std::memcpy(&actual, destination + lane * format.bytes, sizeof(actual));
                        std::fprintf(stderr, "packed format=%u lane=%u actual=%08x expected=%08x\n", format.format, lane, actual, format.words[0]);
                    }
                    Require(std::memcmp(destination + lane * format.bytes, format.words.data(), format.bytes) == 0,
                        "packed GPU store mismatch: format " + std::to_string(format.format) + ", offset " + std::to_string(offset) + ", split " + std::to_string(split));
                }
                Require(std::all_of(origin, destination, [](std::byte value) { return value == std::byte{0xa5}; }), "packed GPU store modified its prefix");
                Require(std::all_of(destination + (Lanes - 1u) * format.bytes, origin + 1024, [](std::byte value) { return value == std::byte{0xa5}; }), "packed GPU store modified its tail or an out-of-bounds record");
            }
        }
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        for (std::uint32_t index = 0; index < InputBytes; ++index) Input[index] = static_cast<std::uint8_t>(index * 41u + 3u + (index >> 3u));
        Run(*device, false);
        Run(*device, true);
        Run(*device, false, true);
        Run(*device, true, true);
        ConditionalDescriptor(*device);
        PackedGpuStores(*device);
        std::puts("buffer descriptor runtime tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
