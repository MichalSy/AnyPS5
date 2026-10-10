#include "SpirvBackend/SpirvModule.hpp"
#include "SpirvBackend/SpirvEmitterInstructions.hpp"
#include "IntermediateRepresentation/IrProgram.hpp"
#include "Translation/TranslationContext.hpp"
#include <bit>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <cstdio>
#include <map>
#include <set>
#include <spirv/unified1/spirv.hpp>
#include <vector>

using namespace ShaderRecompiler;

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", what);
        ++failures;
    }
}

std::map<std::uint32_t, std::size_t> opcodeCounts(const std::vector<std::uint32_t>& words) {
    std::map<std::uint32_t, std::size_t> counts;
    std::size_t offset = 5;
    while (offset < words.size()) {
        const auto header = words[offset];
        const auto wordCount = header >> spv::WordCountShift;
        if (wordCount == 0 || offset + wordCount > words.size()) {
            return counts;
        }
        ++counts[header & 0xffffu];
        offset += wordCount;
    }
    return counts;
}

std::size_t declared(const SpirvModule& module, std::uint32_t opcode) {
    const auto counts = opcodeCounts(module.Finalize());
    const auto found = counts.find(opcode);
    return found == counts.end() ? 0 : found->second;
}

void testRepeatedTypesShareOneId() {
    SpirvModule module;
    const auto first = module.Type(spv::OpTypeInt, 32u, 0u);
    const auto second = module.Type(spv::OpTypeInt, 32u, 0u);
    check(first == second, "a repeated OpTypeInt 32 0 returned a different id");
    check(declared(module, spv::OpTypeInt) == 1, "a repeated OpTypeInt 32 0 was declared more than once");
    check(module.Type(spv::OpTypeVoid) == module.Type(spv::OpTypeVoid), "a repeated OpTypeVoid returned a different id");
    check(declared(module, spv::OpTypeVoid) == 1, "OpTypeVoid was not declared exactly once");
    check(module.Type(spv::OpTypeBool) == module.Type(spv::OpTypeBool), "a repeated OpTypeBool returned a different id");
    check(module.Type(spv::OpTypeFloat, 32u) == module.Type(spv::OpTypeFloat, 32u), "a repeated OpTypeFloat 32 returned a different id");
    check(declared(module, spv::OpTypeFloat) == 1, "OpTypeFloat was not declared exactly once");
}

void testOperandShapesDoNotCollide() {
    SpirvModule module;
    const auto oneWord = module.Type(spv::OpTypeInt, 32u);
    const auto twoWords = module.Type(spv::OpTypeInt, 32u, 0u);
    check(oneWord != twoWords, "OpTypeInt with one operand collided with OpTypeInt with two");
    check(declared(module, spv::OpTypeInt) == 2, "the two OpTypeInt shapes were not both declared");
    const auto voidType = module.Type(spv::OpTypeVoid);
    const auto boolType = module.Type(spv::OpTypeBool);
    check(voidType != boolType, "OpTypeVoid collided with OpTypeBool");
    check(module.Type(spv::OpTypeInt, 32u, 0u) != module.Type(spv::OpTypeInt, 32u, 1u), "unsigned and signed OpTypeInt 32 collided");
    check(module.Type(spv::OpTypeInt, 32u, 0u) != module.Type(spv::OpTypeInt, 64u, 0u), "OpTypeInt 32 collided with OpTypeInt 64");
    check(module.Type(spv::OpTypeFloat, 32u) != module.Type(spv::OpTypeFloat, 64u), "OpTypeFloat 32 collided with OpTypeFloat 64");
}

void testOperandKindsAgree() {
    SpirvModule module;
    const auto fromUnsigned = module.Type(spv::OpTypeInt, 32u, 0u);
    const auto fromSigned = module.Type(spv::OpTypeInt, static_cast<std::int32_t>(32), static_cast<std::int32_t>(0));
    check(fromUnsigned == fromSigned, "OpTypeInt disagreed between unsigned and signed operands");
    const auto typeU32 = module.Type(spv::OpTypeInt, 32u, 0u);
    const auto fromEnum = module.Type(spv::OpTypePointer, spv::StorageClassFunction, typeU32);
    const auto fromWord = module.Type(spv::OpTypePointer, static_cast<std::uint32_t>(spv::StorageClassFunction), typeU32);
    check(fromEnum == fromWord, "OpTypePointer disagreed between enum and integer operands");
    check(fromEnum == module.Type(spv::OpTypePointer, spv::StorageClassFunction, typeU32), "a repeated enum-operand OpTypePointer returned a different id");
    check(declared(module, spv::OpTypePointer) == 1, "OpTypePointer was not declared exactly once");
}

