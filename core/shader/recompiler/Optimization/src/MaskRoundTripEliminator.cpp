#include "Optimization/MaskRoundTripEliminator.hpp"
#include "IntermediateRepresentation/IrBuilder.hpp"
#include <unordered_set>

namespace ShaderRecompiler {
namespace {

bool IsU32(IrValue* value, std::uint32_t bits) {
    value = value->Resolve();
    return value->Type() == IrType::U32 && value->HasImmediate() && value->ImmediateU32() == bits;
}

struct Matcher {
    IrBlock* block;
    const std::unordered_set<IrValue*>& preceding;

    IrValue* Local(IrValue* value, IrOpcode opcode, std::size_t arguments) const {
        value = value->Resolve();
        return value->Opcode() == opcode && value->ArgumentCount() == arguments &&
            value->Parent() == block && preceding.contains(value) ? value : nullptr;
    }

    IrValue* WithConstant(IrValue* value, IrOpcode opcode, std::uint32_t bits) const {
        IrValue* operation = Local(value, opcode, 2u);
        if (operation == nullptr) return nullptr;
        if (IsU32(operation->Argument(1), bits)) return operation->Argument(0)->Resolve();
        if (IsU32(operation->Argument(0), bits)) return operation->Argument(1)->Resolve();
        return nullptr;
    }

    IrValue* BallotWord(IrValue* value, std::uint32_t index) const {
        IrValue* extract = Local(value, IrOpcode::CompositeExtractU32x4, 2u);
        if (extract == nullptr || !IsU32(extract->Argument(1), index)) return nullptr;
        return Local(extract->Argument(0), IrOpcode::Ballot, 1u);
    }

    IrValue* BallotPredicate(IrValue* word, IrValue* lane, std::uint32_t waveSize) const {
        IrValue* ballot = nullptr;
        if (waveSize == 32u) {
            ballot = BallotWord(word, 0u);
        } else {
            IrValue* select = Local(word, IrOpcode::SelectU32, 3u);
            if (select == nullptr) return nullptr;
            IrValue* lowHalf = Local(select->Argument(0), IrOpcode::ULessThan32, 2u);
            if (lowHalf == nullptr || lowHalf->Argument(0)->Resolve() != lane || !IsU32(lowHalf->Argument(1), 32u)) return nullptr;
            ballot = BallotWord(select->Argument(1), 0u);
            if (ballot == nullptr || BallotWord(select->Argument(2), 1u) != ballot) return nullptr;
        }
        return ballot != nullptr ? ballot->Argument(0)->Resolve() : nullptr;
    }
};

}

MaskRoundTripEliminationStats MaskRoundTripEliminator::Eliminate(IrProgram& program) const {
    MaskRoundTripEliminationStats stats;
    if (program.Resources().stage != IrShaderStage::Compute || (program.WaveSize() != 32u && program.WaveSize() != 64u)) return stats;
    IrBuilder ir(program);
    for (const auto& block : program.Blocks()) {
        std::unordered_set<IrValue*> preceding;
        const Matcher matcher{block.get(), preceding};
        for (IrValue* inst : block->Instructions()) {
            if (inst->Opcode() == IrOpcode::INotEqual32 && inst->ArgumentCount() == 2u) {
                IrValue* bit = IsU32(inst->Argument(1), 0u) ? inst->Argument(0)->Resolve() :
                    IsU32(inst->Argument(0), 0u) ? inst->Argument(1)->Resolve() : nullptr;
                IrValue* shifted = bit != nullptr ? matcher.WithConstant(bit, IrOpcode::BitwiseAnd32, 1u) : nullptr;
                IrValue* shift = shifted != nullptr ? matcher.Local(shifted, IrOpcode::ShiftRightLogical32, 2u) : nullptr;
                IrValue* lane = shift != nullptr ? matcher.WithConstant(shift->Argument(1), IrOpcode::BitwiseAnd32, 31u) : nullptr;
                if (lane != nullptr && matcher.Local(lane, IrOpcode::LaneId, 0u) != nullptr) {
                    IrValue* word = shift->Argument(0)->Resolve();
                    if (IsU32(word, 0u) || IsU32(word, 0xffffffffu)) {
                        inst->ReplaceUsesWith(&ir.ConstantBool(IsU32(word, 0xffffffffu)), true);
                        ++stats.rewrittenConstantBits;
                    } else if (IrValue* predicate = matcher.BallotPredicate(word, lane, program.WaveSize()); predicate != nullptr) {
                        inst->ReplaceUsesWith(predicate, true);
                        ++stats.rewrittenBallotBits;
                    }
                }
            }
            preceding.insert(inst);
        }
    }
    return stats;
}

}
