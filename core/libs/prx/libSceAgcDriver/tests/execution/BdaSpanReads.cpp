#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 64;
constexpr std::size_t BlockBytes = 65536;
constexpr std::uint32_t InputDwords = 768;
constexpr std::uint32_t Fill = 0xcdcdcdcdu;
constexpr std::uint32_t LoadBase = 0x100;
constexpr std::uint32_t OutputBase = 0x8000;

alignas(256) std::array<std::array<std::uint32_t, 64>, 4> Programs{};

std::span<const std::uint32_t> Code(bool coherent, bool barrier) {
    auto& program = Programs[(coherent ? 2u : 0u) + (barrier ? 1u : 0u)];
    std::vector<std::uint32_t> code{0x34040084, 0x4a0404ff, LoadBase, 0x34080081, 0x36080882, 0x4a040504, 0x34060083, 0x4a0606ff, OutputBase, 0xdc388ff8 | (coherent ? 0x10000u : 0u), 0x04000002, 0xbf8c3f70};
    if (barrier) code.push_back(0xbf8a0000);
    code.insert(code.end(), {0xdc708000, 0x00000503, 0xdc708004, 0x00000703, 0xbf810000});
    std::copy(code.begin(), code.end(), program.begin());
    return std::span<const std::uint32_t>(program.data(), code.size());
}

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        block = static_cast<std::uint8_t*>(std::aligned_alloc(BlockBytes, BlockBytes));
#endif
        Require(block != nullptr, "BDA span reads: cannot allocate the guest block");
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        VirtualFree(block, 0, MEM_RELEASE);
#else
        std::free(block);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint8_t* Data() { return block; }

private:
    std::uint8_t* block = nullptr;
};

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

std::vector<std::uint32_t> Initial() {
    std::vector<std::uint32_t> memory(BlockBytes / 4u, Fill);
    for (std::uint32_t dword = 0; dword < InputDwords; ++dword) memory[dword] = 0x52000000u + dword * 0x00010203u;
    return memory;
}

std::uint32_t Bytes(const std::vector<std::uint32_t>& memory, std::uint32_t address) {
    std::uint32_t value = 0;
    for (std::uint32_t byte = 0; byte < 4u; ++byte) {
        const std::uint32_t at = address + byte;
        value |= ((memory[at / 4u] >> (at % 4u * 8u)) & 0xffu) << (byte * 8u);
    }
    return value;
}

std::vector<std::uint32_t> Expected() {
    const auto initial = Initial();
    auto memory = initial;
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        const std::uint32_t load = LoadBase + tid * 16u + (tid & 1u) * 2u - 8u;
        memory[OutputBase / 4u + tid * 2u] = Bytes(initial, load + 4u);
        memory[OutputBase / 4u + tid * 2u + 1u] = Bytes(initial, load + 12u);
    }
    return memory;
}

ShaderRecompiler::RecompileResult Recompile(std::span<const std::uint32_t> code, std::uint32_t waveSize, std::span<const std::uint32_t> userData,
                                            const ShaderRecompiler::SpirvTarget& target, std::optional<ShaderRecompiler::MemoryRegion> data = std::nullopt) {
    std::vector<ShaderRecompiler::MemoryRegion> regions{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}};
    if (data)
        regions.push_back(*data);
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{{ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
                                               {waveSize, 0, userData, compute, std::nullopt, std::nullopt, regions},
                                               target,
                                               {0, 0, 0, 128}};
    request.useCache = false;
    return ShaderRecompiler::Recompile(request);
}

void Run(AgcDriver::VulkanDevice& device, GuestBlock& guest, bool coherent, bool barrier, std::uint32_t waveSize, const ShaderRecompiler::SpirvTarget& target, const std::string& name) {
    const auto code = Code(coherent, barrier);
    auto memory = Initial();
    std::memcpy(guest.Data(), memory.data(), BlockBytes);
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(guest.Data()));
    const std::vector<std::uint32_t> userData{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u)};
    const auto result = Recompile(code, waveSize, userData, target);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
    std::memcpy(memory.data(), guest.Data(), BlockBytes);
    const auto expected = Expected();
    for (std::size_t dword = 0; dword < memory.size(); ++dword) {
        Require(memory[dword] == expected[dword], "BDA span reads: " + name + (coherent ? " glc" : "") + (barrier ? " barrier" : "") + " byte " + std::to_string(dword * 4u) + " is " + Hex(memory[dword]) + ", expected " + Hex(expected[dword]));
    }
}

