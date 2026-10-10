#include "SpirvBackend/IndependentComputeHalves.hpp"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ShaderRecompiler {
namespace {
using ValueSet = std::unordered_set<const IrValue*>;

bool Immediate(const IrValue* value, std::uint32_t bits) {
    value = value->Resolve();
    return value->Type() == IrType::U32 && value->HasImmediate() && value->ImmediateU32() == bits;
}
bool True(const IrValue* value) {
    value = value->Resolve();
    return value->Type() == IrType::U1 && value->HasImmediate() && value->ImmediateBool();
}
bool Allowed(const IrValue& value) {
    const auto op = value.Opcode();
    if (SharedAccessOf(op) != SharedAccess::None) return false;
    const auto buffer = BufferAccessOf(op);
    const auto address = AddressOpcodeInfoOf(op).access;
    const auto image = ImageOpcodeInfoOf(op).access;
    if (buffer != BufferAccess::None && op != IrOpcode::ReadConstBuffer) return false;
    if (address != AddressAccess::None && address != AddressAccess::Read) return false;
    if (image != ImageAccess::None && op != IrOpcode::ImageRead && op != IrOpcode::ImageWrite) return false;
    switch (op) {
    case IrOpcode::Ballot: case IrOpcode::LaneId: case IrOpcode::ReadFirstLane:
    case IrOpcode::ReadLane: case IrOpcode::WriteLane: case IrOpcode::DppMoveU32:
    case IrOpcode::DppUpdateU32: case IrOpcode::Permlane16U32: case IrOpcode::PermuteU32:
    case IrOpcode::BpermuteU32: case IrOpcode::SwizzleU32:
    case IrOpcode::ShaderClock: case IrOpcode::RealtimeClock:
    case IrOpcode::UndefU1: case IrOpcode::UndefU8: case IrOpcode::UndefU16:
    case IrOpcode::UndefU32: case IrOpcode::UndefU64:
    case IrOpcode::GetAttribute: case IrOpcode::GetInterpolationParameter:
    case IrOpcode::GetInterpolationParameterF16: case IrOpcode::GetTessellationAttribute:
    case IrOpcode::MeshDrawParameter: case IrOpcode::MeshArgument: case IrOpcode::TessellationBase:
    case IrOpcode::GetScalarRegister: case IrOpcode::GetVectorRegister: case IrOpcode::GetGotoVariable:
    case IrOpcode::GetThreadBitScalarRegister: case IrOpcode::GetScalarMaskTag:
    case IrOpcode::GetExec: case IrOpcode::GetExecLo: case IrOpcode::GetExecHi:
    case IrOpcode::GetVcc: case IrOpcode::GetVccLo: case IrOpcode::GetVccHi:
    case IrOpcode::GetScc: case IrOpcode::GetM0: case IrOpcode::GetRegister:
        return false;
    case IrOpcode::GetBuiltin:
        return value.ArgumentCount() == 2u &&
            (Immediate(value.Argument(0), static_cast<std::uint32_t>(StageInputKind::WorkgroupId)) ||
             Immediate(value.Argument(0), static_cast<std::uint32_t>(StageInputKind::LocalInvocationId)));
    case IrOpcode::Reference: case IrOpcode::ReferenceU32: case IrOpcode::Waitcnt:
    case IrOpcode::InstPrefetch: case IrOpcode::ControlNop: case IrOpcode::ImageWrite:
        return true;
    case IrOpcode::Identity: case IrOpcode::Phi:
    case IrOpcode::BitCastF32U32: case IrOpcode::BitCastU32F32: case IrOpcode::BitFieldUExtract:
    case IrOpcode::BitwiseAnd32: case IrOpcode::BitwiseOr32: case IrOpcode::BitwiseXor32:
    case IrOpcode::CompositeConstructU32x4: case IrOpcode::CompositeExtractU32x2:
    case IrOpcode::CompositeExtractU32x4: case IrOpcode::CompositeExtractU64:
    case IrOpcode::ConvertF32S32: case IrOpcode::ConvertF32U32: case IrOpcode::ConvertS32F32: case IrOpcode::ConvertU32F32:
    case IrOpcode::FPAbs32: case IrOpcode::FPAdd32: case IrOpcode::FPCos: case IrOpcode::FPFract32:
    case IrOpcode::FPIsNan32: case IrOpcode::FPMad32: case IrOpcode::FPMax32: case IrOpcode::FPMin32: case IrOpcode::FPMul32:
    case IrOpcode::FPOrdEqual32: case IrOpcode::FPOrdGreaterThan32: case IrOpcode::FPOrdGreaterThanEqual32:
    case IrOpcode::FPOrdLessThanEqual32: case IrOpcode::FPRecip32: case IrOpcode::FPRecipSqrt32:
    case IrOpcode::FPSaturate32: case IrOpcode::FPSin: case IrOpcode::FPSqrt: case IrOpcode::FPSub32: case IrOpcode::FPTrunc32:
    case IrOpcode::F32ProductIsTiny: case IrOpcode::SelectF32: case IrOpcode::SelectU32:
    case IrOpcode::IAdd32: case IrOpcode::IAdd64: case IrOpcode::IAddCarry32: case IrOpcode::IEqual32:
    case IrOpcode::IMul32: case IrOpcode::INotEqual32: case IrOpcode::ISub32:
    case IrOpcode::LogicalAnd: case IrOpcode::LogicalNot: case IrOpcode::LogicalOr:
    case IrOpcode::SLessThan32: case IrOpcode::ShiftLeftLogical32: case IrOpcode::ShiftRightLogical32:
    case IrOpcode::UGreaterThan32: case IrOpcode::ULessThan32: case IrOpcode::UMax32: case IrOpcode::UMulHi:
    case IrOpcode::GetAddressResource: case IrOpcode::GetBufferResource: case IrOpcode::GetImageResource:
    case IrOpcode::GetShaderBase: case IrOpcode::GetSrtResource: case IrOpcode::GetUserData:
    case IrOpcode::MakeImageAddress: case IrOpcode::ImageRead: case IrOpcode::ReadConst:
    case IrOpcode::ReadConstBuffer: case IrOpcode::LoadAddressU32:
        return true;
    default:
        return false;
    }
}
bool Reads(const IrValue& value) {
    const auto op = value.Opcode();
    return AddressOpcodeInfoOf(op).access == AddressAccess::Read ||
        BufferAccessOf(op) == BufferAccess::Read || ImageOpcodeInfoOf(op).access == ImageAccess::Read ||
        op == IrOpcode::ReadConst || op == IrOpcode::ReadConstBuffer;
}

struct Graph {
    const IrProgram& program;
    std::unordered_map<std::uint32_t, const IrBlock*> ids;
    std::unordered_map<const IrBlock*, const BlockInfo*> infos;
    explicit Graph(const IrProgram& p) : program(p) {
        if (p.BlockOrder().size() != p.Metadata().blockInfo.size()) return;
        for (std::size_t i = 0; i < p.BlockOrder().size(); ++i) {
            if (!ids.emplace(p.Metadata().blockInfo[i].id, p.BlockOrder()[i]).second) { ids.clear(); infos.clear(); return; }
            infos.emplace(p.BlockOrder()[i], &p.Metadata().blockInfo[i]);
        }
    }
    const IrBlock* Target(std::uint32_t id) const {
        const auto found = ids.find(id); return found == ids.end() ? nullptr : found->second;
    }
};

struct Diamond { const IrBlock* body; const IrBlock* merge; const IrValue* mask; };

bool SkipDiamond(const Graph& graph, const IrBlock* branch, Diamond& out) {
    const auto& info = *graph.infos.at(branch);
    const auto& term = info.terminator;
    if (term.condition != BranchCondition::ExecZero || term.loopHeader ||
        (term.mergeBlock != InvalidControlFlowId && term.mergeBlock != term.trueBlock) || info.condition == nullptr) return false;
    const auto* condition = info.condition->Resolve();
    out.mask = condition->Opcode() == IrOpcode::LogicalNot && condition->ArgumentCount() == 1u
        ? condition->Argument(0)->Resolve() : nullptr;
    out.body = graph.Target(term.falseBlock); out.merge = graph.Target(term.trueBlock);
    if (out.body == nullptr || out.merge == nullptr || out.body == out.merge || out.body == branch || out.merge == branch) return false;
    const auto& bodyTerm = graph.infos.at(out.body)->terminator;
    if (bodyTerm.kind != TerminatorKind::Branch || bodyTerm.trueBlock != term.trueBlock || bodyTerm.loopHeader ||
        out.body->Predecessors().size() != 1u || out.body->Predecessors()[0] != branch || out.merge->Predecessors().size() != 2u) return false;
    for (const auto* value : out.body->Instructions()) {
        if (value->IsPhi() || Reads(*value) || IrOpcodeHasSideEffects(value->Opcode()) || !Allowed(*value)) return false;
        for (const auto& use : value->OperandUses()) {
            if (use.user->Parent() == out.body) continue;
            if (use.user->Parent() != out.merge || !use.user->IsPhi()) return false;
        }
    }
    for (const auto* phi : out.merge->Instructions()) {
        if (!phi->IsPhi()) continue;
        if (phi->ArgumentCount() != 2u) return false;
        const IrValue* bypass = nullptr; const IrValue* fromBody = nullptr;
        for (std::size_t i = 0; i < 2u; ++i) {
            if (phi->PhiBlock(i) == branch) bypass = phi->Argument(i)->Resolve();
            else if (phi->PhiBlock(i) == out.body) fromBody = phi->Argument(i)->Resolve();
            else return false;
        }
        if (bypass == nullptr || fromBody == nullptr) return false;
        ValueSet seen;
        while (fromBody != bypass) {
            if (!seen.insert(fromBody).second || fromBody->Parent() != out.body ||
                fromBody->Opcode() != IrOpcode::SelectU32 || fromBody->ArgumentCount() != 3u) return false;
            const auto* mask = fromBody->Argument(0)->Resolve();
            if (out.mask == nullptr) {
                if (mask->Opcode() != IrOpcode::LogicalNot || mask->ArgumentCount() != 1u ||
                    mask->Argument(0)->Resolve() != condition) return false;
                out.mask = mask;
            } else if (mask != out.mask) return false;
            fromBody = fromBody->Argument(2)->Resolve();
        }
    }
    return out.mask != nullptr;
}

ValueSet UniformValues(const IrProgram& program, const std::vector<Diamond>& diamonds) {
    ValueSet varying;
    for (const auto& diamond : diamonds) for (const auto* value : diamond.merge->Instructions()) if (value->IsPhi()) varying.insert(value);
    for (const auto* block : program.BlockOrder()) for (const auto* value : block->Instructions()) {
        if (value->Opcode() == IrOpcode::ImageRead ||
            (value->Opcode() == IrOpcode::GetBuiltin && !Immediate(value->Argument(0), static_cast<std::uint32_t>(StageInputKind::WorkgroupId)))) varying.insert(value);
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto* block : program.BlockOrder()) for (const auto* value : block->Instructions()) {
            if (varying.contains(value) || value->Opcode() == IrOpcode::GetUserData || value->Opcode() == IrOpcode::GetShaderBase) continue;
            for (const auto* argument : value->Arguments()) {
                const auto* source = argument->Resolve();
                if (varying.contains(source)) { varying.insert(value); changed = true; break; }
            }
        }
    }
    ValueSet uniform;
    for (const auto* block : program.BlockOrder()) for (const auto* value : block->Instructions()) if (!varying.contains(value)) uniform.insert(value);
    return uniform;
}
bool Uniform(const IrValue* value, const ValueSet& uniform) {
    value = value->Resolve(); return value->HasImmediate() || uniform.contains(value);
}
bool ScalarSharedSource(IrOpcode op) {
    switch (op) {
    case IrOpcode::GetUserData: case IrOpcode::GetShaderBase: case IrOpcode::ReadConst:
    case IrOpcode::ReadConstBuffer: case IrOpcode::GetSrtResource:
    case IrOpcode::GetBufferResource: case IrOpcode::GetAddressResource:
    case IrOpcode::GetImageResource: case IrOpcode::LoadAddressU32:
        return true;
    default:
        return false;
    }
}
bool LocalOffset(const IrValue* value, std::uint32_t axis, const ValueSet& uniform) {
    value = value->Resolve();
    if (value->Opcode() == IrOpcode::GetBuiltin) return Immediate(value->Argument(0), static_cast<std::uint32_t>(StageInputKind::LocalInvocationId)) && Immediate(value->Argument(1), axis);
    if (value->Opcode() != IrOpcode::IAdd32 || value->ArgumentCount() != 2u) return false;
    return (Uniform(value->Argument(0), uniform) && LocalOffset(value->Argument(1), axis, uniform)) ||
           (Uniform(value->Argument(1), uniform) && LocalOffset(value->Argument(0), axis, uniform));
}
}

