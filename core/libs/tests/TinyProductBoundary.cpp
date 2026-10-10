#include "IntermediateRepresentation/IrBuilder.hpp"
#include "Optimization/ConstantFolder.hpp"
#include "SpirvBackend/SpirvEmitterInstructions.hpp"
#include "SpirvBackend/SpirvOptimizer.hpp"
#include "Translation/TranslationContext.hpp"
#include "RdnaDecoder/RdnaVectorOpDecoder.hpp"
#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ShaderRecompiler;

namespace {

void Require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error("tiny product boundary: " + message);
}

std::uint32_t Bits(std::uint32_t exponent, std::uint32_t fraction, bool negative = false) {
    return (exponent << 23u) | fraction | (negative ? 0x80000000u : 0u);
}

IrValue& OriginalPredicate(IrBuilder& ir, IrValue& a, IrValue& b) {
    const auto exponent = [&](IrValue& bits) -> IrValue& {
        return ir.Emit(IrOpcode::BitFieldUExtract, IrType::U32, {&bits, &ir.Constant(23u), &ir.Constant(8u)});
    };
    const auto significand = [&](IrValue& bits) -> IrValue& {
        return ir.BitwiseOr(ir.BitwiseAnd(bits, ir.Constant(0x007fffffu)), ir.Constant(0x00800000u));
    };
    auto& ea = exponent(a);
    auto& eb = exponent(b);
    const auto normal = [&](IrValue& value) -> IrValue& {
        return ir.LogicalAnd(ir.INotEqual(value, ir.Constant(0u)), ir.INotEqual(value, ir.Constant(255u)));
    };
    auto& high = ir.Emit(IrOpcode::UMulHi, IrType::U32, {&significand(a), &significand(b)});
    auto& carry = ir.Select(ir.UGreaterThan(high, ir.Constant(0x7fffu)), ir.Constant(1u), ir.Constant(0u));
    return ir.LogicalAnd(ir.LogicalAnd(normal(ea), normal(eb)), ir.ULessThan(ir.IAdd(ir.IAdd(ea, eb), carry), ir.Constant(128u)));
}

void CheckFold(std::uint32_t a, std::uint32_t b, int expected = -1) {
    IrProgram program;
    auto& block = program.CreateBlock();
    program.SetEntryBlock(block);
    program.BlockOrder().push_back(&block);
    IrBuilder ir(program);
    ir.SetInsertionPoint(block);
    auto& original = OriginalPredicate(ir, ir.Constant(a), ir.Constant(b));
    auto& replacement = ir.Emit(IrOpcode::F32ProductIsTiny, IrType::U1, {&ir.Constant(a), &ir.Constant(b)});
    auto& keepOriginal = ir.Emit(IrOpcode::Reference, IrType::Void, {&original});
    auto& keepReplacement = ir.Emit(IrOpcode::Reference, IrType::Void, {&replacement});
    ConstantFolder{}.Fold(program);
    const auto* oldValue = keepOriginal.Argument(0)->Resolve();
    const auto* newValue = keepReplacement.Argument(0)->Resolve();
    Require(oldValue->HasImmediate() && newValue->HasImmediate(), "a constant predicate did not fold");
    Require(oldValue->ImmediateBool() == newValue->ImmediateBool(), "replacement differs from the original expanded IR");
    if (expected >= 0) Require(newValue->ImmediateBool() == (expected != 0), "pinned boundary or special-value result changed");
}

void CheckBoundaries() {
    CheckFold(Bits(62u, 0x7fffffu), Bits(64u, 0x7fffffu), 1);
    CheckFold(Bits(63u, 0u), Bits(64u, 0u), 1);
    CheckFold(Bits(63u, 0x7fffffu), Bits(64u, 0x7fffffu), 0);
    CheckFold(Bits(64u, 0u), Bits(64u, 0u), 0);
    CheckFold(0x3f7ffffeu, 0x00800001u, 1);
    CheckFold(0x3f7fffffu, 0x00800001u, 0);
    CheckFold(0xbf7ffffeu, 0x00800001u, 1);
    CheckFold(0x3f800000u, 0x00800000u, 0);
    for (const auto exceptional : {0u, 0x80000000u, 1u, 0x007fffffu, 0x807fffffu, 0x7f800000u, 0xff800000u, 0x7f800001u, 0x7fc12345u}) {
        CheckFold(exceptional, 0x00800001u, 0);
        CheckFold(0x00800001u, exceptional, 0);
    }
    constexpr std::array<std::uint32_t, 10> exponents{0u, 1u, 62u, 63u, 64u, 65u, 126u, 127u, 254u, 255u};
    constexpr std::array<std::uint32_t, 4> fractions{0u, 1u, 0x400000u, 0x7fffffu};
    for (const auto ea : exponents) {
        for (const auto eb : exponents) {
            for (const auto ma : fractions) {
                for (const auto mb : fractions) CheckFold(Bits(ea, ma), Bits(eb, mb, true));
            }
        }
    }
}

