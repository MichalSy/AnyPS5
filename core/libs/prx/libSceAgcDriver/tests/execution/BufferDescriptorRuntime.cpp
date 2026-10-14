#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include <algorithm>
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

struct Case {
    const char* name;
    std::uint32_t baseOffset;
    std::uint32_t records;
    bool present;
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

ShaderRecompiler::RecompileRequest Request(std::span<const std::uint32_t> userData, const ShaderRecompiler::SpirvTarget& target, std::span<const ShaderRecompiler::MemoryRegion> memory, std::span<const std::uint32_t> code = Code) {
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Lanes, 1, 1}, 0u, {false, false, false}, false, 1};
    return {
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {Lanes, 0, userData, compute, std::nullopt, std::nullopt, memory},
        target,
        {0, 0, 0, 128}
    };
}

void Run(AgcDriver::VulkanDevice& device, bool registrationModules) {
    const std::string mode = registrationModules ? "registration modules" : "specialized modules";
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(Code.data()), std::as_bytes(std::span(Code))}}};
    std::array<std::uint32_t, 8> userData{};
    auto target = device.ComputeTarget(Lanes);
    target.registrationModules = registrationModules;
    const auto request = Request(userData, target, memory);
    const auto handle = ShaderRecompiler::PrepareShader(request);
    const std::array<Case, 7> cases{{
        {"aligned", 0, 128, true},
        {"base +1", 1, 128, true},
        {"base +2", 2, 128, true},
        {"base +3", 3, 128, true},
        {"records 37", 0, 37, true},
        {"records 64 base +2", 2, 64, true},
        {"absent", 0, 128, false},
    }};
    for (const auto& test : cases) {
        Output.fill(Fill);
        const auto input = test.present ? BufferDescriptor(Input.data() + test.baseOffset, test.records) : std::array<std::uint32_t, 4>{0u, 0u, 0u, 0x31016facu};
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
            if (test.present && lane * 4u + 4u <= test.records) std::memcpy(&expected, Input.data() + test.baseOffset + lane * 4u, sizeof(expected));
            Require(Output[lane] == expected, "buffer descriptor runtime, " + mode + ", " + test.name + ": lane " + std::to_string(lane) + " is " + Hex(Output[lane]) + ", expected " + Hex(expected));
        }
    }
}

void ConditionalDescriptor() {
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
    const auto device = OpenVulkanTestDevice();
    Require(device != nullptr, "conditional descriptor test could not open the GPU");
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
    enum class Scenario { Valid, SkippedInvalid, UsedInvalid, InvalidTypeA, InvalidTypeB };
    std::vector<std::uint32_t> invalidTypeModule;
    for (const auto mode : {Scenario::Valid, Scenario::SkippedInvalid, Scenario::UsedInvalid, Scenario::InvalidTypeA, Scenario::InvalidTypeB}) {
        std::fill_n(output, Lanes, Fill);
        auto descriptor = BufferDescriptor(input, Lanes * 4u);
        if (mode == Scenario::SkippedInvalid) descriptor = {0x2f7c2c7bu, 0x3bcf2fb5u, 0x2f5a2c65u, 0x3bb72f96u};
        if (mode == Scenario::UsedInvalid) descriptor[3] = (descriptor[3] & ~(0x7fu << 12u)) | (114u << 12u);
        if (mode == Scenario::InvalidTypeA) descriptor[3] = 0x42485d80u;
        if (mode == Scenario::InvalidTypeB) descriptor[3] = 0x60e98f40u;
        std::copy(descriptor.begin(), descriptor.end(), table);
        userData[10] = mode == Scenario::SkippedInvalid ? 0u : 1u;
        auto request = Request(userData, device->ComputeTarget(Lanes), memory, code);
        request.useCache = false;
        const auto shader = ShaderRecompiler::Recompile(request);
        if (mode == Scenario::InvalidTypeA) invalidTypeModule = shader.spirv.Copy();
        if (mode == Scenario::InvalidTypeB) Require(shader.spirv.Copy() == invalidTypeModule, "changing an invalid conditional descriptor type payload created another shader module");
        const bool invalid = mode == Scenario::UsedInvalid || mode == Scenario::InvalidTypeA || mode == Scenario::InvalidTypeB;
        bool faulted = false;
        try {
            device->Dispatch(shader, 1, 1, 1);
            device->WaitIdle();
        } catch (const std::runtime_error& error) {
            const std::string message(error.what());
            if (!invalid || message.find("BDA access failed") == std::string::npos || message.find("reason=8") == std::string::npos) throw;
            faulted = true;
        }
        if (invalid) {
            Require(faulted || std::all_of(output, output + Lanes, [](auto value) { return value == Fill; }),
                "an executed invalid conditional descriptor continued to write after its GPU fault");
            continue;
        }
        for (std::uint32_t lane = 0; lane < Lanes; ++lane) {
            Require(output[lane] == (mode == Scenario::Valid ? input[lane] : 0u),
                "conditional descriptor produced an incorrect value or touched its skipped resource");
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
        ConditionalDescriptor();
        std::puts("buffer descriptor runtime tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
