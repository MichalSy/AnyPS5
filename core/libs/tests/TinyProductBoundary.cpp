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
#include <unordered_map>

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

struct ContextPredicateModule {
    std::vector<std::uint32_t> words;
    std::uint32_t result;
};

ContextPredicateModule EmitContextPredicate(std::uint32_t a, std::uint32_t b, std::uint32_t constantMask, bool loopHeader = false) {
    IrProgram program;
    program.Resources().stage = IrShaderStage::Compute;
    program.SetWaveSize(32u);
    auto& block = program.CreateBlock();
    program.SetEntryBlock(block);
    program.BlockOrder().push_back(&block);
    program.Metadata().blockInfo.push_back(BlockInfo{});
    program.Metadata().blockInfo.back().terminator.loopHeader = loopHeader;
    IrBuilder ir(program);
    ir.SetInsertionPoint(block);
    SpirvEmitterState state(program, ShaderStageInputInfo{});
    SpirvValueEmitContext ctx(state);
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
    const auto argument = [&](std::uint32_t bits, std::uint32_t index) -> IrValue& {
        if ((constantMask & (1u << index)) != 0u) {
            auto& value = program.CreateValue(IrOpcode::Identity, IrType::U32);
            value.AddArgument(&ir.Constant(bits));
            block.AppendInstruction(&value);
            return value;
        }
        auto& value = ir.Emit(IrOpcode::UndefU32, IrType::U32, {});
        const auto copied = module.AllocateId();
        module.AddFunction(spv::OpCopyObject, TypeU32(state), copied, ConstantU32(state, bits));
        ctx.Define(value, copied);
        return value;
    };
    auto& lhs = argument(a, 0u);
    auto& rhs = argument(b, 1u);
    auto& inst = ir.Emit(IrOpcode::F32ProductIsTiny, IrType::U1, {&lhs, &rhs});
    const auto predicate = EmitF32ProductIsTinyContext(ctx, inst);
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
    Require(ValidateAndOptimizeSpirv(words, 0x00401000u, 0x00010300u, false, false) == words, "context predicate validation changed its module");
    return {words, predicate};
}

bool EvaluateContextShortcut(const ContextPredicateModule& emitted) {
    std::unordered_map<std::uint32_t, std::uint32_t> values;
    for (std::size_t offset = 5u; offset < emitted.words.size();) {
        const auto count = emitted.words[offset] >> 16u;
        const auto opcode = emitted.words[offset] & 0xffffu;
        Require(count > 0u && offset + count <= emitted.words.size(), "invalid context SPIR-V word count");
        const auto operand = [&](std::size_t index) { return values.at(emitted.words[offset + index]); };
        switch (opcode) {
        case spv::OpConstant: values[emitted.words[offset + 2u]] = emitted.words[offset + 3u]; break;
        case spv::OpConstantTrue: values[emitted.words[offset + 2u]] = 1u; break;
        case spv::OpConstantFalse: values[emitted.words[offset + 2u]] = 0u; break;
        case spv::OpCopyObject: values[emitted.words[offset + 2u]] = operand(3u); break;
        case spv::OpBitFieldUExtract: values[emitted.words[offset + 2u]] = (operand(3u) >> operand(4u)) & ((1u << operand(5u)) - 1u); break;
        case spv::OpINotEqual: values[emitted.words[offset + 2u]] = operand(3u) != operand(4u); break;
        case spv::OpLogicalAnd: values[emitted.words[offset + 2u]] = operand(3u) != 0u && operand(4u) != 0u; break;
        case spv::OpIAdd: values[emitted.words[offset + 2u]] = operand(3u) + operand(4u); break;
        case spv::OpULessThan: values[emitted.words[offset + 2u]] = operand(3u) < operand(4u); break;
        default: break;
        }
        offset += count;
    }
    return values.at(emitted.result) != 0u;
}

void CheckContextStructure(const ContextPredicateModule& emitted, std::uint32_t expectedSelections, std::uint32_t expectedMultiplies) {
    std::uint32_t selections = 0u;
    std::uint32_t multiplies = 0u;
    for (std::size_t offset = 5u; offset < emitted.words.size();) {
        const auto count = emitted.words[offset] >> 16u;
        const auto opcode = emitted.words[offset] & 0xffffu;
        Require(count > 0u && offset + count <= emitted.words.size(), "invalid context SPIR-V word count");
        if (opcode == spv::OpSelectionMerge) ++selections;
        if (opcode == spv::OpUMulExtended || opcode == spv::OpIMul) ++multiplies;
        offset += count;
    }
    Require(selections == expectedSelections, "constant context has the wrong selection count");
    Require(multiplies == expectedMultiplies, "constant context has the wrong carry multiply count");
}

void CheckConstantContext() {
    const std::array<std::uint32_t, 12> otherValues{
        0u, 0x80000000u, 1u, Bits(1u, 0u), Bits(1u, 0x7fffffu), Bits(2u, 0u),
        Bits(64u, 0x7fffffu), Bits(126u, 1u), Bits(127u, 0u), 0x7f800000u, 0xff800000u, Bits(255u, 0x412345u)};
    for (const auto exponent : {0u, 1u, 63u, 126u, 127u, 255u}) {
        for (const auto negative : {false, true}) {
            for (const auto fraction : {0u, 1u}) {
                const auto factor = Bits(exponent, fraction, negative);
                const bool alwaysFalse = exponent == 0u || exponent >= 127u;
                const bool shortcut = alwaysFalse || fraction == 0u;
                for (const auto other : otherValues) {
                    for (const auto position : {0u, 1u}) {
                        const auto a = position == 0u ? factor : other;
                        const auto b = position == 0u ? other : factor;
                        const auto emitted = EmitContextPredicate(a, b, 1u << position);
                        CheckContextStructure(emitted, shortcut ? 0u : 1u, shortcut ? 0u : 1u);
                        if (shortcut) {
                            const auto result = EvaluateContextShortcut(emitted);
                            CheckFold(a, b, result ? 1 : 0);
                        }
                    }
                }
            }
        }
    }
    for (const bool loopHeader : {false, true}) {
        const auto dynamic = EmitContextPredicate(Bits(63u, 0x7fffffu), Bits(64u, 1u), 0u, loopHeader);
        CheckContextStructure(dynamic, loopHeader ? 0u : 1u, 1u);
        for (const auto position : {0u, 1u}) {
            const auto known = Bits(126u, 0u, true);
            const auto other = Bits(1u, 0x7fffffu);
            const auto unit = EmitContextPredicate(position == 0u ? known : other, position == 0u ? other : known, 1u << position, loopHeader);
            CheckContextStructure(unit, 0u, 0u);
            CheckFold(known, other, EvaluateContextShortcut(unit) ? 1 : 0);
        }
    }
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
        CheckConstantContext();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
