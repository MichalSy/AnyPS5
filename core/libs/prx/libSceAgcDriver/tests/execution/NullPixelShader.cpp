#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/tests/AlignedByteArray.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawCache.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderInputState.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <iostream>
#include <string_view>
#include <vector>
#include <spirv/unified1/spirv.hpp>

namespace {

using AgcDriver::Graphics::Require;
using namespace AgcDriver::DriverDetail;
using namespace ShaderRecompiler;

constexpr std::uint32_t Width = 64;
constexpr std::uint32_t Height = 16;
const auto PixelStorage = MakeAlignedByteArray<16384, 16384>();
const auto DepthStorage = MakeAlignedByteArray<65536, 16384>();
const auto StencilStorage = MakeAlignedByteArray<65536, 16384>();
auto& Pixels = *PixelStorage;
auto& Depth = *DepthStorage;
auto& Stencil = *StencilStorage;
alignas(256) std::array<std::array<float, 4>, 3> Vertices{{{-1, -1, 0.25f, 1}, {3, -1, 0.25f, 1}, {-1, 3, 0.25f, 1}}};
alignas(256) constexpr std::array<std::uint32_t, 6> VertexCode{0xe0382000u, 0x80020005u, 0xbf8c3f70u, 0xf80008cfu, 0x03020100u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 4> PixelCode{0x7e0e02f2u, 0xf800180fu, 0x07070707u, 0xbf810000u};

std::shared_ptr<ShaderSnapshot> Snapshot(std::uint64_t address, std::uint8_t type) {
    auto snapshot = std::make_shared<ShaderSnapshot>();
    snapshot->codeAddress = address;
    snapshot->type = type;
    snapshot->code = {0xbf810000u};
    snapshot->header.resize(sizeof(Shader));
    Shader header{};
    header.type = type;
    std::memcpy(snapshot->header.data(), &header, sizeof(header));
    return snapshot;
}

void Bind(AgcDriver::QueueState& queue, std::uint32_t base, std::uint64_t address) {
    queue.shader[base] = static_cast<std::uint32_t>(address >> 8u);
    queue.shader[base + 1u] = static_cast<std::uint32_t>(address >> 40u);
}

void CheckDecodedAbi(AgcDriver::VulkanDevice& device) {
    auto vertex = Snapshot(0x10000u, 2);
    auto fragment = Snapshot(0x20000u, 1);
    auto null = Snapshot(NullPixelProgramAddress(), 1);
    ShaderRegistry registry{{vertex->codeAddress, vertex}, {fragment->codeAddress, fragment}, {null->codeAddress, null}};
    AgcDriver::QueueState queue;
    queue.context[0x2d5] = 0x2000;
    queue.userConfig[0x242] = 4;
    queue.context[0x1b3] = queue.context[0x1b4] = 2;
    queue.context[0x1b6] = 0x8000;
    queue.context[0x1c5] = 9;
    queue.context[0x8e] = queue.context[0x8f] = 0xf;
    Bind(queue, 0xc8, vertex->codeAddress);
    Bind(queue, 0x8, fragment->codeAddress);
    queue.shader[0x8b] = 0;
    queue.shader[0xb] = 4u << 1u;
    for (std::uint32_t i = 0; i < 4; ++i) queue.shader[0xc + i] = 0x12340000u + i;
    DrawDecode active{};
    active.state.stages = AgcDriver::Graphics::DecodeShaderStages(queue);
    DecodeGraphicsPrograms(active, queue, registry, false, true);
    Require(active.programs.back().binary.codeAddress == fragment->codeAddress && active.programs.back().userData.size() == 4 && active.programs.back().userData.front() == 0x12340000u && active.state.stages.fragmentWaveSize == 32, "active pixel shader lost its user SGPRs or wave32");
    const auto canonical = AgcDriver::Graphics::DecodePixelStageInfo({}, {}, true);
    RecompileRequest prepared{{ShaderStage::Fragment, null->codeAddress, null->code, 0, null->header}, {64, 0, {}, {}, canonical, {}, {}}, device.Target(), {0, 0, 0, 128}};
    null->prepared->entries.push_back({0, PrepareShader(prepared)});
    prepared.layout.pushConstantSizeBytes = 0;
    null->prepared->entries.push_back({0, PrepareShader(prepared)});
    for (const bool explicitNull : {false, true}) {
        auto skipped = queue;
        skipped.context[0x8e] = 0;
        skipped.context[0x203] = skipped.context[0x1c4] = 0;
        skipped.context[0x1b3] = skipped.context[0x1b4] = 0xffffffffu;
        skipped.context[0x1b6] = 0x8000u | 31u;
        skipped.context[0x1c5] = 0xffffffffu;
        if (explicitNull) Bind(skipped, 0x8, 0);
        Require(AgcDriver::Graphics::PixelProgramSkipped(skipped), "null pixel fixture still runs a real shader");
        for (const bool staticAbi : {false, true}) {
            DrawDecode decoded{};
            decoded.state.stages = AgcDriver::Graphics::DecodeShaderStages(skipped);
            DecodeGraphicsPrograms(decoded, skipped, registry, staticAbi, true);
            decoded.pixel = AgcDriver::Graphics::DecodePixelStageInfo(skipped.context, {}, true);
            const auto& program = decoded.programs.back();
            Require(program.binary.codeAddress == null->codeAddress && program.userData.empty() && program.firstUserSgpr == 0, "null pixel shader inherited stale PS_RSRC2 user SGPRs");
            Require(decoded.state.stages.fragmentWaveSize == 64 && !decoded.pixel.wave32, "null pixel shader inherited stale wave32");
            for (const auto layout : {BindingLayout{0, 0, 0, 128}, BindingLayout{0, 0, 20, 108}, BindingLayout{0, 0, 0, 0}}) {
                RecompileRequest request{program.binary, {decoded.state.stages.fragmentWaveSize, program.firstUserSgpr, program.userData, {}, decoded.pixel, {}, program.memory}, device.Target(), layout};
                const auto handle = SourceHandleFor(*program.snapshot, program.codeOffset, request);
                const auto invocation = InvocationFor(*program.snapshot, program.codeOffset, request);
                Require(handle == null->prepared->entries.front().handle || handle == null->prepared->entries.back().handle, "null pixel lookup prepared a replacement artifact");
                Require(invocation.Request().context.waveSize == 64 && invocation.Request().context.userData.empty(), "prepared null invocation changed its canonical ABI");
            }
        }
    }
    for (const bool maskedColor : {false, true}) {
        for (const bool highOnly : {false, true}) {
            auto invalid = queue;
            if (maskedColor) {
                invalid.context[0x8e] = 0;
                invalid.context[0x203] = invalid.context[0x1c4] = 0;
            }
            if (highOnly) invalid.shader.erase(0x8);
            invalid.shader[0x9] = 0x100u;
            bool rejected = false;
            try {
                DrawDecode decoded{};
                decoded.state.stages = AgcDriver::Graphics::DecodeShaderStages(invalid);
                DecodeGraphicsPrograms(decoded, invalid, registry, false, true);
            } catch (const std::exception& error) {
                Require(std::string_view(error.what()).find("reserved graphics program address bits") != std::string_view::npos, "reserved pixel address bits failed for an unrelated reason");
                rejected = true;
            }
            Require(rejected, "reserved pixel program address bits were discarded as a null shader");
        }
    }
    std::cout << "null pixel decoded ABI, active pixel controls and reserved address rejection passed\n";
}

AgcDriver::QueueState Queue() {
    AgcDriver::QueueState queue;
    queue.userConfig[0x242] = 4;
    queue.context = {
        {0x000, 0}, {0x002, 0}, {0x007, ((Height - 1u) << 16u) | (Width - 1u)},
        {0x00a, 3}, {0x00b, std::bit_cast<std::uint32_t>(0.25f)}, {0x010, 3}, {0x011, 1},
        {0x10b, 0x333333}, {0x10c, 0x00ffff03}, {0x10d, 0x00ffff03},
        {0x2d5, 0x2000}, {0x1b6, 0x8000}, {0x207, 0}, {0x200, 0x007007f7}, {0x203, 0x800},
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
        {0x94, 0x80000000}, {0x95, (Height << 16u) | Width},
        {0xb4, 0}, {0xb5, std::bit_cast<std::uint32_t>(1.0f)}
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
    queue.context[0x10f] = queue.context[0x110] = std::bit_cast<std::uint32_t>(Width * 0.5f);
    queue.context[0x111] = std::bit_cast<std::uint32_t>(Height * -0.5f);
    queue.context[0x112] = std::bit_cast<std::uint32_t>(Height * 0.5f);
    queue.context[0x113] = std::bit_cast<std::uint32_t>(1.0f);
    queue.context[0x114] = 0;
    Bind(queue, 0xc8, reinterpret_cast<std::uintptr_t>(VertexCode.data()));
    Bind(queue, 0x8, reinterpret_cast<std::uintptr_t>(PixelCode.data()));
    queue.shader[0x8b] = queue.shader[0xb] = 4u << 1u;
    const auto address = reinterpret_cast<std::uintptr_t>(Vertices.data());
    const std::array<std::uint32_t, 4> descriptor{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (16u << 16u), 3u, 0x01016facu};
    for (std::size_t i = 0; i < descriptor.size(); ++i) {
        queue.shader[0x8c + i] = descriptor[i];
        queue.shader[0xc + i] = 0x12340000u + static_cast<std::uint32_t>(i);
    }
    return queue;
}

template<std::size_t RegisterCount, std::size_t ContextCount>
struct Header {
    Shader shader{};
    ShaderUserData users{};
    std::array<ShaderRegister, RegisterCount> registers{};
    std::array<ShaderRegister, ContextCount> context{};

    void Initialize(std::uint8_t type, const void* code, std::size_t bytes) {
        shader.file_header = 0x34333231u;
        shader.version = 0x18;
        shader.type = type;
        shader.header_size = sizeof(*this);
        shader.shader_size = bytes;
        shader.code = code;
        shader.user_data = &users;
        shader.sh_registers = registers.data();
        shader.num_sh_registers = registers.size();
        shader.cx_registers = context.data();
        shader.num_cx_registers = context.size();
    }
};

void Submit(const AgcDriver::QueueState& queue) {
    std::vector<std::uint32_t> commands;
    for (const auto [offset, value] : queue.shader) commands.insert(commands.end(), {0xc0017600u, offset, value});
    for (const auto [offset, value] : queue.context) commands.insert(commands.end(), {0xc0016900u, offset, value});
    for (const auto [offset, value] : queue.userConfig) commands.insert(commands.end(), {0xc0017900u, offset, value});
    commands.insert(commands.end(), {0xc0002f00u, 1u, 0xc0012d00u, 3u, 2u});
    const Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "null pixel draw submission failed");
    AgcDriverWaitIdle_nid_postfix();
}

void CheckRegisteredDraw(bool rectangleOnly = false, bool pairAbi = false) {
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(Pixels.data(), Pixels.size(), true, true, true);
        mutation.Add(Depth.data(), Depth.size(), true, true, true);
        mutation.Add(Stencil.data(), Stencil.size(), true, true, true);
    }
    auto queue = Queue();
    Header<3, 1> vertex;
    vertex.Initialize(2, VertexCode.data(), sizeof(VertexCode));
    vertex.registers = {{{0xc8, queue.shader.at(0xc8)}, {0xc9, queue.shader.at(0xc9)}, {0x8b, queue.shader.at(0x8b)}}};
    vertex.context = {{{0x2d5, 0x2000}}};
    Header<3, 6> pixel;
    pixel.Initialize(1, PixelCode.data(), sizeof(PixelCode));
    pixel.registers = {{{0x8, queue.shader.at(0x8)}, {0x9, queue.shader.at(0x9)}, {0xb, queue.shader.at(0xb)}}};
    pixel.context = {{{0x1b3, 2}, {0x1b4, 2}, {0x1b6, 0x8000}, {0x1c5, 9}, {0x203, 0x800}, {0x1c4, 0}}};
    AgcDriverRegisterShader_nid_postfix(&vertex.shader);
    AgcDriverRegisterShader_nid_postfix(&pixel.shader);
    const std::array<const Shader*, 1> stages{&vertex.shader};
    for (const std::uint32_t primitiveType : {4u, 17u}) {
        if (rectangleOnly && primitiveType == 4u) continue;
        const std::array<ShaderRegister, 1> primitive{{{0x242, primitiveType}}};
        if (pairAbi) AgcDriverResolveGraphicsAbi_nid_postfix(&vertex.shader, &pixel.shader, primitiveType);
        else AgcDriverResolveGraphicsStagesAbi_nid_postfix(stages, {}, primitive);
        for (const bool explicitNull : {false, true}) {
            for (auto& position : Vertices) position[2] = 0.25f;
            Submit(queue);
            auto skipped = queue;
            skipped.userConfig[0x242] = primitiveType;
            skipped.context[0x8e] = 0;
            skipped.context[0x203] = 0;
            skipped.context[0x1b3] = skipped.context[0x1b4] = 0xffffffffu;
            skipped.context[0x1b6] = 0x8000u | 31u;
            skipped.context[0x1c5] = 0xffffffffu;
            skipped.context[0x91] = (Height << 16u) | (Width / 2u);
            skipped.context[0x10c] = skipped.context[0x10d] = 0x00ffff07;
            if (explicitNull) Bind(skipped, 0x8, 0);
            for (auto& position : Vertices) position[2] = 0.75f;
            Submit(skipped);
            Pixels.fill(std::byte{0x40});
            auto verify = queue;
            verify.context[0x200] = 0x002002a3;
            verify.context[0x10b] = 0;
            verify.context[0x10c] = verify.context[0x10d] = 0x00ffff07;
            Submit(verify);
            for (std::uint32_t y = 0; y < Height; ++y) {
                for (std::uint32_t x = 0; x < Width; ++x) {
                    for (std::uint32_t channel = 0; channel < 4; ++channel) {
                        const auto expected = x < Width / 2u ? std::byte{255} : std::byte{0x40};
                        Require(Pixels[(y * Width + x) * 4u + channel] == expected, "registered null pixel draw lost stencil coverage, scissor or its depth write: primitive=" + std::to_string(primitiveType) + " explicitNull=" + std::to_string(explicitNull));
                    }
                }
            }
        }
    }
    {
        GuestAllocations::Mutation mutation;
        mutation.Remove(Stencil.data());
        mutation.Remove(Depth.data());
        mutation.Remove(Pixels.data());
    }
    std::cout << "registered null pixel invocation, stale state and depth/stencil draw effects passed\n";
}

void CheckDepthOnly(std::uint32_t primitiveType, bool explicitNull) {
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(Pixels.data(), Pixels.size(), true, true, true);
        mutation.Add(Depth.data(), Depth.size(), true, true, true);
        mutation.Add(Stencil.data(), Stencil.size(), true, true, true);
    }
    auto queue = Queue();
    Header<3, 1> vertex;
    vertex.Initialize(2, VertexCode.data(), sizeof(VertexCode));
    vertex.registers = {{{0xc8, queue.shader.at(0xc8)}, {0xc9, queue.shader.at(0xc9)}, {0x8b, queue.shader.at(0x8b)}}};
    vertex.context = {{{0x2d5, 0x2000}}};
    Header<3, 6> pixel;
    pixel.Initialize(1, PixelCode.data(), sizeof(PixelCode));
    pixel.registers = {{{0x8, queue.shader.at(0x8)}, {0x9, queue.shader.at(0x9)}, {0xb, queue.shader.at(0xb)}}};
    pixel.context = {{{0x1b3, 2}, {0x1b4, 2}, {0x1b6, 0x8000}, {0x1c5, 9}, {0x203, 0x800}, {0x1c4, 0}}};
    AgcDriverRegisterShader_nid_postfix(&vertex.shader);
    AgcDriverRegisterShader_nid_postfix(&pixel.shader);
    const std::array<const Shader*, 1> stages{&vertex.shader};
    const std::array<ShaderRegister, 1> primitive{{{0x242, primitiveType}}};
    AgcDriverResolveGraphicsStagesAbi_nid_postfix(stages, {}, primitive);
    auto depthOnly = queue;
    depthOnly.userConfig[0x242] = primitiveType;
    depthOnly.shader.erase(0x8);
    depthOnly.shader.erase(0x9);
    if (explicitNull) Bind(depthOnly, 0x8, 0);
    Require(explicitNull || (!depthOnly.shader.contains(0x8) && !depthOnly.shader.contains(0x9)), "depth-only fixture supplied a pixel program address");
    depthOnly.shader[0xb] = 0xffffffffu;
    depthOnly.context[0x8e] = 0;
    depthOnly.context[0x203] = 0;
    depthOnly.context[0x1b3] = depthOnly.context[0x1b4] = 0xffffffffu;
    depthOnly.context[0x1b6] = 0x8000u | 31u;
    depthOnly.context[0x1c5] = 0xffffffffu;
    depthOnly.context[0x31c] = 0;
    const auto initial = AgcDriver::Graphics::DecodeState(depthOnly);
    Require(!initial.hasColorTarget && initial.depth && initial.depthWrite && initial.stencilTest, "depth-only fixture lost its depth/stencil target or retained a color target");
    const auto unchanged = [] {
        Require(std::all_of(Pixels.begin(), Pixels.end(), [](std::byte value) { return value == std::byte{0x40}; }), "depth/stencil-only draw changed color memory");
    };
    Pixels.fill(std::byte{0x40});
    for (auto& position : Vertices) position[2] = 0.25f;
    Submit(depthOnly);
    unchanged();
    depthOnly.context[0x91] = (Height << 16u) | (Width / 2u);
    depthOnly.context[0x10c] = depthOnly.context[0x10d] = 0x00ffff07;
    for (auto& position : Vertices) position[2] = 0.75f;
    Submit(depthOnly);
    unchanged();
    depthOnly.context[0x90] = 0x80000000u | (Width / 2u);
    depthOnly.context[0x91] = (Height << 16u) | (Width * 3u / 4u);
    depthOnly.context[0x200] = 0x007007f5;
    depthOnly.context[0x10c] = depthOnly.context[0x10d] = 0x00ffff09;
    for (auto& position : Vertices) position[2] = 0.125f;
    Submit(depthOnly);
    unchanged();
    for (const auto reference : {7u, 9u, 3u}) {
        Pixels.fill(std::byte{0x40});
        for (auto& position : Vertices) position[2] = reference == 7u ? 0.75f : 0.25f;
        auto verify = queue;
        verify.context[0x200] = 0x002002a3;
        verify.context[0x10b] = 0;
        verify.context[0x10c] = verify.context[0x10d] = 0x00ffff00u | reference;
        Submit(verify);
        for (std::uint32_t y = 0; y < Height; ++y) {
            for (std::uint32_t x = 0; x < Width; ++x) {
                const auto stencil = x < Width / 2u ? 7u : x < Width * 3u / 4u ? 9u : 3u;
                const auto expected = stencil == reference ? std::byte{255} : std::byte{0x40};
                for (std::uint32_t channel = 0; channel < 4; ++channel) {
                    Require(Pixels[(y * Width + x) * 4u + channel] == expected, "depth-only draw lost depth, stencil or scissor: primitive=" + std::to_string(primitiveType) + " explicitNull=" + std::to_string(explicitNull) + " stencil=" + std::to_string(reference) + " x=" + std::to_string(x) + " y=" + std::to_string(y));
                }
            }
        }
    }
    {
        GuestAllocations::Mutation mutation;
        mutation.Remove(Stencil.data());
        mutation.Remove(Depth.data());
        mutation.Remove(Pixels.data());
    }
    std::cout << "depth-only null pixel draw, stencil-only depth preservation, scissor and color preservation passed\n";
}

