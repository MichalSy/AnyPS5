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

void ConditionalDescriptor(bool scalar, bool grouped = false, std::uint32_t lanes = Lanes) {
    alignas(256) static constexpr std::array<std::uint32_t, 13> vectorCode{
        0x34020082u, 0x7e040280u, 0xbf06800au, 0xbf850006u,
        0xf4080004u, 0xfa000000u, 0xbf8cc07fu, 0xe0001000u, 0x80000201u,
        0xbf8c3f70u, 0xe0701000u, 0x80010201u, 0xbf810000u,
    };
    alignas(256) static constexpr std::array<std::uint32_t, 14> scalarCode{
        0x34020082u, 0x7e040280u, 0xbf06800au, 0xbf850007u,
        0xf4080004u, 0xfa000000u, 0xbf8cc07fu, 0xf4200200u, 0xfa000004u,
        0xbf8cc07fu, 0x7e040208u, 0xe0701000u, 0x80010201u, 0xbf810000u,
    };
    alignas(256) static constexpr std::array<std::uint32_t, 20> groupedCode{
        0x34020084u, 0x7e040280u, 0x7e060280u, 0x7e080280u, 0x7e0a0280u, 0xbf06800au, 0xbf85000au,
        0xf4080004u, 0xfa000000u, 0xbf8cc07fu, 0xf4280200u, 0xfa000004u, 0xbf8cc07fu,
        0x7e040208u, 0x7e060209u, 0x7e08020au, 0x7e0a020bu, 0xe0781000u, 0x80010201u, 0xbf810000u,
    };
    const auto code = grouped ? std::span<const std::uint32_t>(groupedCode) : scalar ? std::span<const std::uint32_t>(scalarCode) : std::span<const std::uint32_t>(vectorCode);
    const auto components = grouped ? 4u : 1u;
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
    const auto outputDescriptor = BufferDescriptor(output, lanes * components * 4u);
    std::copy(outputDescriptor.begin(), outputDescriptor.end(), userData.begin() + 4);
    const auto tableAddress = reinterpret_cast<std::uintptr_t>(table);
    userData[8] = static_cast<std::uint32_t>(tableAddress);
    userData[9] = static_cast<std::uint32_t>(tableAddress >> 32u);
    const std::array<ShaderRecompiler::MemoryRegion, 2> memory{{
        {reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(std::span(code))},
        {tableAddress, std::as_bytes(std::span(table, 4))},
    }};
    enum class Scenario { Valid, ZeroSelectors, SkippedInvalid, UsedInvalid, InvalidTypeA, InvalidTypeB, ReservedWord1, Records8, Records12, Records16, Absent, Base1, Base2, Base3 };
    std::vector<std::uint32_t> validModule;
    std::vector<std::uint32_t> invalidTypeModule;
    for (const auto mode : {Scenario::Valid, Scenario::ZeroSelectors, Scenario::SkippedInvalid, Scenario::UsedInvalid, Scenario::InvalidTypeA, Scenario::InvalidTypeB, Scenario::ReservedWord1, Scenario::Records8, Scenario::Records12, Scenario::Records16, Scenario::Absent, Scenario::Base1, Scenario::Base2, Scenario::Base3}) {
        if (!grouped && mode >= Scenario::Records8) continue;
        std::fill_n(output, lanes * components, Fill);
        auto descriptor = BufferDescriptor(input, Lanes * 4u);
        if (mode == Scenario::ZeroSelectors) descriptor[3] = 0x18004000u;
        if (mode == Scenario::SkippedInvalid) descriptor = {0x2f7c2c7bu, 0x3bcf2fb5u, 0x2f5a2c65u, 0x3bb72f96u};
        if (mode == Scenario::UsedInvalid) descriptor[3] = (descriptor[3] & ~(0x7fu << 12u)) | (114u << 12u);
        if (mode == Scenario::InvalidTypeA) descriptor[3] = 0x42485d80u;
        if (mode == Scenario::InvalidTypeB) descriptor[3] = 0x60e98f40u;
        if (mode == Scenario::ReservedWord1) descriptor[1] |= 0x40000000u;
        const auto baseOffset = mode == Scenario::Base1 ? 1u : mode == Scenario::Base2 ? 2u : mode == Scenario::Base3 ? 3u : 0u;
        const auto records = mode == Scenario::Records8 ? 8u : mode == Scenario::Records12 ? 12u : mode == Scenario::Records16 ? 16u : mode == Scenario::Absent ? 0u : Lanes * 4u;
        if (mode >= Scenario::Records8) descriptor = BufferDescriptor(reinterpret_cast<const std::byte*>(input) + baseOffset, records);
        std::copy(descriptor.begin(), descriptor.end(), table);
        userData[10] = mode == Scenario::SkippedInvalid ? 0u : 1u;
        auto request = Request(userData, device->ComputeTarget(lanes), memory, code);
        request.context.waveSize = lanes;
        request.context.compute->numThreads[0] = lanes;
        request.useCache = false;
        const auto shader = ShaderRecompiler::Recompile(request);
        if (mode == Scenario::Valid) validModule = shader.spirv.Copy();
        if (mode == Scenario::ZeroSelectors) Require(shader.spirv.Copy() == validModule, "changing a valid conditional descriptor format created another shader module");
        if (mode == Scenario::InvalidTypeA) invalidTypeModule = shader.spirv.Copy();
        if (mode == Scenario::InvalidTypeB) Require(shader.spirv.Copy() == invalidTypeModule, "changing an invalid conditional descriptor type payload created another shader module");
        const bool invalid = !scalar && (mode == Scenario::UsedInvalid || mode == Scenario::InvalidTypeA || mode == Scenario::InvalidTypeB || mode == Scenario::ReservedWord1);
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
        for (std::uint32_t lane = 0; lane < lanes; ++lane) {
            for (std::uint32_t component = 0; component < components; ++component) {
                std::uint32_t expected = scalar && mode != Scenario::SkippedInvalid ? input[1] : mode == Scenario::Valid ? input[lane] : 0u;
                if (grouped) {
                    expected = 0u;
                    if (mode != Scenario::SkippedInvalid && 4u + (component + 1u) * 4u <= records)
                        std::memcpy(&expected, reinterpret_cast<const std::byte*>(input) + baseOffset + 4u + component * 4u, sizeof(expected));
                }
                Require(output[lane * components + component] == expected,
                    "conditional descriptor mode " + std::to_string(static_cast<unsigned>(mode)) + " lane " + std::to_string(lane) + " component " + std::to_string(component) + " expected " + Hex(expected) + " got " + Hex(output[lane * components + component]));
            }
        }
    }
}