void testDecoratedTypesStayDistinct() {
    SpirvModule module;
    const auto typeU32 = module.Type(spv::OpTypeInt, 32u, 0u);
    const auto plain = module.Type(spv::OpTypeRuntimeArray, typeU32);
    const auto decorated = module.DecoratedType(spv::OpTypeRuntimeArray, {{spv::OpDecorate, {spv::DecorationArrayStride, 4u}}}, typeU32);
    check(plain != decorated, "a plain OpTypeRuntimeArray collided with the decorated one");
    check(decorated == module.DecoratedType(spv::OpTypeRuntimeArray, {{spv::OpDecorate, {spv::DecorationArrayStride, 4u}}}, typeU32), "a repeated decorated type returned a different id");
    check(declared(module, spv::OpTypeRuntimeArray) == 2, "the plain and decorated OpTypeRuntimeArray were not both declared");
}

void testConstantsShareOneId() {
    SpirvModule module;
    const auto typeU32 = module.Type(spv::OpTypeInt, 32u, 0u);
    const auto typeBool = module.Type(spv::OpTypeBool);
    check(module.Constant(spv::OpConstant, typeU32, 0u) == module.Constant(spv::OpConstant, typeU32, 0u), "a repeated OpConstant 0 returned a different id");
    check(module.Constant(spv::OpConstant, typeU32, 0u) != module.Constant(spv::OpConstant, typeU32, 1u), "OpConstant 0 collided with OpConstant 1");
    check(module.Constant(spv::OpConstant, typeU32, 1000u) != module.Constant(spv::OpConstant, typeU32, 0u), "OpConstant 1000 collided with OpConstant 0");
    check(module.Constant(spv::OpConstantTrue, typeBool) == module.Constant(spv::OpConstantTrue, typeBool), "a repeated OpConstantTrue returned a different id");
    check(module.Constant(spv::OpConstantTrue, typeBool) != module.Constant(spv::OpConstantFalse, typeBool), "OpConstantTrue collided with OpConstantFalse");
    check(declared(module, spv::OpConstant) == 3, "OpConstant was not declared once per distinct value");
    check(declared(module, spv::OpConstantTrue) == 1, "OpConstantTrue was not declared exactly once");
    const auto signedId = module.Constant(spv::OpConstant, module.Type(spv::OpTypeInt, 32u, 1u), static_cast<std::uint32_t>(-1));
    check(signedId != 0, "a signed OpConstant produced a zero id");
}

void testOperandFreeConstantsShareOneId() {
    SpirvModule module;
    const auto typeBool = module.Type(spv::OpTypeBool);
    const auto typeVoid = module.Type(spv::OpTypeVoid);
    check(module.Constant(spv::OpConstantTrue, typeBool) == module.Constant(spv::OpConstantTrue, typeBool), "a repeated OpConstantTrue returned a different id");
    check(module.Constant(spv::OpConstantTrue, typeBool) != module.Constant(spv::OpConstantFalse, typeBool), "OpConstantTrue collided with OpConstantFalse");
    check(module.Constant(spv::OpConstantTrue, typeVoid) != module.Constant(spv::OpConstantTrue, typeBool), "OpConstantTrue of different types collided");
    check(declared(module, spv::OpConstantTrue) == 2, "OpConstantTrue was not declared once per type");
    check(declared(module, spv::OpConstantFalse) == 1, "OpConstantFalse was not declared exactly once");
    check(module.Type(spv::OpTypeVoid) == typeVoid, "a repeated operand-free type returned a different id");
}