alignas(256) constexpr std::array<std::uint32_t, 8> PixelDepthCode{
    0x7e0e02f2u, 0x7e0002ffu, 0x3f400000u, 0xf8001081u,
    0x00000000u, 0xf800180fu, 0x07070707u, 0xbf810000u
};

bool HasExecutionMode(std::span<const std::uint32_t> words, spv::ExecutionMode mode) {
    for (std::size_t offset = 5; offset < words.size();) {
        const auto count = words[offset] >> 16u;
        Require(count != 0u && count <= words.size() - offset, "invalid prepared pixel instruction");
        if ((words[offset] & 0xffffu) == spv::OpExecutionMode && count >= 3u && words[offset + 2u] == mode) return true;
        offset += count;
    }
    return false;
}

void CheckDepthExportAbi(AgcDriver::VulkanDevice& device) {
    for (const std::uint32_t zOrder : {0u, 1u}) {
        auto snapshot = Snapshot(0x30000u, 1);
        snapshot->code.assign(PixelDepthCode.begin(), PixelDepthCode.end());
        AgcDriver::Registers context{
            {0x1b3, 2}, {0x1b4, 2}, {0x1b6, 0x8001}, {0x191, 32},
            {0x1c5, 9}, {0x203, 0x801u | (zOrder << 4u)}, {0x1c4, 1}
        };
        const auto enabled = AgcDriver::Graphics::DecodePixelStageInfo(context, {});
        Require(enabled.depthExportEnable && !enabled.earlyZ, "depth export did not suppress early fragment tests");
        RecompileRequest request{{ShaderStage::Fragment, snapshot->codeAddress, snapshot->code, 0, snapshot->header}, {32, 0, {}, {}, enabled, {}, {}}, device.Target(), {0, 0, 0, 128}};
        for (const auto capacity : {128u, 0u}) {
            request.layout.pushConstantSizeBytes = capacity;
            snapshot->prepared->entries.push_back({0, PrepareShader(request)});
        }
        context[0x203] &= ~1u;
        const auto disabled = AgcDriver::Graphics::DecodePixelStageInfo(context, {});
        Require(!disabled.depthExportEnable && disabled.earlyZ == (zOrder == 1u), "disabling depth export did not recompute early fragment tests");
        request.context.pixel = disabled;
        request.layout = {0, 0, 0, 128};
        bool rejected = false;
        try {
            static_cast<void>(SourceHandleFor(*snapshot, 0, request));
        } catch (const std::exception& error) {
            Require(std::string_view(error.what()).find("prepared shader artifact is missing") != std::string_view::npos, "disabled depth ABI failed for an unrelated reason");
            rejected = true;
        }
        Require(rejected, "enabled depth artifact accepted a different static depth ABI");
        for (const std::uint32_t drawZOrder : {0u, 1u}) {
            context[0x203] = (context[0x203] & ~0x30u) | (drawZOrder << 4u);
            request.context.pixel = AgcDriver::Graphics::DecodePixelStageInfo(context, {});
            for (const auto capacity : {128u, 0u}) {
                request.layout.pushConstantSizeBytes = capacity;
                snapshot->prepared->entries.push_back({0, PrepareShader(request)});
            }
        }
        for (const std::uint32_t drawZOrder : {0u, 1u}) {
            for (const bool exportDepth : {true, false}) {
                context[0x203] = (context[0x203] & ~0x31u) | (drawZOrder << 4u) | static_cast<std::uint32_t>(exportDepth);
                request.context.pixel = AgcDriver::Graphics::DecodePixelStageInfo(context, {});
                for (const auto layout : {BindingLayout{0, 0, 0, 128}, BindingLayout{0, 0, 20, 108}, BindingLayout{0, 0, 0, 0}}) {
                    request.layout = layout;
                    const auto handle = SourceHandleFor(*snapshot, 0, request);
                    const auto& words = GetPreparedArtifact(*handle).spirv.Words();
                    Require(HasExecutionMode(words, spv::ExecutionModeDepthReplacing) == exportDepth, "prepared pixel retained the wrong depth export execution mode");
                    Require(HasExecutionMode(words, spv::ExecutionModeEarlyFragmentTests) == (!exportDepth && drawZOrder == 1u), "prepared pixel retained the wrong early depth execution mode");
                    const auto invocation = InvocationFor(*snapshot, 0, request);
                    Require(invocation.Request().layout.pushConstantOffsetBytes == layout.pushConstantOffsetBytes, "prepared pixel lost its actual preceding-stage push offset");
                }
            }
        }
        for (const bool changeInterpolator : {true, false}) {
            auto invalid = context;
            invalid[0x203] = (invalid[0x203] & ~0x31u) | (zOrder << 4u);
            if (changeInterpolator) invalid[0x191] = 0;
            else invalid[0x203] |= 0x40u;
            request.context.pixel = AgcDriver::Graphics::DecodePixelStageInfo(invalid, {});
            request.layout = {0, 0, 0, 128};
            rejected = false;
            try {
                static_cast<void>(SourceHandleFor(*snapshot, 0, request));
            } catch (const std::exception& error) {
                Require(std::string_view(error.what()).find("prepared shader artifact is missing") != std::string_view::npos, "unprepared pixel ABI failed for an unrelated reason");
                rejected = true;
            }
            Require(rejected, "preparing depth variants relaxed an unrelated static pixel ABI");
        }
    }
    std::cout << "prepared depth export, early fragment tests, push offsets and strict pixel ABI passed\n";
}