const IrValue* IndependentComputeHalfStore(const IrProgram& program, const ShaderComputeInputInfo& input) {
    if (program.Resources().stage != IrShaderStage::Compute || program.WaveSize() != 64u || input.hostSubgroupSize != 32u ||
        input.waveSize != 64u || input.threadsNum[0] != 8u || input.threadsNum[1] != 8u || input.threadsNum[2] != 1u ||
        input.partialGroups || input.tgSizeEn || input.ldsSizeDwords != 0u || input.scratchSizeDwords != 0u) return nullptr;
    if (!program.Resources().guardedSrtSlots.empty()) return nullptr;
    Graph graph(program);
    if (graph.infos.size() != program.BlockOrder().size() || graph.infos.empty()) return nullptr;
    const IrValue* store = nullptr;
    for (const auto* block : program.BlockOrder()) for (const auto* value : block->Instructions()) {
        if (!Allowed(*value)) return nullptr;
        if (value->Opcode() == IrOpcode::ImageWrite) { if (store != nullptr) return nullptr; store = value; }
        const auto access = AddressOpcodeInfoOf(value->Opcode()).access;
        if (access != AddressAccess::None) {
            const auto& memory = program.Resources().memoryInfo.at(value->Flags<MemoryFlags>().index);
            if (memory.kind != ResourceKind::ScalarAddress || !memory.planningOnly) return nullptr;
        }
        if (value->Opcode() == IrOpcode::ReadConstBuffer) {
            const auto& memory = program.Resources().memoryInfo.at(value->Flags<MemoryFlags>().index);
            if (memory.kind != ResourceKind::ScalarBuffer || memory.coherent || (!memory.planningOnly && memory.gpuDescriptor)) return nullptr;
        }
    }
    if (store == nullptr || store->ArgumentCount() != 4u || !True(store->Argument(3))) return nullptr;
    const auto* finalBlock = store->Parent();
    if (graph.infos.at(finalBlock)->terminator.kind != TerminatorKind::Return || !finalBlock->Successors().empty()) return nullptr;
    bool afterStore = false;
    for (const auto* value : finalBlock->Instructions()) {
        if (value == store) { afterStore = true; continue; }
        if (afterStore && Reads(*value)) return nullptr;
    }
    std::vector<Diamond> diamonds;
    std::unordered_set<const IrBlock*> skipBranches;
    for (const auto* block : program.BlockOrder()) {
        const auto& term = graph.infos.at(block)->terminator;
        if (block == finalBlock) continue;
        if (term.kind == TerminatorKind::Branch) { if (graph.Target(term.trueBlock) == nullptr) return nullptr; }
        else if (term.kind == TerminatorKind::ConditionalBranch) {
            if (graph.Target(term.trueBlock) == nullptr || graph.Target(term.falseBlock) == nullptr || graph.infos.at(block)->condition == nullptr) return nullptr;
            Diamond diamond{};
            if (SkipDiamond(graph, block, diamond)) { diamonds.push_back(diamond); skipBranches.insert(block); }
        } else return nullptr;
    }
    const auto uniform = UniformValues(program, diamonds);
    for (const auto* block : program.BlockOrder()) for (const auto* value : block->Instructions()) {
        if (!ScalarSharedSource(value->Opcode())) continue;
        if (value->Opcode() == IrOpcode::GetUserData) {
            if (value->ArgumentCount() != 1u) return nullptr;
            const auto* reg = value->Argument(0)->Resolve();
            if (reg->Type() != IrType::ScalarReg || reg->Opcode() != IrOpcode::Void || reg->Parent() != nullptr ||
                reg->ArgumentCount() != 0u || reg->Register().bank != RegisterBank::UserData || reg->Register().index >= NumScalarRegs) return nullptr;
            continue;
        }
        for (const auto* argument : value->Arguments()) if (!Uniform(argument, uniform)) return nullptr;
    }
    for (const auto* block : program.BlockOrder()) {
        const auto& info = *graph.infos.at(block);
        if (info.terminator.kind == TerminatorKind::ConditionalBranch && !skipBranches.contains(block) && !Uniform(info.condition, uniform)) return nullptr;
        if (info.terminator.gotoVariable != InvalidControlFlowId || info.indirectTarget != nullptr) return nullptr;
    }
    std::unordered_set<const IrBlock*> reachable, reachesFinal;
    std::vector<const IrBlock*> pending{program.BlockOrder().front()};
    while (!pending.empty()) { const auto* b = pending.back(); pending.pop_back(); if (!reachable.insert(b).second) continue; for (const auto* s : b->Successors()) pending.push_back(s); }
    pending.push_back(finalBlock);
    while (!pending.empty()) { const auto* b = pending.back(); pending.pop_back(); if (!reachesFinal.insert(b).second) continue; for (const auto* p : b->Predecessors()) pending.push_back(p); }
    if (reachable.size() != graph.infos.size() || reachesFinal.size() != graph.infos.size()) return nullptr;
    const auto* imageHandle = store->Argument(0)->Resolve();
    const auto* coordinates = store->Argument(1)->Resolve();
    if (!Uniform(imageHandle, uniform) || coordinates->Opcode() != IrOpcode::MakeImageAddress || coordinates->ArgumentCount() < 2u ||
        !LocalOffset(coordinates->Argument(0), 0u, uniform) || !LocalOffset(coordinates->Argument(1), 1u, uniform)) return nullptr;
    const auto& memory = program.Resources().memoryInfo.at(store->Flags<MemoryFlags>().index);
    const auto& image = program.Info().images.at(memory.resource);
    const bool array = memory.imageDimension == RdnaImageDimension::Dim2DArray;
    if ((!array && memory.imageDimension != RdnaImageDimension::Dim2D) || memory.imageAddressComponents != (array ? 3u : 2u) || memory.imageSampleFlags != 0u || memory.imageHasMip || memory.imageByElements != 0u ||
        image.dimension != memory.imageDimension || image.mipMode != ImageMipMode::None || image.indirectRoot != ImageResource::NoIndirectImage || image.atomic ||
        (array && !Uniform(coordinates->Argument(2), uniform))) return nullptr;
    return store;
}

}
