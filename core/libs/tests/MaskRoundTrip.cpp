#include "IntermediateRepresentation/IrBuilder.hpp"
#include "Optimization/MaskRoundTripEliminator.hpp"
#include "Optimization/ConstantFolder.hpp"
#include "Optimization/DeadCodeEliminator.hpp"
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace ShaderRecompiler;

namespace {
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(std::string("mask roundtrip: ") + message);
}

struct Fixture {
    IrProgram program;
    IrBuilder ir{program};
    IrBlock* block;
    IrValue* predicate;
    IrValue* ballot;
    IrValue* low;
    IrValue* high;
    IrValue* lane;
    IrValue* condition;
    IrValue* word;
    IrValue* bitIndex;
    IrValue* shift;
    IrValue* test;
    IrValue* keep;

    explicit Fixture(std::uint32_t wave, bool predecessorBallot = false) {
        program.SetWaveSize(wave);
        program.Resources().stage = IrShaderStage::Compute;
        auto& entry = program.CreateBlock();
        program.SetEntryBlock(entry);
        program.BlockOrder().push_back(&entry);
        ir.SetInsertionPoint(entry);
        predicate = &ir.Emit(IrOpcode::UndefU1, IrType::U1, {});
        ballot = &ir.Emit(IrOpcode::Ballot, IrType::U32x4, {predicate});
        block = &entry;
        if (predecessorBallot) {
            block = &program.CreateBlock();
            program.BlockOrder().push_back(block);
            entry.AddBranch(block);
            ir.SetInsertionPoint(*block);
        }
        low = &ir.CompositeExtract(*ballot, 0u);
        high = &ir.CompositeExtract(*ballot, 1u);
        lane = &ir.Emit(IrOpcode::LaneId, IrType::U32, {});
        condition = &ir.ULessThan(*lane, ir.Constant(32u));
        word = wave == 64u ? &ir.Select(*condition, *low, *high) : low;
        bitIndex = &ir.BitwiseAnd(*lane, ir.Constant(31u));
        shift = &ir.ShiftRightLogical(*word, *bitIndex);
        test = &ir.INotEqual(ir.BitwiseAnd(*shift, ir.Constant(1u)), ir.Constant(0u));
        keep = &ir.Emit(IrOpcode::Reference, IrType::Void, {test});
    }

    void Positive() {
        const auto stats = MaskRoundTripEliminator{}.Eliminate(program);
        Require(stats.rewrittenBallotBits == 1u && stats.rewrittenConstantBits == 0u, "exact ballot own-bit did not rewrite once");
        Require(keep->Argument(0)->Resolve() == predicate, "ballot own-bit did not become the original predicate");
    }
    void Negative() {
        const auto stats = MaskRoundTripEliminator{}.Eliminate(program);
        Require(stats.rewrittenBallotBits == 0u && stats.rewrittenConstantBits == 0u, "a mismatched or stale ballot graph was rewritten");
        Require(keep->Argument(0)->Resolve() == test, "negative fixture changed");
    }
};

void CheckBallotPatterns() {
    for (const auto wave : {32u, 64u}) {
        Fixture exact(wave); exact.Positive();
        Fixture stale(wave, true); stale.Negative();
        Fixture wrongIndex(wave);
        wrongIndex.bitIndex->ReplaceArgument(1u, &wrongIndex.ir.Constant(30u));
        wrongIndex.Negative();
        Fixture wrongLane(wave);
        auto& otherLane = wrongLane.program.CreateValue(IrOpcode::LaneId, IrType::U32);
        wrongLane.block->InsertInstructionBefore(wrongLane.bitIndex, &otherLane);
        if (wave == 32u) {
            auto& offset = wrongLane.program.CreateValue(IrOpcode::IAdd32, IrType::U32);
            offset.AddArgument(&otherLane); offset.AddArgument(&wrongLane.ir.Constant(1u));
            wrongLane.block->InsertInstructionBefore(wrongLane.bitIndex, &offset);
            wrongLane.bitIndex->ReplaceArgument(0u, &offset);
        } else {
            wrongLane.bitIndex->ReplaceArgument(0u, &otherLane);
        }
        wrongLane.Negative();
        Fixture wrongWord(wave);
        if (wave == 32u) wrongWord.shift->ReplaceArgument(0u, wrongWord.high);
        else wrongWord.word->ReplaceArgument(1u, wrongWord.high);
        wrongWord.Negative();
        Fixture preserved(wave);
        auto& additionalUse = preserved.ir.Emit(IrOpcode::ReferenceU32, IrType::Void, {preserved.low});
        preserved.Positive();
        DeadCodeEliminator{}.RemoveIdentities(preserved.program);
        DeadCodeEliminator{}.Eliminate(preserved.program);
        Require(additionalUse.Argument(0)->Resolve() == preserved.low && preserved.ballot->HasUses(), "an unrelated ballot consumer was deleted");
    }
    Fixture wrongCut(64u);
    wrongCut.condition->ReplaceArgument(1u, &wrongCut.ir.Constant(31u));
    wrongCut.Negative();
    Fixture otherBallot(64u);
    auto& separate = otherBallot.program.CreateValue(IrOpcode::Ballot, IrType::U32x4);
    separate.AddArgument(otherBallot.predicate);
    otherBallot.block->InsertInstructionBefore(otherBallot.high, &separate);
    otherBallot.high->ReplaceArgument(0u, &separate);
    otherBallot.Negative();
    Fixture lowOnly(64u);
    lowOnly.shift->ReplaceArgument(0u, lowOnly.low);
    lowOnly.Negative();
    Fixture graphics(64u);
    graphics.program.Resources().stage = IrShaderStage::Pixel;
    graphics.Negative();
}