void CheckTranslation(const std::vector<std::uint32_t>& words, RdnaOpcode expected, std::uint32_t mode, bool needsTiny) {
    const auto instruction = DecodeRdnaVectorOp(std::span<const std::uint32_t>(words), 0u);
    Require(instruction.op == expected, "fixture decoded to another operation");
    IrProgram program;
    auto& block = program.CreateBlock();
    program.SetEntryBlock(block);
    TranslationContext context(program, block, 256u);
    context.SetFloatMode(ShaderFloatMode{mode, true, false, false});
    context.TranslateInstruction(instruction);
    std::uint32_t tiny = 0u;
    std::uint32_t high = 0u;
    for (const auto* value : block.Instructions()) {
        if (value->Opcode() == IrOpcode::F32ProductIsTiny) ++tiny;
        if (value->Opcode() == IrOpcode::UMulHi) ++high;
    }
    Require(tiny == (needsTiny ? 1u : 0u), "wrong number of whole tiny-product predicates");
    Require(high == 0u, "translation emitted an eager carry multiply");
}

std::vector<std::uint32_t> EmitPredicate(bool loopHeader) {
    IrProgram program;
    program.Resources().stage = IrShaderStage::Compute;
    program.SetWaveSize(32u);
    auto& block = program.CreateBlock();
    program.SetEntryBlock(block);
    program.BlockOrder().push_back(&block);
    program.Metadata().blockInfo.push_back(BlockInfo{});
    program.Metadata().blockInfo.back().terminator.loopHeader = loopHeader;
    SpirvEmitterState state(program, ShaderStageInputInfo{});
    auto& module = state.module;
    module.EmitCapability(spv::CapabilityShader);
    module.AddMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
    const auto function = module.AllocateId();
    module.EmitEntryPoint(spv::ExecutionModelGLCompute, function, "main", {});
    module.AddExecutionMode(function, spv::ExecutionModeLocalSize, 32u, 1u, 1u);
    const auto typeVoid = module.Type(spv::OpTypeVoid);
    const auto typeFunction = module.Type(spv::OpTypeFunction, typeVoid);
    module.AddFunction(spv::OpFunction, typeVoid, function, spv::FunctionControlMaskNone, typeFunction);
    EmitLabel(state, module.AllocateId());
    const auto header = module.AllocateId();
    const auto continuing = module.AllocateId();
    const auto merge = module.AllocateId();
    if (loopHeader) {
        module.AddFunction(spv::OpBranch, header);
        EmitLabel(state, header);
    }
    state.currentBlock = &block;
    const auto predicate = EmitF32ProductIsTiny(state, ConstantU32(state, Bits(63u, 0x7fffffu)), ConstantU32(state, Bits(64u, 1u)));
    if (loopHeader) {
        module.AddFunction(spv::OpLoopMerge, merge, continuing, spv::LoopControlMaskNone);
        module.AddFunction(spv::OpBranchConditional, predicate, continuing, merge);
        EmitLabel(state, continuing);
        module.AddFunction(spv::OpBranch, header);
        EmitLabel(state, merge);
    }
    module.AddFunction(spv::OpReturn);
    module.AddFunction(spv::OpFunctionEnd);
    const auto words = module.Finalize();
    const auto validated = ValidateAndOptimizeSpirv(words, 0x00401000u, 0x00010300u, false, false);
    Require(validated == words, "validation changed an unoptimized predicate module");
    return words;
}

void CheckSpirv(bool loopHeader) {
    const auto words = EmitPredicate(loopHeader);
    std::uint32_t label = 0u;
    std::uint32_t guardedLabel = 0u;
    std::uint32_t selections = 0u;
    std::uint32_t multiplies = 0u;
    for (std::size_t offset = 5u; offset < words.size();) {
        const auto count = words[offset] >> 16u;
        const auto opcode = words[offset] & 0xffffu;
        Require(count > 0u && offset + count <= words.size(), "invalid SPIR-V word count");
        if (opcode == spv::OpLabel) label = words[offset + 1u];
        if (opcode == spv::OpSelectionMerge) ++selections;
        if (!loopHeader && opcode == spv::OpBranchConditional && guardedLabel == 0u) guardedLabel = words[offset + 2u];
        if (opcode == spv::OpUMulExtended) {
            ++multiplies;
            if (!loopHeader) Require(guardedLabel != 0u && label == guardedLabel, "carry multiply is outside the conditional true arm");
        }
        offset += count;
    }
    Require(multiplies == 1u, "predicate has the wrong number of carry multiplies");
    Require(selections == (loopHeader ? 0u : 1u), "loop-header fallback or ordinary guard structure changed");
}

}

int main() {
    try {
        CheckBoundaries();
        CheckTranslation({0x100a0f06u}, RdnaOpcode::VMulF32, 0xc0u, true);
        CheckTranslation({0x100a0f06u}, RdnaOpcode::VMulF32, 0xf0u, false);
        CheckTranslation({0xd54b0017u, 0x04164104u}, RdnaOpcode::VFmaF32, 0xc0u, true);
        CheckSpirv(false);
        CheckSpirv(true);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