void testIdsSurviveInterleavedAllocation() {
    SpirvModule module;
    const auto typeU32 = module.Type(spv::OpTypeInt, 32u, 0u);
    const auto constant = module.Constant(spv::OpConstant, typeU32, 4u);
    for (int i = 0; i < 8; ++i) {
        check(module.AllocateId() != 0, "AllocateId returned zero");
        check(module.Type(spv::OpTypeInt, 32u, 0u) == typeU32, "a memoized type changed id after AllocateId");
        check(module.Constant(spv::OpConstant, typeU32, 4u) == constant, "a memoized constant changed id after AllocateId");
    }
}

void testModulesAreIndependent() {
    SpirvModule first;
    SpirvModule second;
    const auto firstId = first.Type(spv::OpTypeInt, 32u, 0u);
    const auto secondId = second.Type(spv::OpTypeInt, 32u, 0u);
    check(firstId == 1, "the first id of a fresh module was not 1");
    check(secondId == 1, "a second module did not start its ids from 1");
    first.Type(spv::OpTypeVoid);
    check(second.Type(spv::OpTypeInt, 32u, 0u) == secondId, "a second module's ids moved when the first module was used");
}

void testIdsStayUniqueBeyondCacheCapacity() {
    SpirvModule module;
    const auto typeU32 = module.Type(spv::OpTypeInt, 32u, 0u);
    std::vector<std::uint32_t> ids;
    ids.reserve(4096);
    for (std::uint32_t value = 0; value < 4096; ++value) {
        ids.push_back(module.Constant(spv::OpConstant, typeU32, value));
    }
    const std::set<std::uint32_t> distinct(ids.begin(), ids.end());
    check(distinct.size() == ids.size(), "distinct constants shared an id once the cache was overrun");
    for (std::uint32_t value = 0; value < 4096; ++value) {
        if (module.Constant(spv::OpConstant, typeU32, value) != ids[value]) {
            std::fprintf(stderr, "constant %u changed id after the cache was overrun\n", value);
            ++failures;
            break;
        }
    }
    std::uint32_t largest = 0;
    for (const auto id : distinct) {
        largest = largest < id ? id : largest;
    }
    const auto first = ids.front();
    check(largest - first + 1 == distinct.size(), "the constant ids were not contiguous, so some were never declared");
    check(first > typeU32, "a constant was numbered before the type it uses");
}

void testEmittedWordsAreStable() {
    SpirvModule module;
    const auto typeU32 = module.Type(spv::OpTypeInt, 32u, 0u);
    for (int i = 0; i < 32; ++i) {
        module.Type(spv::OpTypeVector, typeU32, 4u);
        module.Type(spv::OpTypePointer, spv::StorageClassFunction, typeU32);
        module.Constant(spv::OpConstant, typeU32, static_cast<std::uint32_t>(i % 8));
        module.AddFunction(spv::OpNop);
    }
    const auto words = module.Finalize();
    check(!words.empty(), "Finalize produced no words");
    const auto counts = opcodeCounts(words);
    check(counts.at(spv::OpTypeVector) == 1, "OpTypeVector was declared more than once");
    check(counts.at(spv::OpTypePointer) == 1, "OpTypePointer was declared more than once");
    check(counts.at(spv::OpConstant) == 8, "OpConstant was not declared once per distinct value");
    check(counts.at(spv::OpNop) == 32, "the function body lost instructions");
}

