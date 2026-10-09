#include "VulkanTestDevice.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderPreparation.hpp"
#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace ShaderRecompiler;
using namespace AgcDriver::DriverDetail;

alignas(256) constexpr std::array<std::uint32_t, 4> ExternalCode{0xbe8e0304u, 0xbe8f0305u, 0xbe8e210eu, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 5> StoreCode{0x7e0202ffu, 0x13579bdfu, 0xe0700000u, 0x80000100u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 3> InvalidCode{0xbe8e2104u, 0xffffffffu, 0xbf810000u};
alignas(256) std::array<std::uint32_t, 64> Output{};

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename TAction>
std::string ExpectFailure(TAction action, const char* expected) {
    try {
        action();
    } catch (const std::exception& error) {
        Require(std::string(error.what()).find(expected) != std::string::npos, error.what());
        return error.what();
    }
    throw std::runtime_error("expected a deferred-compute failure");
}

struct Header {
    Shader shader{};
    std::array<ShaderRegister, 7> registers{};
    ShaderSpecialRegs specials{};

    Header(std::span<const std::uint32_t> code, std::uint32_t users) {
        const auto address = reinterpret_cast<std::uintptr_t>(code.data());
        shader.file_header = 0x34333231u;
        shader.version = 0x18;
        shader.header_size = sizeof(Header);
        shader.shader_size = code.size_bytes();
        shader.code = code.data();
        shader.sh_registers = registers.data();
        shader.num_sh_registers = registers.size();
        shader.specials = &specials;
        specials.dispatch_modifier = 0x8000;
        registers = {{{0x20c, static_cast<std::uint32_t>(address >> 8u)}, {0x20d, static_cast<std::uint32_t>(address >> 40u)},
            {0x207, 1}, {0x208, 1}, {0x209, 1}, {0x212, 0}, {0x213, users << 1u}}};
    }
};

void StateAndInvocation(AgcDriver::VulkanDevice& device) {
    ShaderSnapshot snapshot{0x10000, 0x20000, 0, {ExternalCode.begin(), ExternalCode.end()}, {}};
    snapshot.header.resize(sizeof(Shader));
    std::array<std::uint32_t, 15> users{};
    users[4] = 0x12345678;
    users[5] = 0x42;
    RecompileRequest request{{ShaderStage::Compute, snapshot.codeAddress, snapshot.code, snapshot.headerAddress, snapshot.header},
        {32, 0, users, ShaderComputeStageInfo{{1, 1, 1}, 0, {false, false, false}, false, 1, {}}, {}, {}, {}},
        device.ComputeTarget(32), {0, 0, 0, 128}};
    std::vector<std::uint64_t> key;
    BuildPreparedShaderKey(request, key);
    const PreparedShaderState::DeferredComputeEntry deferred{0, key, {8, 14, 14}};
    {
        ShaderPreparationTransaction transaction;
        transaction.Edit(snapshot).deferredCompute.push_back(deferred);
    }
    Require(snapshot.prepared->deferredCompute.empty() && snapshot.prepared->entries.empty(), "aborted transaction published a deferred compute shader");
    {
        ShaderPreparationTransaction transaction;
        transaction.Edit(snapshot).deferredCompute.push_back(deferred);
        transaction.Commit();
    }
    Require(snapshot.prepared->deferredCompute.size() == 1 && snapshot.prepared->entries.empty(), "deferred compute was published as a ready artifact");
    auto replacedCode = ExternalCode;
    replacedCode[0] = 0xbe8e0306u;
    auto replacedRequest = request;
    replacedRequest.shader.code = replacedCode;
    ExpectFailure([&] { static_cast<void>(SourceHandleFor(snapshot, 0, replacedRequest)); }, "artifact is missing");
    Require(snapshot.prepared->entries.empty() && snapshot.prepared->deferredCompute.size() == 1, "different code with the same static ABI changed the deferred snapshot");
    const auto failure = ExpectFailure([&] { static_cast<void>(InvocationFor(snapshot, 0, request)); }, "program counter 0x00000008 is not statically resolvable");
    Require(failure.find("RecompileRequest:") != std::string::npos, "first used deferred shader lost its full runtime request diagnostic");
    ExpectFailure([&] { static_cast<void>(SourceHandleFor(snapshot, 0, request)); }, "program counter 0x00000008 is not statically resolvable");
    Require(snapshot.prepared->entries.empty() && snapshot.prepared->deferredCompute.size() == 1, "failed deferred invocation published a ready artifact or lost its requirement");
    request.context.compute->partialThreads = {1, 1, 1};
    ExpectFailure([&] { static_cast<void>(InvocationFor(snapshot, 0, request)); }, "artifact is missing");
    BuildPreparedShaderKey(request, key);
    snapshot.prepared->deferredCompute.push_back({0, key, {8, 14, 14}});
    ExpectFailure([&] { static_cast<void>(InvocationFor(snapshot, 0, request)); }, "program counter 0x00000008 is not statically resolvable");
    request.context.waveSize = 64;
    ExpectFailure([&] { static_cast<void>(InvocationFor(snapshot, 0, request)); }, "artifact is missing");
    request.context.waveSize = 32;
    request.context.floatMode = ShaderFloatMode{192, true, false, false};
    ExpectFailure([&] { static_cast<void>(InvocationFor(snapshot, 0, request)); }, "artifact is missing");
    request.context.floatMode = {};
    snapshot.type = 1;
    ExpectFailure([&] { static_cast<void>(InvocationFor(snapshot, 0, request)); }, "artifact is missing");
    Require(snapshot.prepared->entries.empty() && snapshot.prepared->deferredCompute.size() == 2, "static ABI mismatch altered deferred shader state");
}

std::vector<std::uint32_t> RegisterCommands(const Header& header, std::span<const std::uint32_t> users) {
    std::vector<std::uint32_t> commands;
    for (const auto reg : header.registers) commands.insert(commands.end(), {0xc0017600u, reg.offset, reg.value});
    for (std::uint32_t index = 0; index < users.size(); ++index) commands.insert(commands.end(), {0xc0017600u, 0x240u + index, users[index]});
    commands.insert(commands.end(), {0xc0031500u, 1, 1, 1, 0x8041});
    return commands;
}

void RegistrationAndDispatch() {
    Header external(ExternalCode, 15);
    AgcDriverRegisterShader_nid_postfix(&external.shader);
    for (unsigned iteration = 0; iteration < 4; ++iteration) {
        AgcDriverRegisterShader_nid_postfix(&external.shader);
        AgcDriverResolveShaderAbi_nid_postfix(&external.shader, {}, {});
    }
    external.registers[2].value = 0;
    ExpectFailure([&] { AgcDriverRegisterShader_nid_postfix(&external.shader); }, "must be nonzero");
    external.registers[2].value = 1;
    external.registers[1].value |= 0x100;
    ExpectFailure([&] { AgcDriverRegisterShader_nid_postfix(&external.shader); }, "invalid registered program address");
    external.registers[1].value &= 0xff;
    ++external.registers[0].value;
    ExpectFailure([&] { AgcDriverRegisterShader_nid_postfix(&external.shader); }, "entry point is outside shader code");
    --external.registers[0].value;
    AgcDriverRegisterShader_nid_postfix(&external.shader);
    Header invalid(InvalidCode, 15);
    ExpectFailure([&] { AgcDriverRegisterShader_nid_postfix(&invalid.shader); }, "");
    Header normal(StoreCode, 4);
    AgcDriverRegisterShader_nid_postfix(&normal.shader);
    AgcDriverResolveShaderAbi_nid_postfix(&normal.shader, {}, {});
    const auto outputAddress = reinterpret_cast<std::uintptr_t>(Output.data());
    const std::array<std::uint32_t, 4> descriptor{static_cast<std::uint32_t>(outputAddress), static_cast<std::uint32_t>((outputAddress >> 32u) & 0xffffu), sizeof(Output), 0x31016facu};
    Output.fill(0xdeadbeefu);
    auto commands = RegisterCommands(normal, descriptor);
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    sceAgcDriverSubmitAcb(0x20, &packet);
    AgcDriverWaitIdle_nid_postfix();
    AgcDriver::GuestMemory::FlushGpuWrites(outputAddress, sizeof(Output));
    Require(Output[0] == 0x13579bdfu, "unused deferred shader blocked normal compute output");
    for (std::size_t index = 1; index < Output.size(); ++index) Require(Output[index] == 0xdeadbeefu, "normal compute wrote outside its one-thread output");
    std::array<std::uint32_t, 15> users{};
    users[4] = 0x12345678;
    users[5] = 0x42;
    commands = RegisterCommands(external, users);
    std::uint32_t afterDispatch = 0;
    const auto afterAddress = reinterpret_cast<std::uintptr_t>(&afterDispatch);
    commands.insert(commands.end(), {0xc0033700u, 0x00100200u, static_cast<std::uint32_t>(afterAddress), static_cast<std::uint32_t>(afterAddress >> 32u), 7});
    packet = {commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    sceAgcDriverSubmitAcb(0x20, &packet);
    ExpectFailure([] { AgcDriverWaitIdle_nid_postfix(); }, "program counter 0x00000008 is not statically resolvable");
    ExpectFailure([] { AgcDriverShutdown_nid_postfix(); }, "program counter 0x00000008 is not statically resolvable");
    Require(afterDispatch == 0 && Output[0] == 0x13579bdfu, "unresolved dispatched shader was skipped or executed subsequent writes");
}

}

int main() {
    try {
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        StateAndInvocation(*device);
        device.reset();
        RegistrationAndDispatch();
        std::cout << "unused compute deferral and strict first dispatch tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try { AgcDriverShutdown_nid_postfix(); } catch (const std::exception&) {}
        return 1;
    }
}