void RunScalar(AgcDriver::VulkanDevice& device, GuestBlock& guest, std::uint32_t waveSize, const ShaderRecompiler::SpirvTarget& target) {
    // s_load_dwordx4 s[8:11], s[0:1], s2; copy to v[1:4]; write one vector per
    // lane.
    alignas(256) const std::array<std::uint32_t, 13> code{0xf4080200u, 0x04000000u, 0xbf8cc07fu, 0x7e020208u, 0x7e040209u, 0x7e06020au, 0x7e08020bu,
                                                          0x340a0084u, 0x4a0a0affu, OutputBase,  0xdc788000u, 0x00000105u, 0xbf810000u};
    for (const std::uint32_t offset : {LoadBase, LoadBase + 4u, LoadBase + 12u}) {
        auto memory = Initial();
        std::memcpy(guest.Data(), memory.data(), BlockBytes);
        const auto address = reinterpret_cast<std::uintptr_t>(guest.Data());
        const std::array<std::uint32_t, 3> userData{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), offset};
        const auto result = Recompile(code, waveSize, userData, target);
        device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
        device.WaitIdle();
        std::memcpy(memory.data(), guest.Data(), BlockBytes);
        auto expected = Initial();
        for (std::uint32_t lane = 0; lane < Threads; ++lane) {
            for (std::uint32_t component = 0; component < 4; ++component)
                expected[OutputBase / 4u + lane * 4u + component] = expected[offset / 4u + component];
        }
        for (std::size_t index = 0; index < memory.size(); ++index) {
            Require(memory[index] == expected[index], "BDA scalar group at " + std::to_string(index * 4) + " offset " + std::to_string(offset) + " wave " +
                                                          std::to_string(waveSize) + " got " + Hex(memory[index]) + " expected " + Hex(expected[index]));
        }
    }
}

void RunScalarBuffer(AgcDriver::VulkanDevice& device, GuestBlock& guest, std::uint32_t waveSize, const ShaderRecompiler::SpirvTarget& target) {
    // Two s_buffer_load_dwordx4 instructions reuse one runtime V#; store both vectors per lane.
    alignas(256) const std::array<std::uint32_t, 27> code{
        0xbf068004u, 0xbf850002u, 0xf4080200u, 0x04000000u, 0xbf8cc07fu, 0xf4280404u, 0x06000000u, 0xbf8cc07fu, 0xf4280504u,
        0xfa000000u, 0xbf8cc07fu, 0x7e020210u, 0x7e040211u, 0x7e060212u, 0x7e080213u, 0x340a0085u, 0x4a0a0affu, OutputBase,
        0xdc788000u, 0x00000105u, 0x7e020214u, 0x7e040215u, 0x7e060216u, 0x7e080217u, 0xdc788010u, 0x00000105u, 0xbf810000u,
    };
    struct Case {
        std::uint32_t source;
        std::uint32_t offset;
        std::uint32_t records;
    };
    const std::array<Case, 11> cases{{
        {0x1000u, 32u, 48u},
        {0x1001u, 32u, 48u},
        {0x1000u, 32u, 0u},
        {0x1000u, 32u, 44u},
        {0x1000u, 32u, 40u},
        {0x1000u, 32u, 36u},
        {BlockBytes - 32u, 16u, 64u},
        {BlockBytes - 32u, 16u, 32u},
        {BlockBytes - 32u, 16u, 28u},
        {BlockBytes - 32u, 16u, 24u},
        {BlockBytes - 32u, 16u, 20u},
    }};
    for (const auto& test : cases) {
        auto initial = Initial();
        const auto address = reinterpret_cast<std::uintptr_t>(guest.Data());
        const auto source = address + test.source;
        initial[LoadBase / 4u] = static_cast<std::uint32_t>(source);
        initial[LoadBase / 4u + 1u] = static_cast<std::uint32_t>(source >> 32u);
        initial[LoadBase / 4u + 2u] = test.records;
        initial[LoadBase / 4u + 3u] = 56u << 12u;
        for (std::uint32_t component = 0; component < 4u; ++component) {
            const auto first = 0x92000000u + component;
            const auto second = 0x91000000u + component;
            std::memcpy(reinterpret_cast<std::byte*>(initial.data()) + test.source + test.offset + component * 4u, &first, 4);
            std::memcpy(reinterpret_cast<std::byte*>(initial.data()) + test.source + component * 4u, &second, 4);
        }
        std::memcpy(guest.Data(), initial.data(), BlockBytes);
        const std::array<std::uint32_t, 5> userData{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), LoadBase, test.offset, 1u};
        const auto result = Recompile(code, waveSize, userData, target, ShaderRecompiler::MemoryRegion{address, std::as_bytes(std::span(initial))});
        device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
        device.WaitIdle();
        auto memory = initial;
        std::memcpy(memory.data(), guest.Data(), BlockBytes);
        auto expected = initial;
        for (std::uint32_t lane = 0; lane < Threads; ++lane) {
            for (std::uint32_t component = 0; component < 4u; ++component) {
                expected[OutputBase / 4u + lane * 8u + component] = test.offset + component * 4u + 4u <= test.records ? 0x92000000u + component : 0u;
                expected[OutputBase / 4u + lane * 8u + 4u + component] = component * 4u + 4u <= test.records ? 0x91000000u + component : 0u;
            }
        }
        Require(memory == expected, "BDA scalar buffer reuse changed bounds, values, or neighboring bytes");
    }
}
}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        GuestBlock guest;
        const auto native = device->Target();
        const auto split = device->ComputeTarget(32);
        for (const bool coherent : {false, true}) {
            for (const bool barrier : {false, true}) {
                Run(*device, guest, coherent, barrier, 32, native, "wave32");
                Run(*device, guest, coherent, barrier, 64, native, "wave64");
                if (split.subgroupSize != native.subgroupSize) Run(*device, guest, coherent, barrier, 64, split, "wave64 split");
            }
        }
        RunScalar(*device, guest, 32, native);
        RunScalar(*device, guest, 64, native);
        RunScalarBuffer(*device, guest, 32, native);
        RunScalarBuffer(*device, guest, 64, native);
        std::puts("BDA span read tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
