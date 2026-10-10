#include "IntermediateRepresentation/IrBuilder.hpp"
#include "SpirvBackend/IndependentComputeHalves.hpp"
#include <iostream>
#include <stdexcept>
#include <string>

using namespace ShaderRecompiler;
namespace {
void Require(bool condition, const char* name) {
    if (!condition) throw std::runtime_error(std::string("independent compute halves: ") + name);
}
struct Fixture {
    IrProgram program;
    ShaderComputeInputInfo input;
    IrBuilder ir{program};
    IrBlock* entry;
    IrBlock* final;
    IrValue* handle;
    IrValue* address;
    IrValue* x;
    IrValue* y;
    IrValue* read;
    IrValue* old;
    IrValue* store;
    IrValue* bodyValue = nullptr;
    IrValue* masked = nullptr;
    IrValue* phi = nullptr;
    IrBlock* body = nullptr;
    explicit Fixture(bool array = true, bool explicitEntry = true) {
        input.threadsNum[0] = 8u; input.threadsNum[1] = 8u; input.threadsNum[2] = 1u;
        input.waveSize = 64u; input.hostSubgroupSize = 32u;
        program.Resources().stage = IrShaderStage::Compute; program.SetWaveSize(64u);
        entry = &program.CreateBlock(); final = &program.CreateBlock();
        if (explicitEntry) program.SetEntryBlock(*entry);
        program.BlockOrder() = {entry, final};
        entry->AddBranch(final);
        program.Metadata().blockInfo.resize(2u);
        Info(entry).id = entry->Id(); Info(entry).terminator.kind = TerminatorKind::Branch; Info(entry).terminator.trueBlock = final->Id();
        Info(final).id = final->Id(); Info(final).terminator.kind = TerminatorKind::Return;
        ir.SetInsertionPoint(*entry);
        auto& localX = Builtin(StageInputKind::LocalInvocationId, 0u);
        auto& localY = Builtin(StageInputKind::LocalInvocationId, 1u);
        auto& groupX = Builtin(StageInputKind::WorkgroupId, 0u);
        auto& groupY = Builtin(StageInputKind::WorkgroupId, 1u);
        x = &ir.IAdd(localX, groupX); y = &ir.IAdd(localY, groupY);
        handle = &ir.Emit(IrOpcode::GetImageResource, IrType::ImageResource, {&ir.Constant(1u), &ir.Constant(0u), &ir.Constant(0u), &ir.Constant(0u), &ir.Constant(0u), &ir.Constant(0u), &ir.Constant(0u), &ir.Constant(0u)});
        address = &ir.Emit(IrOpcode::MakeImageAddress, IrType::ImageAddress, {x, y, &ir.Constant(0u), &ir.Constant(0u), &ir.Constant(0u), &ir.Constant(0u), &ir.Constant(0u), &ir.Constant(0u), &ir.Constant(0u), &ir.Constant(0u), &ir.Constant(0u), &ir.Constant(0u), &ir.Constant(0u)});
        MemoryInfo memory{}; memory.kind = ResourceKind::Image; memory.imageDimension = array ? RdnaImageDimension::Dim2DArray : RdnaImageDimension::Dim2D; memory.imageAddressComponents = array ? 3u : 2u;
        program.Resources().memoryInfo = {memory, memory};
        ImageResource image{}; image.dimension = memory.imageDimension; image.resourceClass = ImageResourceClass::Storage; image.read = true; image.written = true;
        program.Info().images.push_back(image);
        read = &ir.Emit(IrOpcode::ImageRead, IrType::U32x4, {handle, address, &ir.ConstantBool(true)});
        read->SetFlags(MemoryFlags{0u, 0u});
        old = &ir.CompositeExtract(*read, 0u);
        ir.SetInsertionPoint(*final);
        auto& data = ir.Emit(IrOpcode::CompositeConstructU32x4, IrType::U32x4, {old, &ir.Constant(0u), &ir.Constant(0u), &ir.Constant(0u)});
        store = &ir.Emit(IrOpcode::ImageWrite, IrType::Void, {handle, address, &data, &ir.ConstantBool(true)});
        store->SetFlags(MemoryFlags{1u, 0u});
    }
    BlockInfo& Info(IrBlock* block) {
        for (std::size_t i = 0; i < program.BlockOrder().size(); ++i) if (program.BlockOrder()[i] == block) return program.Metadata().blockInfo.at(i);
        throw std::runtime_error("fixture block missing");
    }
    IrValue& Builtin(StageInputKind kind, std::uint32_t axis) {
        return ir.Emit(IrOpcode::GetBuiltin, IrType::U32, {&ir.Constant(static_cast<std::uint32_t>(kind)), &ir.Constant(axis)});
    }
    void Check(bool accepted, const char* name) {
        Require((IndependentComputeHalfStore(program, input) == store) == accepted, name);
    }
    void Diamond(bool foldedComplement = false) {
        auto& branch = program.CreateBlock(); body = &program.CreateBlock(); auto& merge = program.CreateBlock();
        program.BlockOrder() = {entry, &branch, body, &merge, final};
        program.Metadata().blockInfo.clear(); program.Metadata().blockInfo.resize(5u);
        entry->Successors().clear(); final->Predecessors().clear();
        entry->AddBranch(&branch); branch.AddBranch(&merge); branch.AddBranch(body); body->AddBranch(&merge); merge.AddBranch(final);
        for (auto* block : program.BlockOrder()) Info(block).id = block->Id();
        Info(entry).terminator.kind = TerminatorKind::Branch; Info(entry).terminator.trueBlock = branch.Id();
        Info(&branch).terminator.kind = TerminatorKind::ConditionalBranch; Info(&branch).terminator.condition = BranchCondition::ExecZero;
        Info(&branch).terminator.trueBlock = merge.Id(); Info(&branch).terminator.falseBlock = body->Id(); Info(&branch).terminator.mergeBlock = merge.Id();
        Info(body).terminator.kind = TerminatorKind::Branch; Info(body).terminator.trueBlock = merge.Id();
        Info(&merge).terminator.kind = TerminatorKind::Branch; Info(&merge).terminator.trueBlock = final->Id();
        Info(final).terminator.kind = TerminatorKind::Return;
        ir.SetInsertionPoint(branch);
        auto& compare = ir.INotEqual(*old, ir.Constant(0u));
        auto* mask = foldedComplement ? &ir.LogicalNot(compare) : &compare;
        Info(&branch).condition = foldedComplement ? &compare : &ir.LogicalNot(*mask);
        ir.SetInsertionPoint(*body);
        bodyValue = &ir.IAdd(*old, ir.Constant(1u)); masked = &ir.Select(*mask, *bodyValue, *old);
        phi = &program.CreateValue(IrOpcode::Phi, IrType::U32); merge.AppendInstruction(phi);
        phi->AddPhiOperand(&branch, old); phi->AddPhiOperand(body, masked);
        store->Argument(2)->ReplaceArgument(0u, phi);
        ir.SetInsertionPoint(*final);
    }
    void UniformLoop() {
        auto& header = program.CreateBlock(); auto& latch = program.CreateBlock();
        program.BlockOrder() = {entry, &header, &latch, final};
        program.Metadata().blockInfo.clear(); program.Metadata().blockInfo.resize(4u);
        entry->Successors().clear(); final->Predecessors().clear();
        entry->AddBranch(&header); header.AddBranch(&latch); header.AddBranch(final); latch.AddBranch(&header);
        for (auto* block : program.BlockOrder()) Info(block).id = block->Id();
        Info(entry).terminator.kind = TerminatorKind::Branch; Info(entry).terminator.trueBlock = header.Id();
        auto& term = Info(&header).terminator;
        term.kind = TerminatorKind::ConditionalBranch; term.condition = BranchCondition::SccNonZero;
        term.trueBlock = latch.Id(); term.falseBlock = final->Id(); term.loopHeader = true;
        term.mergeBlock = final->Id(); term.continueBlock = latch.Id();
        Info(&latch).terminator.kind = TerminatorKind::Branch; Info(&latch).terminator.trueBlock = header.Id();
        Info(final).terminator.kind = TerminatorKind::Return;
        auto& counter = program.CreateValue(IrOpcode::Phi, IrType::U32); header.AppendInstruction(&counter);
        ir.SetInsertionPoint(latch); auto& next = ir.IAdd(counter, ir.Constant(1u));
        counter.AddPhiOperand(entry, &ir.Constant(0u)); counter.AddPhiOperand(&latch, &next);
        ir.SetInsertionPoint(header); Info(&header).condition = &ir.ULessThan(counter, ir.Constant(3u));
        ir.SetInsertionPoint(*final);
    }
    void ScalarRead(bool varyingOffset, bool coherent = false, bool gpuDescriptor = false) {
        ir.SetInsertionPoint(*entry);
        auto& resource = ir.Emit(IrOpcode::GetBufferResource, IrType::BufferResource, {&ir.Constant(0u), &ir.Constant(0u), &ir.Constant(64u), &ir.Constant(0u)});
        MemoryInfo memory{}; memory.kind = ResourceKind::ScalarBuffer;
        memory.coherent = coherent; memory.gpuDescriptor = gpuDescriptor;
        const auto index = static_cast<std::uint32_t>(program.Resources().memoryInfo.size());
        program.Resources().memoryInfo.push_back(memory);
        BufferResource buffer{}; buffer.scalar = true; buffer.read = true; program.Info().buffers.push_back(buffer);
        auto& load = ir.Emit(IrOpcode::ReadConstBuffer, IrType::U32, {&resource, varyingOffset ? y : &ir.Constant(0u)});
        load.SetFlags(MemoryFlags{index, 0u}); store->Argument(2)->ReplaceArgument(0u, &load);
        ir.SetInsertionPoint(*final);
    }
};
void CheckCases() {
    Fixture base; base.Check(true, "aliased image read-prefix/single-injective-store should certify");
    Fixture plain2D(false); plain2D.Check(true, "2D output should certify");
    Fixture orderedEntry(true, false); orderedEntry.Check(true, "certificate must use the emitter's ordered entry block");
    Fixture diamond; diamond.Diamond(); diamond.Check(true, "fully remasked pure EXECz skip diamond should certify");
    Fixture folded; folded.Diamond(true); folded.Check(true, "exact complement retained in remask after branch double-not fold should certify");
    Fixture inferred; inferred.Diamond(true); inferred.Info(inferred.phi->PhiBlock(0u)).terminator.mergeBlock = InvalidControlFlowId; inferred.Check(true, "proved one-block diamond should not require a merge hint");
    Fixture wrongMerge; wrongMerge.Diamond(true); wrongMerge.Info(wrongMerge.phi->PhiBlock(0u)).terminator.mergeBlock = wrongMerge.entry->Id(); wrongMerge.Check(false, "a conflicting merge hint must reject the skip diamond");
    Fixture samePredicate; samePredicate.Diamond(true); samePredicate.masked->ReplaceArgument(0u, samePredicate.Info(samePredicate.phi->PhiBlock(0u)).condition); samePredicate.Check(false, "same branch/remask predicate is not a complement");
    Fixture otherPredicate; otherPredicate.Diamond(true); auto* otherBranch = otherPredicate.phi->PhiBlock(0u); otherPredicate.ir.SetInsertionPoint(*otherBranch); auto& otherCompare = otherPredicate.ir.INotEqual(*otherPredicate.old, otherPredicate.ir.Constant(1u)); auto& otherMask = otherPredicate.ir.LogicalNot(otherCompare); otherPredicate.masked->ReplaceArgument(0u, &otherMask); otherPredicate.Check(false, "independent complement SSA is not the branch complement");
    Fixture uniformLoop; uniformLoop.UniformLoop(); uniformLoop.Check(true, "uniform loop-back scalar counter should certify");
    Fixture scalar; scalar.ScalarRead(false); scalar.Check(true, "uniform noncoherent direct scalar read should certify");
    Fixture userData; userData.ir.SetInsertionPoint(*userData.entry); auto& user = userData.ir.GetUserData(static_cast<ScalarReg>(0u)); userData.store->Argument(2)->ReplaceArgument(0u, &user); userData.Check(true, "fixed GetUserData register-index metadata should certify");
    Fixture varyingScalar; varyingScalar.ScalarRead(true); varyingScalar.Check(false, "varying scalar offset would change low-half shared data");
    Fixture coherentScalar; coherentScalar.ScalarRead(false, true); coherentScalar.Check(false, "duplicated coherent scalar read accepted");
    Fixture bdaScalar; bdaScalar.ScalarRead(false, false, true); bdaScalar.Check(false, "BDA scalar descriptor accepted");
    Fixture varyingSrt; varyingSrt.ir.SetInsertionPoint(*varyingSrt.entry); auto& srt = varyingSrt.ir.Emit(IrOpcode::GetSrtResource, IrType::SrtResource, {}); auto& srtRead = varyingSrt.ir.Emit(IrOpcode::ReadConst, IrType::U32, {&srt, varyingSrt.y}); varyingSrt.store->Argument(2)->ReplaceArgument(0u, &srtRead); varyingSrt.Check(false, "varying flattened SRT offset accepted");
    Fixture varyingResource; varyingResource.ir.SetInsertionPoint(*varyingResource.entry); (void)varyingResource.ir.Emit(IrOpcode::GetBufferResource, IrType::BufferResource, {varyingResource.y, &varyingResource.ir.Constant(0u), &varyingResource.ir.Constant(64u), &varyingResource.ir.Constant(0u)}); varyingResource.Check(false, "varying low-half shared resource descriptor accepted");
    Fixture partial; partial.input.partialGroups = true; partial.Check(false, "partial group accepted");
    Fixture waveId; waveId.input.tgSizeEn = true; waveId.Check(false, "guest-visible wave id accepted");
    Fixture scratch; scratch.input.scratchSizeDwords = 1u; scratch.Check(false, "scratch accepted");
    Fixture lds; lds.input.ldsSizeDwords = 1u; lds.Check(false, "LDS accepted");
    Fixture wider; wider.input.threadsNum[0] = 16u; wider.Check(false, "multiple guest waves accepted");
    Fixture native64; native64.input.hostSubgroupSize = 64u; native64.Check(false, "non-32 host accepted");
    Fixture hiddenLane; hiddenLane.ir.SetInsertionPoint(*hiddenLane.entry); (void)hiddenLane.ir.Emit(IrOpcode::LaneId, IrType::U32, {}); hiddenLane.Check(false, "numeric lane ID accepted");
    Fixture explicitBarrier; explicitBarrier.ir.SetInsertionPoint(*explicitBarrier.entry); (void)explicitBarrier.ir.Emit(IrOpcode::Barrier, IrType::Void, {}); explicitBarrier.Check(false, "preexisting synchronization accepted");
    Fixture earlyExit; earlyExit.Info(earlyExit.entry).terminator.kind = TerminatorKind::Return; earlyExit.Check(false, "early return accepted");
    Fixture extraWrite; extraWrite.ir.SetInsertionPoint(*extraWrite.entry); auto& prior = extraWrite.ir.Emit(IrOpcode::ImageWrite, IrType::Void, {extraWrite.handle, extraWrite.address, extraWrite.read, &extraWrite.ir.ConstantBool(true)}); prior.SetFlags(MemoryFlags{1u, 0u}); extraWrite.Check(false, "write in read-prefix accepted");
    Fixture lateRead; auto& later = lateRead.ir.Emit(IrOpcode::ImageRead, IrType::U32x4, {lateRead.handle, lateRead.address, &lateRead.ir.ConstantBool(true)}); later.SetFlags(MemoryFlags{0u, 0u}); lateRead.Check(false, "read after first output store accepted");
    Fixture a16; a16.program.Resources().memoryInfo[1u].imageSampleFlags = RdnaImageSampleFlagA16; a16.Check(false, "A16 packed output coordinates accepted");
    Fixture guardedSrt; guardedSrt.program.Resources().guardedSrtSlots.push_back(0u); guardedSrt.Check(false, "SRT poison early-return path accepted");
    Fixture opaqueRay; auto& opaque = opaqueRay.program.CreateValue(IrOpcode::ImageBvhIntersectRay, IrType::U32x4); opaqueRay.entry->AppendInstruction(&opaque); opaqueRay.Check(false, "opaque hidden BDA read accepted");
    for (const bool planning : {false, true}) {
        Fixture rawAddress; rawAddress.ir.SetInsertionPoint(*rawAddress.entry);
        auto& resource = rawAddress.ir.Emit(IrOpcode::GetAddressResource, IrType::AddressResource, {&rawAddress.ir.Constant(1u), &rawAddress.ir.Constant(0u)});
        MemoryInfo addressMemory{}; addressMemory.kind = ResourceKind::ScalarAddress; addressMemory.planningOnly = planning;
        const auto index = static_cast<std::uint32_t>(rawAddress.program.Resources().memoryInfo.size()); rawAddress.program.Resources().memoryInfo.push_back(addressMemory);
        auto& load = rawAddress.ir.Emit(IrOpcode::LoadAddressU32, IrType::U32, {&resource, &rawAddress.ir.Constant(0u), &rawAddress.ir.Constant(0u), &rawAddress.ir.ConstantBool(true)}); load.SetFlags(MemoryFlags{index, 0u});
        rawAddress.Check(planning, "real BDA versus planning-only descriptor load classification");
    }
    Fixture conflicting; conflicting.address->ReplaceArgument(0u, &conflicting.ir.Constant(0u)); conflicting.Check(false, "non-injective output accepted");
    Fixture varyingOffset; varyingOffset.x->ReplaceArgument(1u, varyingOffset.old); varyingOffset.Check(false, "lane-varying output offset accepted");
    Fixture varyingLayer; varyingLayer.address->ReplaceArgument(2u, varyingLayer.old); varyingLayer.Check(false, "lane-varying array layer accepted");
    Fixture divergentRead; divergentRead.Diamond(); divergentRead.ir.SetInsertionPoint(*divergentRead.body); auto& bodyRead = divergentRead.ir.Emit(IrOpcode::ImageRead, IrType::U32x4, {divergentRead.handle, divergentRead.address, &divergentRead.ir.ConstantBool(true)}); bodyRead.SetFlags(MemoryFlags{0u, 0u}); divergentRead.Check(false, "memory in half-local skip body accepted");
    Fixture unmasked; unmasked.Diamond(); unmasked.store->Argument(2)->ReplaceArgument(0u, unmasked.bodyValue); unmasked.Check(false, "unmasked body live-out accepted");
    Fixture wrongBypass; wrongBypass.Diamond(); wrongBypass.masked->ReplaceArgument(2u, &wrongBypass.ir.Constant(99u)); wrongBypass.Check(false, "wrong false-arm bypass accepted");
    Fixture phiBranch; phiBranch.Diamond(); auto* merge = phiBranch.phi->Parent(); phiBranch.ir.SetInsertionPoint(*merge); phiBranch.Info(merge).terminator.kind = TerminatorKind::ConditionalBranch; phiBranch.Info(merge).terminator.condition = BranchCondition::SccNonZero; phiBranch.Info(merge).terminator.falseBlock = phiBranch.final->Id(); phiBranch.Info(merge).condition = &phiBranch.ir.INotEqual(*phiBranch.phi, phiBranch.ir.Constant(0u)); phiBranch.Check(false, "half-local merge phi controls scalar branch");
    Fixture varyingBranch; varyingBranch.Info(varyingBranch.entry).terminator.kind = TerminatorKind::ConditionalBranch; varyingBranch.Info(varyingBranch.entry).terminator.trueBlock = varyingBranch.final->Id(); varyingBranch.Info(varyingBranch.entry).terminator.falseBlock = varyingBranch.final->Id(); varyingBranch.ir.SetInsertionPoint(*varyingBranch.entry); varyingBranch.Info(varyingBranch.entry).condition = &varyingBranch.ir.INotEqual(*varyingBranch.old, varyingBranch.ir.Constant(0u)); varyingBranch.Check(false, "ordinary image-varying branch accepted");
}
}
int main() {
    try { CheckCases(); std::cout << "independent compute halves PASS\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
