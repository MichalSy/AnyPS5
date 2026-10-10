#include "Optimization/LoopInvariantCache.hpp"
#include "Optimization/SrtWalker/SrtFlatSlotClasses.hpp"
#include "IntermediateRepresentation/IrBuilder.hpp"
#include <algorithm>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace ShaderRecompiler {
namespace {

constexpr std::size_t MaxBlocks = 256u;
constexpr std::size_t MinInstructions = 64u;
constexpr std::size_t MaxInstructions = 512u;
constexpr std::size_t MaxOutputs = 4u;

struct Graph {
    const IrProgram& program;
    std::unordered_map<const IrBlock*, std::size_t> indices;
    std::unordered_map<std::uint32_t, IrBlock*> targets;
    std::vector<std::vector<bool>> dominators;

    explicit Graph(const IrProgram& program) : program(program) {
        const auto& blocks = program.BlockOrder();
        for (std::size_t index = 0; index < blocks.size(); ++index) {
            indices.emplace(blocks[index], index);
            targets.emplace(program.Metadata().blockInfo[index].id, blocks[index]);
        }
        dominators.assign(blocks.size(), std::vector<bool>(blocks.size(), true));
        dominators.front().assign(blocks.size(), false);
        dominators.front().front() = true;
        bool changed = true;
        while (changed) {
            changed = false;
            for (std::size_t index = 1u; index < blocks.size(); ++index) {
                std::vector<bool> next(blocks.size(), true);
                for (const auto* predecessor : blocks[index]->Predecessors()) {
                    const auto predecessorIndex = indices.at(predecessor);
                    for (std::size_t candidate = 0u; candidate < next.size(); ++candidate) {
                        next[candidate] = next[candidate] && dominators[predecessorIndex][candidate];
                    }
                }
                next[index] = true;
                if (next != dominators[index]) {
                    dominators[index] = std::move(next);
                    changed = true;
                }
            }
        }
    }

    bool Dominates(const IrBlock* definition, const IrBlock* use) const {
        return dominators[indices.at(use)][indices.at(definition)];
    }