void CheckRegisteredDepthExport(std::uint32_t zOrder, bool registeredDepth = true) {
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(Pixels.data(), Pixels.size(), true, true, true);
        mutation.Add(Depth.data(), Depth.size(), true, true, true);
        mutation.Add(Stencil.data(), Stencil.size(), true, true, true);
    }
    auto queue = Queue();
    queue.context[0x200] = 0x76;
    queue.context[0x011] = 0;
    Header<3, 1> vertex;
    vertex.Initialize(2, VertexCode.data(), sizeof(VertexCode));
    vertex.registers = {{{0xc8, queue.shader.at(0xc8)}, {0xc9, queue.shader.at(0xc9)}, {0x8b, queue.shader.at(0x8b)}}};
    vertex.context = {{{0x2d5, 0x2000}}};
    Header<3, 6> color;
    color.Initialize(1, PixelCode.data(), sizeof(PixelCode));
    color.registers = {{{0x8, queue.shader.at(0x8)}, {0x9, queue.shader.at(0x9)}, {0xb, queue.shader.at(0xb)}}};
    color.context = {{{0x1b3, 2}, {0x1b4, 2}, {0x1b6, 0x8000}, {0x1c5, 9}, {0x203, 0x800}, {0x1c4, 0}}};
    auto exportQueue = queue;
    Bind(exportQueue, 0x8, reinterpret_cast<std::uintptr_t>(PixelDepthCode.data()));
    exportQueue.context[0x1b6] = 0x8001;
    exportQueue.context[0x191] = 32;
    exportQueue.context[0x1c4] = 1;
    exportQueue.context[0x203] = 0x800u | (zOrder << 4u) | static_cast<std::uint32_t>(registeredDepth);
    Header<3, 7> pixel;
    pixel.Initialize(1, PixelDepthCode.data(), sizeof(PixelDepthCode));
    pixel.registers = {{{0x8, exportQueue.shader.at(0x8)}, {0x9, exportQueue.shader.at(0x9)}, {0xb, exportQueue.shader.at(0xb)}}};
    pixel.context = {{{0x1b3, 2}, {0x1b4, 2}, {0x1b6, 0x8001}, {0x1c5, 9}, {0x203, exportQueue.context.at(0x203)}, {0x1c4, 1}, {0x191, 0}}};
    AgcDriverRegisterShader_nid_postfix(&vertex.shader);
    AgcDriverRegisterShader_nid_postfix(&color.shader);
    AgcDriverRegisterShader_nid_postfix(&pixel.shader);
    const std::array<const Shader*, 1> stages{&vertex.shader};
    const std::array<ShaderRegister, 1> primitive{{{0x242, 4}}};
    AgcDriverResolveGraphicsStagesAbi_nid_postfix(stages, {}, primitive);
    const std::array<ShaderRegister, 1> interpolant{{{0x191, 32}}};
    AgcDriverResolveShaderAbi_nid_postfix(&pixel.shader, interpolant, {});
    AgcDriverResolveShaderAbi_nid_postfix(&pixel.shader, interpolant, {});
    const auto verifyDepth = [&](float reference, VkCompareOp compare, bool expectedPass) {
        for (auto& position : Vertices) position[2] = reference;
        Pixels.fill(std::byte{0x40});
        auto verify = queue;
        verify.context[0x200] = (static_cast<std::uint32_t>(compare) << 4u) | 2u;
        Submit(verify);
        const auto expected = expectedPass ? std::byte{255} : std::byte{0x40};
        Require(std::all_of(Pixels.begin(), Pixels.begin() + Width * Height * 4u, [&](std::byte value) { return value == expected; }), "registered depth variant produced the wrong stored depth");
        Require(std::all_of(Pixels.begin() + Width * Height * 4u, Pixels.end(), [](std::byte value) { return value == std::byte{0x40}; }), "depth verification draw changed color padding");
    };
    for (const std::uint32_t drawZOrder : {0u, 1u}) {
        for (const bool exportDepth : {true, false}) {
            if (exportDepth && !registeredDepth) continue;
            for (auto& position : Vertices) position[2] = 0.25f;
            Submit(queue);
            verifyDepth(0.25f, VK_COMPARE_OP_EQUAL, true);
            for (auto& position : Vertices) position[2] = 0.25f;
            auto draw = exportQueue;
            draw.context[0x203] = (draw.context[0x203] & ~0x31u) | (drawZOrder << 4u) | static_cast<std::uint32_t>(exportDepth);
            const auto pixelInfo = AgcDriver::Graphics::DecodePixelStageInfo(draw.context, {});
            Require(pixelInfo.depthExportEnable == exportDepth && pixelInfo.earlyZ == (!exportDepth && drawZOrder == 1u), "registered depth fixture decoded the wrong pixel controls");
            Submit(draw);
            verifyDepth(exportDepth ? 0.75f : 0.25f, VK_COMPARE_OP_EQUAL, true);
            verifyDepth(0.5f, VK_COMPARE_OP_LESS, exportDepth);
            Require(pixel.context[4].value == (0x800u | (zOrder << 4u) | static_cast<std::uint32_t>(registeredDepth)) && pixel.context[6].value == 0, "preparing a depth variant changed the registered shader header");
        }
    }
    {
        GuestAllocations::Mutation mutation;
        mutation.Remove(Stencil.data());
        mutation.Remove(Depth.data());
        mutation.Remove(Pixels.data());
    }
    std::cout << "registered enabled/disabled depth export and resolved interpolants passed: zOrder=" << zOrder << " depth=" << registeredDepth << '\n';
}

}