std::uint32_t conversionResult(const std::vector<std::uint32_t>& words, std::uint32_t result) {
    std::map<std::uint32_t, std::vector<std::uint32_t>> values;
    for (std::size_t offset = 5; offset < words.size(); offset += words[offset] >> spv::WordCountShift) {
        const auto count = words[offset] >> spv::WordCountShift;
        const auto opcode = words[offset] & spv::OpCodeMask;
        switch (opcode) {
            case spv::OpConstant: case spv::OpSpecConstant: case spv::OpBitcast:
            case spv::OpBitwiseAnd: case spv::OpUGreaterThan: case spv::OpIEqual:
            case spv::OpFOrdLessThanEqual: case spv::OpFOrdGreaterThanEqual:
            case spv::OpLogicalOr: case spv::OpLogicalAnd: case spv::OpSelect:
            case spv::OpExtInst: case spv::OpConvertFToS: case spv::OpConvertFToU:
                values[words[offset + 2]] = std::vector<std::uint32_t>(words.begin() + offset, words.begin() + offset + count);
                break;
            default: break;
        }
    }
    const std::function<std::uint32_t(std::uint32_t)> evaluate = [&](std::uint32_t id) -> std::uint32_t {
        const auto& inst = values.at(id);
        const auto opcode = inst[0] & spv::OpCodeMask;
        const auto arg = [&](std::size_t index) { return evaluate(inst[index]); };
        const auto fp = [&](std::size_t index) { return std::bit_cast<float>(arg(index)); };
        switch (opcode) {
            case spv::OpConstant: case spv::OpSpecConstant: return inst[3];
            case spv::OpBitcast: return arg(3);
            case spv::OpBitwiseAnd: return arg(3) & arg(4);
            case spv::OpUGreaterThan: return arg(3) > arg(4);
            case spv::OpIEqual: return arg(3) == arg(4);
            case spv::OpFOrdLessThanEqual: return fp(3) <= fp(4);
            case spv::OpFOrdGreaterThanEqual: return fp(3) >= fp(4);
            case spv::OpLogicalOr: return arg(3) || arg(4);
            case spv::OpLogicalAnd: return arg(3) && arg(4);
            case spv::OpSelect: return arg(3) ? arg(4) : arg(5);
            case spv::OpExtInst:
                if (inst[4] != 3u) throw std::runtime_error("conversion fixture: unexpected extended instruction");
                return std::bit_cast<std::uint32_t>(std::trunc(fp(5)));
            case spv::OpConvertFToS: case spv::OpConvertFToU: {
                const double input = std::trunc(static_cast<double>(fp(3)));
                const bool signedValue = opcode == spv::OpConvertFToS;
                const double low = signedValue ? -2147483648.0 : 0.0;
                const double high = signedValue ? 2147483647.0 : 4294967295.0;
                if (!std::isfinite(input) || input < low || input > high)
                    throw std::runtime_error("conversion fixture: selected undefined SPIR-V conversion");
                return signedValue ? static_cast<std::uint32_t>(static_cast<std::int32_t>(input)) : static_cast<std::uint32_t>(input);
            }
            default: throw std::runtime_error("conversion fixture: unexpected result instruction");
        }
    };
    return evaluate(result);
}

void testF32IntegerConversionContract() {
    struct Row { std::uint32_t input, signedResult, unsignedResult; };
    constexpr Row rows[]{
        {0x00000000u, 0u, 0u}, {0x80000000u, 0u, 0u},
        {0x00000001u, 0u, 0u}, {0x80000001u, 0u, 0u},
        {0x007fffffu, 0u, 0u}, {0x807fffffu, 0u, 0u},
        {0x3f400000u, 0u, 0u}, {0xbf400000u, 0u, 0u},
        {0x3fe00000u, 1u, 1u}, {0xbfe00000u, 0xffffffffu, 0u},
        {0x4effffffu, 0x7fffff80u, 0x7fffff80u},
        {0x4f000000u, 0x7fffffffu, 0x80000000u},
        {0x4f000001u, 0x7fffffffu, 0x80000100u},
        {0xceffffffu, 0x80000080u, 0u},
        {0xcf000000u, 0x80000000u, 0u},
        {0xcf000001u, 0x80000000u, 0u},
        {0x4f7fffffu, 0x7fffffffu, 0xffffff00u},
        {0x4f800000u, 0x7fffffffu, 0xffffffffu},
        {0x4f800001u, 0x7fffffffu, 0xffffffffu},
        {0x7f7fffffu, 0x7fffffffu, 0xffffffffu},
        {0xff7fffffu, 0x80000000u, 0u},
        {0x7f800000u, 0x7fffffffu, 0xffffffffu},
        {0xff800000u, 0x80000000u, 0u},
        {0x7fc12345u, 0u, 0u}, {0xffc12345u, 0u, 0u},
        {0x7f800001u, 0u, 0u}, {0xff800001u, 0u, 0u},
        {0x7fffffffu, 0u, 0u}, {0xffffffffu, 0u, 0u},
    };
    for (const auto& row : rows) for (const bool signedValue : {false, true}) {
        const double value = std::bit_cast<float>(row.input);
        const bool finiteInRange = std::isfinite(value) &&
            value >= (signedValue ? -2147483648.0 : 0.0) &&
            value < (signedValue ? 2147483648.0 : 4294967296.0);
        for (const bool trusted : {false, true}) {
            if (trusted && !finiteInRange) continue;
            IrProgram program;
            program.Resources().stage = IrShaderStage::Compute;
            auto& inst = program.CreateValue(signedValue ? IrOpcode::ConvertS32F32 : IrOpcode::ConvertU32F32, IrType::U32);
            if (trusted) inst.SetFlags(F32IntegerConvertFlags{true});
            SpirvEmitterState state(program, {});
            const auto source = ConstantF32(state, row.input);
            const auto result = signedValue ? EmitConvertS32F32(state, inst, source) : EmitConvertU32F32(state, inst, source);
            const auto words = state.module.Finalize();
            check(conversionResult(words, result) == (signedValue ? row.signedResult : row.unsignedResult), "f32 integer conversion changed a golden raw-IR result");
            const auto counts = opcodeCounts(words);
            const auto present = [&](spv::Op opcode) { const auto found = counts.find(opcode); return found != counts.end() && found->second != 0u; };
            check(present(signedValue ? spv::OpConvertFToS : spv::OpConvertFToU), "f32 integer conversion is missing");
            check(present(spv::OpSelect) != trusted, "trusted range flag did not preserve the generic fallback or select the fast path");
        }
    }
}