    const BlockInfo& Info(const IrBlock* block) const {
        return program.Metadata().blockInfo[indices.at(block)];
    }
};

struct Loop {
    IrBlock* header = nullptr;
    IrBlock* preheader = nullptr;
    IrBlock* latch = nullptr;
    std::unordered_set<IrBlock*> blocks;
};

const MemoryInfo* Memory(const IrProgram& program, const IrValue& value) {
    const auto index = value.Flags<MemoryFlags>().index;
    return index < program.Resources().memoryInfo.size() ? &program.Resources().memoryInfo[index] : nullptr;
}

bool IsSchedulingOnly(IrOpcode opcode) {
    return opcode == IrOpcode::Waitcnt || opcode == IrOpcode::InstPrefetch ||
        opcode == IrOpcode::Reference || opcode == IrOpcode::ReferenceU32 || opcode == IrOpcode::Void;
}

bool IsPure(IrOpcode opcode) {
    switch (opcode) {
        case IrOpcode::IAdd32:
        case IrOpcode::ISub32:
        case IrOpcode::IMul32:
        case IrOpcode::SelectU32:
        case IrOpcode::BitCastU16F16:
        case IrOpcode::BitCastF16U16:
        case IrOpcode::BitCastU32F32:
        case IrOpcode::BitCastF32U32:
        case IrOpcode::ConvertU16U32:
        case IrOpcode::ConvertU32U16:
        case IrOpcode::ConvertU8U32:
        case IrOpcode::ConvertU32U8:
        case IrOpcode::ConvertF32F16:
        case IrOpcode::ConvertF16F32:
        case IrOpcode::ConvertS32F32:
        case IrOpcode::ConvertU32F32:
        case IrOpcode::ConvertF32S32:
        case IrOpcode::ConvertF32U32:
        case IrOpcode::CompositeConstructU64:
        case IrOpcode::CompositeExtractU64:
        case IrOpcode::CompositeExtractU32x2:
        case IrOpcode::CompositeExtractU32x3:
        case IrOpcode::CompositeExtractU32x4:
        case IrOpcode::FPAbs32:
        case IrOpcode::FPNeg32:
        case IrOpcode::FPSaturate32:
        case IrOpcode::BitFieldInsert:
        case IrOpcode::BitFieldUExtract:
        case IrOpcode::BitFieldSExtract:
            return true;
        default:
            return opcode >= IrOpcode::SelectU1 && opcode <= IrOpcode::FPFract32;
    }
}

bool IsSafeLoop(const IrProgram& program, const Loop& loop) {
    for (const auto* block : loop.blocks) {
        for (const auto* value : block->Instructions()) {
            const auto opcode = value->Opcode();
            if (value->MayHaveSideEffects() && !IsSchedulingOnly(opcode)) return false;
            if (SharedAccessOf(opcode) != SharedAccess::None) return false;
            if (!IsPure(opcode) && !IsSchedulingOnly(opcode)) {
                switch (opcode) {
                    case IrOpcode::Phi:
                    case IrOpcode::GetUserData:
                    case IrOpcode::GetShaderBase:
                    case IrOpcode::GetBuiltin:
                    case IrOpcode::GetSrtResource:
                    case IrOpcode::GetBufferResource:
                    case IrOpcode::GetAddressResource:
                    case IrOpcode::GetImageResource:
                    case IrOpcode::MakeImageAddress:
                    case IrOpcode::ReadConst:
                    case IrOpcode::ReadConstBuffer:
                    case IrOpcode::LoadAddressU32:
                    case IrOpcode::ImageRead:
                        break;
                    default:
                        return false;
                }
            }
            const auto address = AddressOpcodeInfoOf(opcode).access;
            const bool memoryRead = opcode == IrOpcode::ReadConstBuffer || address == AddressAccess::Read ||
                BufferAccessOf(opcode) == BufferAccess::Read || SharedAccessOf(opcode) == SharedAccess::Read ||
                ImageOpcodeInfoOf(opcode).access == ImageAccess::Read;
            if (!memoryRead) continue;
            const auto* memory = Memory(program, *value);
            if (memory == nullptr || memory->coherent || memory->gpuDescriptor) return false;
            if (address == AddressAccess::Read && !memory->planningOnly) return false;
        }
    }
    return true;
}

std::optional<Loop> FindLoop(const Graph& graph, IrBlock* header) {
    const auto& term = graph.Info(header).terminator;
    if (!term.loopHeader || term.mergeBlock == InvalidControlFlowId || term.continueBlock == InvalidControlFlowId ||
        header->Predecessors().size() != 2u) return std::nullopt;
    Loop loop;
    loop.header = header;
    for (auto* predecessor : header->Predecessors()) {
        if (graph.Dominates(header, predecessor)) {
            if (loop.latch != nullptr) return std::nullopt;
            loop.latch = predecessor;
        } else {
            if (loop.preheader != nullptr) return std::nullopt;
            loop.preheader = predecessor;
        }
    }
    if (loop.latch == nullptr || loop.preheader == nullptr || loop.latch == header) return std::nullopt;
    loop.blocks.insert(header);
    std::vector<IrBlock*> pending{loop.latch};
    while (!pending.empty()) {
        auto* block = pending.back();
        pending.pop_back();
        if (!loop.blocks.insert(block).second) continue;
        if (!graph.Dominates(header, block)) return std::nullopt;
        for (auto* predecessor : block->Predecessors()) pending.push_back(predecessor);
    }
    if (loop.blocks.contains(loop.preheader) || !IsSafeLoop(graph.program, loop)) return std::nullopt;
    return loop;
}

bool IsInvariant(const Graph& graph, const Loop& loop, const std::unordered_set<IrValue*>& preceding, const IrValue& value) {
    const auto opcode = value.Opcode();
    if (opcode == IrOpcode::ReadConstBuffer) {
        const auto* memory = Memory(graph.program, value);
        const auto& buffers = graph.program.Info().buffers;
        if (memory == nullptr || memory->kind != ResourceKind::ScalarBuffer || memory->coherent || memory->gpuDescriptor ||
            memory->planningOnly || memory->resource >= buffers.size()) return false;
        const auto& buffer = buffers[memory->resource];
        if (!buffer.read || buffer.written || buffer.atomic) return false;
    } else if (AddressOpcodeInfoOf(opcode).access == AddressAccess::Read) {
        const auto* memory = Memory(graph.program, value);
        if (opcode != IrOpcode::LoadAddressU32 || memory == nullptr || !memory->planningOnly || memory->coherent || memory->gpuDescriptor) return false;
    } else if (opcode != IrOpcode::ReadConst && opcode != IrOpcode::GetSrtResource &&
        opcode != IrOpcode::GetBufferResource && opcode != IrOpcode::GetAddressResource &&
        !IsSchedulingOnly(opcode) && !IsPure(opcode)) {
        return false;
    }
    for (auto* argument : value.Arguments()) {
        argument = argument->Resolve();
        if (argument->HasImmediate() || preceding.contains(argument)) continue;
        const auto* parent = argument->Parent();
        if (parent == nullptr || loop.blocks.contains(argument->Parent()) || !graph.Dominates(parent, loop.header)) return false;
    }
    return true;
}

struct Candidate {
    Loop loop;
    IrBlock* block = nullptr;
    std::vector<IrValue*> prefix;
    std::vector<IrValue*> outputs;
    std::vector<IrValue*> references;
    std::uint32_t loads = 0u;
};

std::optional<Candidate> FindCandidate(const Graph& graph, const Loop& loop, IrBlock* block) {
    if (block == loop.header || block == loop.latch || !graph.Dominates(block, loop.latch)) return std::nullopt;
    const auto& headerTerm = graph.Info(loop.header).terminator;
    if (graph.targets.at(headerTerm.continueBlock) == block || graph.Info(block).terminator.loopHeader) return std::nullopt;
    std::unordered_set<IrBlock*> visited;
    std::vector<IrBlock*> pending(block->Successors().begin(), block->Successors().end());
    while (!pending.empty()) {
        auto* next = pending.back();
        pending.pop_back();
        if (next == loop.header || !loop.blocks.contains(next)) continue;
        if (next == block) return std::nullopt;
        if (!visited.insert(next).second) continue;
        for (auto* successor : next->Successors()) pending.push_back(successor);
    }
    Candidate candidate;
    candidate.loop = loop;
    candidate.block = block;
    std::unordered_set<IrValue*> preceding;
    for (auto* value : block->Instructions()) {
        if (!IsInvariant(graph, loop, preceding, *value)) break;
        if (candidate.prefix.size() == MaxInstructions) return std::nullopt;
        candidate.prefix.push_back(value);
        preceding.insert(value);
        candidate.loads += value->Opcode() == IrOpcode::ReadConstBuffer ? 1u : 0u;
    }
    if (candidate.prefix.size() < MinInstructions) return std::nullopt;
    for (auto* value : candidate.prefix) {
        bool exported = false;
        for (const auto& use : value->OperandUses()) {
            if (preceding.contains(use.user)) continue;
            const bool planning = AddressOpcodeInfoOf(value->Opcode()).access == AddressAccess::Read && Memory(graph.program, *value)->planningOnly;
            if (planning && use.user->Opcode() == IrOpcode::ReferenceU32 && use.user->Parent() == block) {
                candidate.references.push_back(use.user);
                continue;
            }
            if (use.user->Parent() != block || value->Type() != IrType::U32) return std::nullopt;
            exported = true;
        }
        for (const auto& info : graph.program.Metadata().blockInfo) {
            if (info.condition == value || info.indirectTarget == value) return std::nullopt;
        }
        if (exported) candidate.outputs.push_back(value);
    }
    if (candidate.outputs.empty() || candidate.outputs.size() > MaxOutputs) return std::nullopt;
    return candidate;
}

void ReplacePhiPredecessor(IrProgram& program, IrBlock& block, IrBlock* previous, IrBlock* replacement) {
    const std::vector<IrValue*> instructions(block.Instructions().begin(), block.Instructions().end());
    for (auto* phi : instructions) {
        if (!phi->IsPhi()) break;
        auto& next = program.CreateValue(IrOpcode::Phi, phi->Type(), phi->Flags<std::uint64_t>());
        for (std::size_t index = 0u; index < phi->ArgumentCount(); ++index) {
            auto* predecessor = phi->PhiBlock(index);
            next.AddPhiOperand(predecessor == previous ? replacement : predecessor, phi->Argument(index));
        }
        block.InsertInstructionBefore(phi, &next);
        for (auto& info : program.Metadata().blockInfo) {
            if (info.condition == phi) info.condition = &next;
            if (info.indirectTarget == phi) info.indirectTarget = &next;
        }
        phi->ReplaceAllUsesWith(&next);
        phi->ReplaceUsesWith(&next, true);
        block.RemoveInstruction(phi);
    }
}

LoopInvariantCacheStats Apply(IrProgram& program, const Graph& graph, const Candidate& candidate) {
    auto* guard = candidate.block;
    const auto position = graph.indices.at(guard);
    const auto originalInfo = graph.Info(guard);
    std::uint32_t nextId = 0u;
    for (const auto& info : program.Metadata().blockInfo) nextId = std::max(nextId, info.id);
    if (nextId >= InvalidControlFlowId - 2u) return {};
    auto& compute = program.CreateBlock();
    auto& tail = program.CreateBlock();
    IrBuilder ir(program);
    auto& valid = program.CreateValue(IrOpcode::Phi, IrType::Bool);
    valid.AddPhiOperand(candidate.loop.preheader, &ir.ConstantBool(false));
    valid.AddPhiOperand(candidate.loop.latch, &ir.ConstantBool(true));
    candidate.loop.header->InsertInstructionBefore(nullptr, &valid);
    std::unordered_set<IrValue*> prefix(candidate.prefix.begin(), candidate.prefix.end());
    for (auto* output : candidate.outputs) {
        auto& cached = program.CreateValue(IrOpcode::Phi, IrType::U32);
        candidate.loop.header->InsertInstructionBefore(nullptr, &cached);
        auto& joined = program.CreateValue(IrOpcode::Phi, IrType::U32);
        joined.AddPhiOperand(guard, &cached);
        joined.AddPhiOperand(&compute, output);
        tail.AppendInstruction(&joined);
        cached.AddPhiOperand(candidate.loop.preheader, &ir.Constant(0u));
        cached.AddPhiOperand(candidate.loop.latch, &joined);
        const auto uses = output->OperandUses();
        for (const auto& use : uses) {
            if (use.user == &joined || prefix.contains(use.user)) continue;
            use.user->ReplaceArgument(use.operand, &joined);
        }
    }
    for (auto* value : candidate.prefix) {
        guard->RemoveInstruction(value);
        compute.AppendInstruction(value);
    }
    for (auto* value : candidate.references) {
        guard->RemoveInstruction(value);
        compute.AppendInstruction(value);
    }
    const std::vector<IrValue*> remaining(guard->Instructions().begin(), guard->Instructions().end());
    for (auto* value : remaining) {
        guard->RemoveInstruction(value);
        tail.AppendInstruction(value);
    }
    ir.SetInsertionPoint(*guard);
    (void)ir.Emit(IrOpcode::Reference, IrType::Void, {&valid});
    const auto successors = guard->Successors();
    guard->Successors().clear();
    for (auto* successor : successors) {
        auto& predecessors = successor->Predecessors();
        std::replace(predecessors.begin(), predecessors.end(), guard, &tail);
        tail.AddSuccessor(successor);
        ReplacePhiPredecessor(program, *successor, guard, &tail);
    }
    guard->AddBranch(&compute);
    guard->AddBranch(&tail);
    compute.AddBranch(&tail);
    auto& guardInfo = program.Metadata().blockInfo[position];
    guardInfo.condition = &valid;
    guardInfo.indirectTarget = nullptr;
    guardInfo.terminator = {};
    guardInfo.terminator.kind = TerminatorKind::ConditionalBranch;
    guardInfo.terminator.condition = BranchCondition::ScalarInstruction;
    guardInfo.terminator.trueBlock = nextId + 2u;
    guardInfo.terminator.falseBlock = nextId + 1u;
    guardInfo.terminator.mergeBlock = nextId + 2u;
    BlockInfo computeInfo;
    computeInfo.id = nextId + 1u;
    computeInfo.startPc = originalInfo.startPc;
    computeInfo.endPc = originalInfo.endPc;
    computeInfo.terminator.kind = TerminatorKind::Branch;
    computeInfo.terminator.trueBlock = nextId + 2u;
    auto tailInfo = originalInfo;
    tailInfo.id = nextId + 2u;
    program.Metadata().blockInfo.push_back(std::move(computeInfo));
    program.Metadata().blockInfo.push_back(std::move(tailInfo));
    program.BlockOrder().push_back(&compute);
    program.BlockOrder().push_back(&tail);
    auto& storage = program.Blocks();
    std::rotate(storage.begin() + position + 1u, storage.end() - 2u, storage.end());
    auto& order = program.BlockOrder();
    std::rotate(order.begin() + position + 1u, order.end() - 2u, order.end());
    auto& info = program.Metadata().blockInfo;
    std::rotate(info.begin() + position + 1u, info.end() - 2u, info.end());
    ValidateProgram(program, true);
    return {1u, static_cast<std::uint32_t>(candidate.outputs.size()), candidate.loads, static_cast<std::uint32_t>(candidate.prefix.size())};
}

}

LoopInvariantCacheStats LoopInvariantCache::Cache(IrProgram& program) const {
    if (program.Resources().stage != IrShaderStage::Compute || program.BlockOrder().empty() ||
        program.BlockOrder().size() > MaxBlocks || !program.Resources().guardedSrtSlots.empty() ||
        !Detail::ComputeGuardedFlatSlots(program.Resources()).empty() ||
        program.Metadata().cfgFailureKind != FailureKind::None) return {};
    ValidateProgram(program, true);
    const Graph graph(program);
    std::optional<Candidate> best;
    for (auto* header : program.BlockOrder()) {
        const auto loop = FindLoop(graph, header);
        if (!loop) continue;
        for (auto* block : program.BlockOrder()) {
            if (!loop->blocks.contains(block)) continue;
            auto candidate = FindCandidate(graph, *loop, block);
            if (candidate && (!best || candidate->prefix.size() > best->prefix.size())) best = std::move(candidate);
        }
    }
    return best ? Apply(program, graph, *best) : LoopInvariantCacheStats{};
}

}
