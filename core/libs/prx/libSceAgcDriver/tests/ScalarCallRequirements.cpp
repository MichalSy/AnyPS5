#include "ControlFlow/GraphBuilder.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "RdnaDecoder/RdnaInstructionDecoder.hpp"
#include "RdnaDecoder/RdnaScalarOpDecoder.hpp"
#include <array>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace ShaderRecompiler;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

RdnaInstruction Sop1(std::uint32_t pc, std::uint32_t destination, std::uint32_t opcode, std::uint32_t source) {
    const std::array<std::uint32_t, 2> words{0xbe800000u | (destination << 16u) | (opcode << 8u) | source, 0};
    return DecodeRdnaSop1(pc, words, 0);
}

RdnaInstruction Sop2Literal(std::uint32_t pc, std::uint32_t destination, std::uint32_t opcode, std::uint32_t source, std::uint32_t value) {
    const std::array<std::uint32_t, 2> words{0x80000000u | (opcode << 23u) | (destination << 16u) | 0xff00u | source, value};
    return DecodeRdnaSop2(pc, words, 0);
}

RdnaInstruction Sopp(std::uint32_t pc, std::uint32_t opcode) {
    const std::array<std::uint32_t, 1> words{0xbf800000u | (opcode << 16u)};
    return DecodeRdnaSopp(pc, words, 0);
}

RdnaProgram Program(std::initializer_list<RdnaInstruction> instructions) {
    RdnaProgram result;
    result.instructions.assign(instructions);
    return result;
}

template<typename TAction>
void RequireStrictFailure(TAction action, const char* expected) {
    try {
        action();
    } catch (const UnresolvedScalarCall&) {
        throw std::runtime_error("an unrelated static error became a deferred scalar call");
    } catch (const std::invalid_argument& error) {
        Require(std::string(error.what()).find(expected) != std::string::npos, error.what());
        return;
    }
    throw std::runtime_error("expected a strict static failure");
}

void Requirements() {
    const std::array<std::uint32_t, 4> code{0xbe8e0304u, 0xbe8f0305u, 0xbe8e210eu, 0xbf810000u};
    const auto decoded = RdnaInstructionDecoder{}.Decode(code);
    const SwappcInfo info{false, 0, 15};
    try {
        static_cast<void>(GraphBuilder{}.Build(decoded, &info));
    } catch (const UnresolvedScalarCall& error) {
        const auto& requirement = error.Requirement();
        Require(requirement.programCounter == 8 && requirement.targetRegister == 14 && requirement.linkRegister == 14,
            "unresolved scalar call lost its aliased SGPR target/link or program counter");
        Require(std::string(error.what()) == "computed/data-dependent s_swappc_b64 call target at program counter 0x00000008 is not statically resolvable",
            "unresolved scalar call changed its concrete diagnostic");
        return;
    }
    throw std::runtime_error("data-dependent scalar call was accepted");
}

void StaticCalls() {
    const auto program = Program({
        Sop1(0x00, 4, 0x1f, 0),
        Sop2Literal(0x04, 4, 0x00, 4, 0x18),
        Sop2Literal(0x0c, 5, 0x04, 5, 0),
        Sop1(0x14, 8, 0x21, 4),
        Sopp(0x18, 0x01),
        Sopp(0x1c, 0x00),
        Sop1(0x20, 0, 0x20, 8),
    });
    const auto graph = GraphBuilder{}.Build(program);
    Require(graph.FindBlockByProgramCounter(0).terminator.trueBlock == graph.FindBlockByProgramCounter(0x1c).id,
        "resolved scalar call lost its callee edge");
    Require(graph.FindBlockByProgramCounter(0x1c).terminator.trueBlock == graph.FindBlockByProgramCounter(0x18).id,
        "resolved scalar call lost its return edge");
    const SwappcInfo fetch{true, 16, 8};
    const auto fetchGraph = GraphBuilder{}.Build(Program({Sopp(0, 0), Sop1(4, 2, 0x21, 16), Sopp(8, 1)}), &fetch);
    Require(fetchGraph.hasFetchCall && fetchGraph.fetchCallProgramCounter == 4, "existing fetch-call ABI became deferred");
    Require(GraphBuilder{}.Build(Program({Sopp(0, 1)})).blocks.size() == 1, "normal compute program changed");
}

void StrictErrors() {
    RequireStrictFailure([] {
        static_cast<void>(GraphBuilder{}.Build(Program({Sop1(0, 15, 0x21, 4), Sopp(4, 1)})));
    }, "unsupported scalar call link register");
    RequireStrictFailure([] {
        static_cast<void>(GraphBuilder{}.Build(Program({Sop1(0, 14, 0x21, 5), Sopp(4, 1)})));
    }, "not statically resolvable");
    RequireStrictFailure([] {
        static_cast<void>(GraphBuilder{}.Build(Program({Sop1(0, 14, 0x21, 106), Sopp(4, 1)})));
    }, "not statically resolvable");
    RequireStrictFailure([] {
        static_cast<void>(GraphBuilder{}.Build(Program({
            Sop1(0x00, 4, 0x1f, 0),
            Sop2Literal(0x04, 4, 0x00, 4, 0x100),
            Sop2Literal(0x0c, 5, 0x04, 5, 0),
            Sop1(0x14, 8, 0x21, 4),
            Sopp(0x18, 1),
        })));
    }, "not statically resolvable");
    RequireStrictFailure([] {
        static_cast<void>(GraphBuilder{}.Build(Program({Sop1(0, 4, 0x1f, 0), Sop1(4, 8, 0x21, 4), Sopp(8, 1)})));
    }, "no paired s_setpc_b64 return");
}

void InterfaceKeys() {
    const std::array<std::uint32_t, 1> code{0xbf810000u};
    std::array<std::uint32_t, 15> users{};
    RecompileRequest request{};
    request.shader = {ShaderStage::Compute, 0x10000, code, 0, {}};
    request.context.waveSize = 64;
    request.context.userData = users;
    request.context.compute = ShaderComputeStageInfo{{64, 1, 1}, 0, {false, false, false}, false, 1, {}};
    request.target.subgroupSize = 32;
    std::vector<std::uint64_t> normal, partial, changed;
    BuildPreparedShaderKey(request, normal);
    request.context.compute->partialThreads = {1, 1, 1};
    BuildPreparedShaderKey(request, partial);
    Require(normal != partial, "deferred normal and partial groups have the same static ABI");
    request.context.compute->partialThreads = {63, 1, 1};
    BuildPreparedShaderKey(request, changed);
    Require(changed == partial, "partial dispatch dimensions require an unregistered static variant");
    request.context.compute->partialThreads = {};
    users[4] = 0x12345678u;
    users[5] = 0x00000042u;
    BuildPreparedShaderKey(request, changed);
    Require(changed == normal, "runtime scalar targets changed a static ABI marker");
    request.context.waveSize = 32;
    BuildPreparedShaderKey(request, changed);
    Require(changed != normal, "wave size is missing from the deferred static ABI");
    request.context.waveSize = 64;
    request.context.floatMode = ShaderFloatMode{192, true, false, false};
    BuildPreparedShaderKey(request, changed);
    Require(changed != normal, "float mode is missing from the deferred static ABI");
    request.context.floatMode = {};
    request.context.userData = std::span(users).first(14);
    BuildPreparedShaderKey(request, changed);
    Require(changed != normal, "user SGPR count is missing from the deferred static ABI");
}

}

int main() {
    try {
        Requirements();
        StaticCalls();
        StrictErrors();
        InterfaceKeys();
        std::cout << "typed scalar-call requirements and static ABI tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