void RuntimeStoreFormats() {
#ifdef _WIN32
    auto* block = static_cast<std::byte*>(VirtualAlloc(nullptr, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
    auto* block = static_cast<std::byte*>(std::aligned_alloc(65536, 65536));
#endif
    Require(block != nullptr, "runtime store formats allocation failed");
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
    Require(device != nullptr, "runtime store format GPU unavailable");
    auto* actual = block;
    auto* expected = block + 4096;
    auto* formatWord = reinterpret_cast<std::uint32_t*>(block + 8192);
    for (const auto d16 : {false, true}) {
        for (const auto components : {1u, 4u}) {
            std::vector<std::uint32_t> referenceCode{
                0x34020084u, 0x7e080208u, 0x7e0a0209u, 0x7e0c020au, 0x7e0e020bu,
                0xe0001000u | ((d16 ? 0x84u : 0x04u) + components - 1u) << 18u, 0x80000401u, 0xbf810000u,
            };
            auto runtimeCode = referenceCode;
            runtimeCode.insert(runtimeCode.begin(), {0xe0300000u, 0x80010200u, 0xbf8c3f70u, 0x7e060502u});
            std::array<std::uint32_t, 12> userData{};
            const auto outputDescriptor = BufferDescriptor(actual, Lanes * 16u);
            std::copy(outputDescriptor.begin(), outputDescriptor.end(), userData.begin());
            const auto tableDescriptor = BufferDescriptor(formatWord, 4u);
            std::copy(tableDescriptor.begin(), tableDescriptor.end(), userData.begin() + 4);
            const std::array<std::uint32_t, 4> values = d16 ? std::array<std::uint32_t, 4>{0x00003800u, 0x0000bc00u, 0x00007c00u, 0x00000001u} : std::array<std::uint32_t, 4>{0x3f000000u, 0xbf800000u, 0x7f800000u, 0x00000001u};
            std::copy(values.begin(), values.end(), userData.begin() + 8);
            auto runtimeRequest = Request(userData, device->ComputeTarget(Lanes), {}, runtimeCode);
            runtimeRequest.useCache = false;
            const auto runtimeShader = ShaderRecompiler::Recompile(runtimeRequest);
            for (std::uint32_t format = 1; format <= 77u; ++format) {
                std::memset(actual, 0xcd, Lanes * 16u + 16u);
                std::memset(expected, 0xcd, Lanes * 16u + 16u);
                *formatWord = (0x31016facu & ~(0x7fu << 12u)) | (format << 12u);
                const auto referenceDescriptor = BufferDescriptor(expected, Lanes * 16u);
                std::copy(referenceDescriptor.begin(), referenceDescriptor.end(), userData.begin());
                userData[3] = *formatWord;
                auto referenceRequest = Request(userData, device->ComputeTarget(Lanes), {}, referenceCode);
                referenceRequest.useCache = false;
                const auto referenceShader = ShaderRecompiler::Recompile(referenceRequest);
                device->Dispatch(referenceShader, 1, 1, 1);
                device->WaitIdle();
                device->Dispatch(runtimeShader, 1, 1, 1);
                device->WaitIdle();
                Require(std::memcmp(actual, expected, Lanes * 16u + 16u) == 0, "runtime formatted store differs from specialized store: format " + std::to_string(format) + " components " + std::to_string(components) + " d16 " + std::to_string(d16));
            }
            for (const auto invalidFormat : {0u, 78u}) {
                std::memset(actual, 0xcd, Lanes * 16u + 16u);
                *formatWord = (0x31016facu & ~(0x7fu << 12u)) | (invalidFormat << 12u);
                try {
                    device->Dispatch(runtimeShader, 1, 1, 1);
                    device->WaitIdle();
                } catch (const std::runtime_error& error) {
                    const std::string message(error.what());
                    if (message.find("BDA access failed") == std::string::npos || message.find("reason=8") == std::string::npos) throw;
                }
                Require(std::all_of(actual, actual + Lanes * 16u + 16u, [](std::byte value) { return value == std::byte{0xcd}; }), "invalid runtime formatted store modified its output");
            }
            *formatWord = 0x3104dfacu;
            std::memset(actual, 0xcd, Lanes * 16u + 16u);
            std::memset(expected, 0xcd, Lanes * 16u + 16u);
            const auto limitedActual = BufferDescriptor(actual, Lanes * 16u - 1u);
            std::copy(limitedActual.begin(), limitedActual.end(), userData.begin());
            userData[3] = *formatWord;
            auto limitedRequest = Request(userData, device->ComputeTarget(Lanes), {}, runtimeCode);
            limitedRequest.useCache = false;
            const auto limitedShader = ShaderRecompiler::Recompile(limitedRequest);
            const auto limitedExpected = BufferDescriptor(expected, Lanes * 16u - 1u);
            std::copy(limitedExpected.begin(), limitedExpected.end(), userData.begin());
            userData[3] = *formatWord;
            auto controlRequest = Request(userData, device->ComputeTarget(Lanes), {}, referenceCode);
            controlRequest.useCache = false;
            const auto controlShader = ShaderRecompiler::Recompile(controlRequest);
            device->Dispatch(controlShader, 1, 1, 1);
            device->WaitIdle();
            device->Dispatch(limitedShader, 1, 1, 1);
            device->WaitIdle();
            Require(std::memcmp(actual, expected, Lanes * 16u + 16u) == 0, "runtime formatted store differs at NUM_RECORDS boundary");
            Require(std::all_of(actual + (Lanes - 1u) * 16u, actual + Lanes * 16u + 16u, [](std::byte value) { return value == std::byte{0xcd}; }), "out-of-range runtime formatted store performed a partial write");
            for (const auto misalignment : {1u, 2u, 3u}) {
                std::memset(actual, 0xcd, Lanes * 16u + 16u);
                std::memset(expected, 0xcd, Lanes * 16u + 16u);
                *formatWord = 0x31042facu; // Four SNORM16 components, including negative values.
                const auto unalignedActual = BufferDescriptor(actual + misalignment, Lanes * 16u);
                std::copy(unalignedActual.begin(), unalignedActual.end(), userData.begin());
                userData[3] = *formatWord;
                auto unalignedRequest = Request(userData, device->ComputeTarget(Lanes), {}, runtimeCode);
                unalignedRequest.useCache = false;
                const auto unalignedShader = ShaderRecompiler::Recompile(unalignedRequest);
                // The specialized reference requires an element-aligned base. Compare its
                // aligned bytes with the runtime path's shifted bytes and untouched prefix.
                const auto unalignedExpected = BufferDescriptor(expected, Lanes * 16u);
                std::copy(unalignedExpected.begin(), unalignedExpected.end(), userData.begin());
                userData[3] = *formatWord;
                auto referenceRequest = Request(userData, device->ComputeTarget(Lanes), {}, referenceCode);
                referenceRequest.useCache = false;
                const auto referenceShader = ShaderRecompiler::Recompile(referenceRequest);
                device->Dispatch(referenceShader, 1, 1, 1);
                device->WaitIdle();
                device->Dispatch(unalignedShader, 1, 1, 1);
                device->WaitIdle();
                Require(std::memcmp(actual + misalignment, expected, Lanes * 16u + 8u) == 0, "runtime SNORM16 store differs at base alignment " + std::to_string(misalignment));
                Require(std::all_of(actual, actual + misalignment, [](std::byte value) { return value == std::byte{0xcd}; }), "runtime SNORM16 store modified bytes before its unaligned base");
            }
        }
    }
}

void ScalarReads(AgcDriver::VulkanDevice& device) {
    constexpr std::array<std::uint32_t, 8> scalarCode{0x34020082u, 0xf4200200u, 0xfa000004u, 0xbf8cc07fu, 0x7e040208u, 0xe0701000u, 0x80010201u, 0xbf810000u};
    std::array<std::uint32_t, 8> userData{};
    const auto output = BufferDescriptor(Output.data(), sizeof(Output));
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    auto request = Request(userData, device.ComputeTarget(Lanes), {});
    request.shader.code = scalarCode;
    request.shader.codeAddress = reinterpret_cast<std::uintptr_t>(scalarCode.data());
    request.useCache = false;
    std::uint32_t expected = 0;
    std::memcpy(&expected, Input.data() + 4u, sizeof(expected));
    for (const auto ignoredWord1 : {0u, 0x40000000u}) {
        for (const auto word : {0u, 0x31016facu, 0x91b00f2eu, 0xffffffffu}) {
            auto input = BufferDescriptor(Input.data(), InputBytes);
            input[1] |= ignoredWord1;
            input[3] = word;
            std::copy(input.begin(), input.end(), userData.begin());
            Output.fill(Fill);
            const auto result = ShaderRecompiler::Recompile(request);
            device.Dispatch(result, 1, 1, 1);
            device.WaitIdle();
            Require(std::all_of(Output.begin(), Output.end(), [expected](std::uint32_t value) { return value == expected; }), "scalar buffer read changed with ignored descriptor word " + Hex(word));
        }
    }
    constexpr std::array<std::uint32_t, 13> mixedCode{0xf4200200u, 0xfa000004u, 0xbf8cc07fu, 0x34020082u, 0xe0301000u, 0x80000201u, 0xbf8c3f70u, 0xe0701000u, 0x80010201u, 0x7e060208u, 0xe0701000u, 0x80010301u, 0xbf810000u};
    for (const auto code : {std::span<const std::uint32_t>(Code), std::span<const std::uint32_t>(mixedCode)}) {
        request.shader.code = code;
        request.shader.codeAddress = reinterpret_cast<std::uintptr_t>(code.data());
        try {
            static_cast<void>(ShaderRecompiler::Recompile(request));
        } catch (const std::runtime_error& error) {
            Require(std::string(error.what()).find("buffer descriptor uses an unsupported type") != std::string::npos, "vector buffer descriptor failed for another reason");
            continue;
        }
        throw std::runtime_error("a vector or mixed buffer access accepted a non-buffer descriptor type");
    }
    for (const auto code : {std::span<const std::uint32_t>(Code), std::span<const std::uint32_t>(mixedCode)}) {
        request.shader.code = code;
        request.shader.codeAddress = reinterpret_cast<std::uintptr_t>(code.data());
        auto input = BufferDescriptor(Input.data(), InputBytes);
        input[1] |= 0x40000000u;
        std::copy(input.begin(), input.end(), userData.begin());
        const auto result = ShaderRecompiler::Recompile(request);
        try {
            device.Dispatch(result, 1, 1, 1);
        } catch (const std::runtime_error& error) {
            Require(std::string(error.what()).find("buffer descriptor has reserved bits set") != std::string::npos, "vector or mixed buffer descriptor failed for another reason");
            continue;
        }
        throw std::runtime_error("a vector or mixed buffer access accepted reserved descriptor bits");
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
        ConditionalDescriptor(false);
        ConditionalDescriptor(true);
        ConditionalDescriptor(true, true);
        ConditionalDescriptor(true, true, 64u);
        RuntimeStoreFormats();
        ScalarReads(*device);
        std::puts("buffer descriptor runtime tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