std::uint32_t translatedConversionValue(const IrValue& value, bool active) {
    if (value.HasImmediate()) return static_cast<std::uint32_t>(value.ImmediateU64());
    const auto arg = [&](std::size_t index) { return translatedConversionValue(*value.Argument(index), active); };
    const auto fp = [&](std::size_t index) { return std::bit_cast<float>(arg(index)); };
    switch (value.Opcode()) {
        case IrOpcode::GetExec: return active;
        case IrOpcode::GetVectorRegister: return 0x12345678u;
        case IrOpcode::BitCastF32U32: case IrOpcode::BitCastU32F32: return arg(0);
        case IrOpcode::BitwiseAnd32: return arg(0) & arg(1);
        case IrOpcode::BitwiseXor32: return arg(0) ^ arg(1);
        case IrOpcode::LogicalOr: return arg(0) || arg(1);
        case IrOpcode::FPIsNan32: return (arg(0) & 0x7fffffffu) > 0x7f800000u;
        case IrOpcode::FPOrdLessThanEqual32: return fp(0) <= fp(1);
        case IrOpcode::FPOrdGreaterThanEqual32: return fp(0) >= fp(1);
        case IrOpcode::SelectU32: case IrOpcode::SelectF32: return arg(0) ? arg(1) : arg(2);
        case IrOpcode::ConvertS32F32: case IrOpcode::ConvertU32F32: {
            const double input = std::trunc(static_cast<double>(fp(0)));
            const bool signedValue = value.Opcode() == IrOpcode::ConvertS32F32;
            if (!value.Flags<F32IntegerConvertFlags>().inputClampedToIntegerRange || !std::isfinite(input) ||
                input < (signedValue ? -2147483648.0 : 0.0) || input > (signedValue ? 2147483647.0 : 4294967295.0))
                throw std::runtime_error("translated conversion did not establish its trusted range contract");
            return signedValue ? static_cast<std::uint32_t>(static_cast<std::int32_t>(input)) : static_cast<std::uint32_t>(input);
        }
        default: throw std::runtime_error("unexpected translated conversion dependency");
    }
}