int main(int argc, char** argv) {
    try {
        Require(argc == 1 || (argc == 2 && (std::string_view(argv[1]) == "--abi" || std::string_view(argv[1]) == "--draw" || std::string_view(argv[1]) == "--rect" || std::string_view(argv[1]) == "--rect-pair" || std::string_view(argv[1]) == "--depth-only" || std::string_view(argv[1]) == "--depth-only-rect" || std::string_view(argv[1]) == "--depth-only-zero" || std::string_view(argv[1]) == "--depth-only-rect-zero" || std::string_view(argv[1]) == "--depth-variants")), "invalid test arguments");
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (argc == 1 || std::string_view(argv[1]) == "--abi") CheckDecodedAbi(*device);
        if (argc == 1 || std::string_view(argv[1]) == "--depth-variants") CheckDepthExportAbi(*device);
        device.reset();
        if (argc == 1 || std::string_view(argv[1]) == "--draw") CheckRegisteredDraw();
        if (argc == 2 && std::string_view(argv[1]) == "--rect") CheckRegisteredDraw(true);
        if (argc == 2 && std::string_view(argv[1]) == "--rect-pair") CheckRegisteredDraw(true, true);
        if (argc == 2 && std::string_view(argv[1]) == "--depth-only") CheckDepthOnly(4u, false);
        if (argc == 2 && std::string_view(argv[1]) == "--depth-only-rect") CheckDepthOnly(17u, false);
        if (argc == 2 && std::string_view(argv[1]) == "--depth-only-zero") CheckDepthOnly(4u, true);
        if (argc == 2 && std::string_view(argv[1]) == "--depth-only-rect-zero") CheckDepthOnly(17u, true);
        if (argc == 1 || std::string_view(argv[1]) == "--depth-variants") {
            CheckRegisteredDepthExport(0u);
            CheckRegisteredDepthExport(1u);
            CheckRegisteredDepthExport(0u, false);
            CheckRegisteredDepthExport(1u, false);
        }
        if (argc == 1 || std::string_view(argv[1]) != "--abi") AgcDriverShutdown_nid_postfix();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