void CheckFreshLoopBallot() {
    IrProgram program;
    program.SetWaveSize(64u);
    program.Resources().stage = IrShaderStage::Compute;
    auto& entry = program.CreateBlock();
    auto& loop = program.CreateBlock();
    program.SetEntryBlock(entry);
    program.BlockOrder() = {&entry, &loop};
    entry.AddBranch(&loop);
    loop.AddBranch(&loop);
    IrBuilder ir(program);
    ir.SetInsertionPoint(loop);
    auto& predicate = program.CreateValue(IrOpcode::Phi, IrType::U1);
    loop.AppendInstruction(&predicate);
    predicate.AddPhiOperand(&entry, &ir.ConstantBool(true));
    auto& ballot = ir.Emit(IrOpcode::Ballot, IrType::U32x4, {&predicate});
    auto& lane = ir.Emit(IrOpcode::LaneId, IrType::U32, {});
    auto& word = ir.Select(ir.ULessThan(lane, ir.Constant(32u)), ir.CompositeExtract(ballot, 0u), ir.CompositeExtract(ballot, 1u));
    auto& bit = ir.BitwiseAnd(ir.ShiftRightLogical(word, ir.BitwiseAnd(lane, ir.Constant(31u))), ir.Constant(1u));
    auto& keep = ir.Emit(IrOpcode::Reference, IrType::Void, {&ir.INotEqual(bit, ir.Constant(0u))});
    auto& next = ir.LogicalNot(predicate);
    predicate.AddPhiOperand(&loop, &next);
    const auto stats = MaskRoundTripEliminator{}.Eliminate(program);
    Require(stats.rewrittenBallotBits == 1u && keep.Argument(0)->Resolve() == &predicate, "fresh loop-local ballot lost its current phi predicate");
}

void CheckConstantMasks() {
    for (const auto wave : {32u, 64u}) {
        for (const auto mask : {0u, 0xffffffffu}) {
            Fixture value(wave);
            value.shift->ReplaceArgument(0u, &value.ir.Constant(mask));
            const auto stats = MaskRoundTripEliminator{}.Eliminate(value.program);
            const auto* result = value.keep->Argument(0)->Resolve();
            Require(stats.rewrittenConstantBits == 1u && stats.rewrittenBallotBits == 0u, "full/zero own-bit did not rewrite once");
            Require(result->HasImmediate() && result->ImmediateBool() == (mask != 0u), "full/zero own-bit has the wrong truth value");
        }
        Fixture partial(wave);
        partial.shift->ReplaceArgument(0u, &partial.ir.Constant(0x7fffffffu));
        partial.Negative();
    }
}

void CheckActiveLaneSemantics() {
    std::uint64_t random = 0x13579bdf2468ace0ull;
    for (const auto wave : {32u, 64u}) {
        for (std::uint32_t iteration = 0u; iteration < 128u; ++iteration) {
            random ^= random << 13u; random ^= random >> 7u; random ^= random << 17u;
            const std::uint64_t predicate = random;
            random ^= random << 13u; random ^= random >> 7u; random ^= random << 17u;
            const std::uint64_t active = random;
            const std::uint64_t ballot = predicate & active;
            for (std::uint32_t lane = 0u; lane < wave; ++lane) {
                if (((active >> lane) & 1u) == 0u) continue;
                const auto word = static_cast<std::uint32_t>(ballot >> (lane < 32u ? 0u : 32u));
                Require((((word >> (lane & 31u)) & 1u) != 0u) == (((predicate >> lane) & 1u) != 0u), "active own-bit differs from predicate");
            }
        }
    }
}
}

int main() {
    try {
        CheckBallotPatterns(); CheckFreshLoopBallot(); CheckConstantMasks(); CheckActiveLaneSemantics();
        std::cout << "mask roundtrip PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