void testTranslatedF32IntegerModifiers() {
    constexpr std::uint32_t inputs[]{0u,0x80000000u,1u,0x80000001u,0x007fffffu,0x807fffffu,
        0x3fe00000u,0xbfe00000u,0x4effffffu,0x4f000000u,0xcf000000u,0x4f7fffffu,0x4f800000u,
        0x7f800000u,0xff800000u,0x7fc12345u,0xff800001u};
    for (const auto input : inputs) for (const bool signedValue : {false,true})
    for (const bool absolute : {false,true}) for (const bool negate : {false,true})
    for (const auto mode : {0xc0u,0xf0u}) for (const auto sourceKind : {RdnaOperandKind::LiteralConstant,RdnaOperandKind::FloatInlineConstant}) {
        IrProgram program;
        auto& block = program.CreateBlock();
        program.SetEntryBlock(block);
        program.BlockOrder().push_back(&block);
        TranslationContext translator(program, block, 256u);
        translator.SetFloatMode(ShaderFloatMode{mode,mode == 0xf0u,false,false});
        RdnaInstruction inst{};
        inst.op = signedValue ? RdnaOpcode::VCvtI32F32 : RdnaOpcode::VCvtU32F32;
        inst.family = RdnaInstructionFamily::VOP3;
        inst.source0.kind = sourceKind;
        inst.source0.value = input;
        inst.source0.absolute = absolute;
        inst.source0.negate = negate;
        inst.sourceCount = 1u;
        inst.destination.kind = RdnaOperandKind::VectorRegister;
        inst.destination.reg = 0u;
        inst.destination.omod = 3u;
        inst.destination.clamp = true;
        translator.TranslateInstruction(inst);
        auto bits = absolute ? input & 0x7fffffffu : input;
        if (negate) bits ^= 0x80000000u;
        const double number = std::bit_cast<float>(bits);
        std::uint32_t expected = 0;
        if (!std::isnan(number)) {
            if (signedValue && number <= -2147483648.0) expected = 0x80000000u;
            else if (number >= (signedValue ? 2147483648.0 : 4294967296.0)) expected = signedValue ? 0x7fffffffu : 0xffffffffu;
            else if (signedValue || number > 0.0) expected = signedValue ? static_cast<std::uint32_t>(static_cast<std::int32_t>(number)) : static_cast<std::uint32_t>(number);
        }
        std::uint32_t writes = 0, converts = 0;
        for (const auto* value : block.Instructions()) {
            if (value->Opcode() == IrOpcode::ConvertS32F32 || value->Opcode() == IrOpcode::ConvertU32F32) {
                (void)translatedConversionValue(*value, true);
                ++converts;
            }
            if (value->Opcode() == IrOpcode::SetVectorRegister) {
                check(translatedConversionValue(*value->Argument(1), true) == expected, "translated f32 conversion changed source/destination modifiers or a boundary result");
                check(translatedConversionValue(*value->Argument(1), false) == 0x12345678u, "translated f32 conversion changed an inactive EXEC destination");
                ++writes;
            }
        }
        check(writes == 1u && converts == 1u, "translated f32 conversion lost its destination or trusted producer");
    }
    for (const auto mode : {0xd0u,0xe0u}) {
        IrProgram program;
        auto& block = program.CreateBlock();
        TranslationContext translator(program, block, 256u);
        translator.SetFloatMode(ShaderFloatMode{mode,true,false,false});
        RdnaInstruction inst{};
        inst.op = RdnaOpcode::VCvtI32F32;
        inst.family = RdnaInstructionFamily::VOP1;
        inst.source0.kind = RdnaOperandKind::LiteralConstant;
        inst.destination.kind = RdnaOperandKind::VectorRegister;
        bool rejected = false;
        try { translator.TranslateInstruction(inst); }
        catch (const std::runtime_error& error) { rejected = std::string(error.what()).find("f32 denormal mode") != std::string::npos; }
        check(rejected, "translated f32 conversion stopped rejecting an unsupported denormal mode");
    }
}

}

int main() {
    testRepeatedTypesShareOneId();
    testOperandShapesDoNotCollide();
    testOperandKindsAgree();
    testDecoratedTypesStayDistinct();
    testConstantsShareOneId();
    testOperandFreeConstantsShareOneId();
    testIdsSurviveInterleavedAllocation();
    testModulesAreIndependent();
    testIdsStayUniqueBeyondCacheCapacity();
    testEmittedWordsAreStable();
    testF32IntegerConversionContract();
    testTranslatedF32IntegerModifiers();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
