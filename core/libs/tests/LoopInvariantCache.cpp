#include "IntermediateRepresentation/IrBuilder.hpp"
#include "Optimization/LoopInvariantCache.hpp"
#include "Optimization/ResourceMaterializer.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using namespace ShaderRecompiler;

namespace {

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(std::string("loop invariant cache: ") + message);
}

struct Fixture {
    IrProgram program;
    IrBuilder ir{program};
    IrBlock* entry;
    IrBlock* header;
    IrBlock* body;
    IrBlock* latch;
    IrBlock* exit;
    IrValue* counter;
    IrValue* handle;
    IrValue* consumer;
    std::vector<IrValue*> prefix;
    std::array<IrValue*, 9u> loads{};
    std::array<IrValue*, 3u> outputs{};

    Fixture() {
        program.Resources().stage = IrShaderStage::Compute;
        entry = &program.CreateBlock();
        header = &program.CreateBlock();
        body = &program.CreateBlock();
        latch = &program.CreateBlock();
        exit = &program.CreateBlock();
        program.SetEntryBlock(*entry);
        program.BlockOrder() = {entry, header, body, latch, exit};
        for (std::uint32_t index = 0u; index < 5u; ++index) {
            BlockInfo info;
            info.id = 10u + index;
            program.Metadata().blockInfo.push_back(info);
        }
        entry->AddBranch(header);
        header->AddBranch(body);
        header->AddBranch(exit);
        body->AddBranch(latch);
        latch->AddBranch(header);
        auto& infos = program.Metadata().blockInfo;
        infos[0].terminator.kind = TerminatorKind::Branch;
        infos[0].terminator.trueBlock = 11u;
        infos[1].terminator.kind = TerminatorKind::ConditionalBranch;
        infos[1].terminator.condition = BranchCondition::ScalarInstruction;
        infos[1].terminator.trueBlock = 12u;
        infos[1].terminator.falseBlock = 14u;
        infos[1].terminator.loopHeader = true;
        infos[1].terminator.mergeBlock = 14u;
        infos[1].terminator.continueBlock = 13u;
        infos[2].terminator.kind = TerminatorKind::Branch;
        infos[2].terminator.trueBlock = 13u;
        infos[3].terminator.kind = TerminatorKind::Branch;
        infos[3].terminator.trueBlock = 11u;
        ir.SetInsertionPoint(*entry);
        auto& offset = ir.GetUserData(static_cast<ScalarReg>(0u));
        auto& count = ir.GetUserData(static_cast<ScalarReg>(1u));
        counter = &program.CreateValue(IrOpcode::Phi, IrType::U32);
        header->AppendInstruction(counter);
        counter->AddPhiOperand(entry, &ir.Constant(0u));
        ir.SetInsertionPoint(*header);
        infos[1].condition = &ir.ULessThan(*counter, count);
        (void)ir.Emit(IrOpcode::Reference, IrType::Void, {infos[1].condition});
        ir.SetInsertionPoint(*body);
        handle = &ir.Emit(IrOpcode::GetBufferResource, IrType::BufferResource,
            {&ir.Constant(1u), &ir.Constant(2u), &ir.Constant(3u), &ir.Constant(4u)});
        BufferResource buffer;
        buffer.read = true;
        buffer.scalar = true;
        program.Info().buffers.push_back(buffer);
        constexpr std::array offsets{0u, 4u, 8u, 16u, 20u, 24u, 32u, 36u, 40u};
        for (std::uint32_t index = 0u; index < loads.size(); ++index) {
            MemoryInfo memory;
            memory.kind = ResourceKind::ScalarBuffer;
            memory.offset = offsets[index];
            program.Resources().memoryInfo.push_back(memory);
            loads[index] = &ir.Emit(IrOpcode::ReadConstBuffer, IrType::U32, {handle, &offset});
            loads[index]->SetFlags(MemoryFlags{index, 0x100u + index * 4u});
        }
        for (std::size_t component = 0u; component < outputs.size(); ++component) {
            IrValue* value = &ir.Emit(IrOpcode::BitwiseXor32, IrType::U32, {loads[component], loads[component + 3u]});
            value = &ir.Emit(IrOpcode::BitwiseXor32, IrType::U32, {value, loads[component + 6u]});
            for (std::uint32_t index = 0u; index < 64u; ++index) {
                value = &ir.Emit(IrOpcode::BitwiseXor32, IrType::U32, {value, &ir.Constant(0x80000001u)});
            }
            outputs[component] = value;
        }
        prefix.assign(body->Instructions().begin(), body->Instructions().end());
        consumer = &ir.IAdd(*outputs[0u], *counter);
        (void)ir.Emit(IrOpcode::ReferenceU32, IrType::Void, {consumer});
        (void)ir.Emit(IrOpcode::ReferenceU32, IrType::Void, {outputs[1u]});
        (void)ir.Emit(IrOpcode::ReferenceU32, IrType::Void, {outputs[2u]});
        ir.SetInsertionPoint(*latch);
        auto& next = ir.IAdd(*counter, ir.Constant(1u));
        counter->AddPhiOperand(latch, &next);
        ValidateProgram(program, true);
    }

    void Negative() {
        const auto before = ProgramToString(program);
        Require(LoopInvariantCache{}.Cache(program).cachedSlices == 0u, "unsupported graph was cached");
        Require(ProgramToString(program) == before, "a refused graph changed");
    }
};

struct Execution {
    std::vector<std::uint32_t> trace;
    std::uint32_t loads = 0u;
};

Execution Execute(const IrProgram& program, std::uint32_t iterations, const std::array<std::uint32_t, 9u>& words) {
    Execution result;
    std::unordered_map<const IrValue*, std::uint32_t> values;
    std::unordered_map<std::uint32_t, IrBlock*> targets;
    std::unordered_map<const IrBlock*, const BlockInfo*> infos;
    for (std::size_t index = 0u; index < program.BlockOrder().size(); ++index) {
        auto* block = program.BlockOrder()[index];
        targets.emplace(program.Metadata().blockInfo[index].id, block);
        infos.emplace(block, &program.Metadata().blockInfo[index]);
    }
    const auto read = [&](const IrValue* value) {
        return value->HasImmediate() ? static_cast<std::uint32_t>(value->ImmediateU64()) : values.at(value);
    };
    IrBlock* current = &program.EntryBlock();
    IrBlock* previous = nullptr;
    for (std::size_t visits = 0u; visits < 1000u; ++visits) {
        std::vector<std::pair<const IrValue*, std::uint32_t>> incoming;
        for (const auto* value : current->Instructions()) {
            if (!value->IsPhi()) break;
            bool found = false;
            for (std::size_t index = 0u; index < value->ArgumentCount(); ++index) {
                if (value->PhiBlock(index) != previous) continue;
                incoming.emplace_back(value, read(value->Argument(index)));
                found = true;
            }
            Require(found, "interpreter Phi edge is missing");
        }
        for (const auto& [value, bits] : incoming) values[value] = bits;
        for (const auto* value : current->Instructions()) {
            switch (value->Opcode()) {
                case IrOpcode::Phi:
                case IrOpcode::GetBufferResource:
                case IrOpcode::GetAddressResource:
                case IrOpcode::Waitcnt:
                case IrOpcode::Reference:
                    break;
                case IrOpcode::GetUserData:
                    values[value] = value->Argument(0u)->Register().index == 1u ? iterations : 0u;
                    break;
                case IrOpcode::ReadConstBuffer:
                    values[value] = words.at(value->Flags<MemoryFlags>().index) ^ read(value->Argument(1u));
                    ++result.loads;
                    break;
                case IrOpcode::LoadAddressU32:
                    Require(program.Resources().memoryInfo.at(value->Flags<MemoryFlags>().index).planningOnly, "interpreter encountered an executable address load");
                    values[value] = 0u;
                    break;
                case IrOpcode::ReferenceU32:
                    if (value->Argument(0u)->Opcode() != IrOpcode::LoadAddressU32) result.trace.push_back(read(value->Argument(0u)));
                    break;
                case IrOpcode::IAdd32:
                    values[value] = read(value->Argument(0u)) + read(value->Argument(1u));
                    break;
                case IrOpcode::BitwiseXor32:
                    values[value] = read(value->Argument(0u)) ^ read(value->Argument(1u));
                    break;
                case IrOpcode::ULessThan32:
                    values[value] = read(value->Argument(0u)) < read(value->Argument(1u));
                    break;
                default:
                    throw std::runtime_error("loop invariant cache interpreter encountered an unsupported opcode");
            }
        }
        const auto& info = *infos.at(current);
        if (info.terminator.kind == TerminatorKind::Return) return result;
        auto target = info.terminator.trueBlock;
        if (info.terminator.kind == TerminatorKind::ConditionalBranch && read(info.condition) == 0u) target = info.terminator.falseBlock;
        previous = current;
        current = targets.at(target);
    }
    throw std::runtime_error("loop invariant cache interpreter exceeded its block limit");
}

void CheckExecution() {
    Fixture fixture;
    constexpr std::array rawBits{0u, 0x80000000u, 0x7f800000u, 0xff800000u, 1u, 0x007fffffu, 0x7fc12345u, 0x7fa12345u};
    std::vector<Execution> expected;
    for (const auto bits : rawBits) {
        std::array<std::uint32_t, 9u> words{bits, bits ^ 0x80000000u, bits ^ 1u};
        for (const auto count : {0u, 1u, 7u}) expected.push_back(Execute(fixture.program, count, words));
    }
    std::vector<std::pair<IrOpcode, std::uint64_t>> original;
    for (const auto* value : fixture.prefix) original.emplace_back(value->Opcode(), value->Flags<std::uint64_t>());
    const auto memory = fixture.program.Resources().memoryInfo;
    const auto stats = LoopInvariantCache{}.Cache(fixture.program);
    Require(stats.cachedSlices == 1u && stats.cachedValues == 3u && stats.cachedLoads == 9u, "invariant slice was not cached completely");
    Require(stats.cachedInstructions == fixture.prefix.size(), "cached instruction count is inconsistent");
    Require(fixture.program.Resources().memoryInfo == memory, "memory offsets or attributes changed");
    const auto* compute = fixture.outputs.front()->Parent();
    std::size_t index = 0u;
    for (const auto* value : compute->Instructions()) {
        Require(value == fixture.prefix.at(index), "first-use instruction order changed");
        Require(value->Opcode() == original[index].first && value->Flags<std::uint64_t>() == original[index].second, "first-use instruction semantics changed");
        ++index;
    }
    std::size_t expectedIndex = 0u;
    for (const auto bits : rawBits) {
        std::array<std::uint32_t, 9u> words{bits, bits ^ 0x80000000u, bits ^ 1u};
        for (const auto count : {0u, 1u, 7u}) {
            const auto actual = Execute(fixture.program, count, words);
            Require(actual.trace == expected[expectedIndex++].trace, "cached output raw bits differ");
            Require(actual.loads == (count == 0u ? 0u : 9u), "loads did not stay on the first actual use");
        }
    }
    ValidateProgram(fixture.program, true);
}

void CheckPlanningOnlyReferences() {
    Fixture fixture;
    auto& address = fixture.program.CreateValue(IrOpcode::GetAddressResource, IrType::AddressResource);
    address.AddArgument(&fixture.ir.Constant(0u));
    address.AddArgument(&fixture.ir.Constant(0u));
    fixture.body->InsertInstructionBefore(fixture.prefix.front(), &address);
    MemoryInfo memory;
    memory.kind = ResourceKind::ScalarAddress;
    memory.planningOnly = true;
    const auto index = static_cast<std::uint32_t>(fixture.program.Resources().memoryInfo.size());
    fixture.program.Resources().memoryInfo.push_back(memory);
    auto& load = fixture.program.CreateValue(IrOpcode::LoadAddressU32, IrType::U32);
    load.SetFlags(MemoryFlags{index, 0x80u});
    load.AddArgument(&address);
    load.AddArgument(&fixture.ir.Constant(0u));
    load.AddArgument(&fixture.ir.Constant(0u));
    load.AddArgument(&fixture.ir.ConstantBool(true));
    fixture.body->InsertInstructionBefore(fixture.prefix.front(), &load);
    fixture.ir.SetInsertionPoint(*fixture.body);
    auto& reference = fixture.ir.Emit(IrOpcode::ReferenceU32, IrType::Void, {&load});
    Require(LoopInvariantCache{}.Cache(fixture.program).cachedValues == 3u, "planning-only reference became a cached output");
    Require(reference.Parent() == load.Parent(), "planning-only reference is outside its definition path");
    ValidateProgram(fixture.program, true);
}

void CheckSuccessorPhis() {
    Fixture fixture;
    auto& data = fixture.program.CreateValue(IrOpcode::Phi, IrType::U32);
    data.AddPhiOperand(fixture.body, fixture.consumer);
    fixture.latch->InsertInstructionBefore(nullptr, &data);
    auto& condition = fixture.program.CreateValue(IrOpcode::Phi, IrType::Bool);
    condition.AddPhiOperand(fixture.body, &fixture.ir.ConstantBool(true));
    fixture.latch->InsertInstructionBefore(nullptr, &condition);
    DescriptorSource source;
    source.dwordCount = 4u;
    source.dwords = {&data, &fixture.ir.Constant(0u), &fixture.ir.Constant(0u), &fixture.ir.Constant(0u)};
    fixture.program.Resources().descriptorSources.push_back(source);
    fixture.program.Resources().srtReads.push_back({&data, 0u});
    fixture.program.Resources().uniformFill.values[0u] = &data;
    ResourceBlock resourceBlock;
    resourceBlock.condition = &condition;
    fixture.program.Resources().controlFlow.push_back(resourceBlock);
    fixture.program.Metadata().dynamicReads.push_back(&data);
    fixture.program.Resources().srtPlanComplete = true;
    fixture.program.Resources().resourceTrackingComplete = true;
    fixture.ir.SetInsertionPoint(*fixture.latch);
    auto& reference = fixture.ir.Emit(IrOpcode::ReferenceU32, IrType::Void, {&data});
    (void)fixture.ir.Emit(IrOpcode::Reference, IrType::Void, {&condition});
    fixture.latch->AddBranch(fixture.exit);
    auto& latchInfo = fixture.program.Metadata().blockInfo[3u];
    latchInfo.condition = &condition;
    latchInfo.terminator.kind = TerminatorKind::ConditionalBranch;
    latchInfo.terminator.condition = BranchCondition::ScalarInstruction;
    latchInfo.terminator.falseBlock = 14u;
    const std::array<std::uint32_t, 9u> words{0x80000000u, 0x7fc12345u, 1u};
    const auto before = Execute(fixture.program, 7u, words);
    Require(LoopInvariantCache{}.Cache(fixture.program).cachedSlices == 1u, "successor Phi fixture was not cached");
    auto* replacement = reference.Argument(0u);
    Require(replacement->IsPhi() && replacement->PhiBlock(0u) == fixture.consumer->Parent(), "successor Phi predecessor did not become the tail block");
    const auto& order = fixture.program.BlockOrder();
    const auto position = static_cast<std::size_t>(std::find(order.begin(), order.end(), fixture.latch) - order.begin());
    const auto* replacedCondition = fixture.program.Metadata().blockInfo[position].condition;
    Require(replacedCondition != &condition && replacedCondition->IsPhi() && replacedCondition->PhiBlock(0u) == fixture.consumer->Parent(), "control condition still points to the old Phi");
    Require(fixture.program.Resources().descriptorSources[0u].dwords[0u]->Resolve() == replacement &&
        fixture.program.Resources().srtReads[0u].value->Resolve() == replacement &&
        fixture.program.Resources().uniformFill.values[0u]->Resolve() == replacement &&
        fixture.program.Metadata().dynamicReads[0u]->Resolve() == replacement,
        "resource-plan roots lost the repaired successor Phi");
    Require(fixture.program.Resources().controlFlow[0u].condition->Resolve() == replacedCondition, "resource control-flow root lost its condition Phi");
    const auto plan = ResourceMaterializer{}.ExtractPlan(fixture.program);
    Require(plan.descriptorSources[0u].dwords[0u]->Resolve()->IsPhi() &&
        plan.srtReads[0u].value->Resolve()->IsPhi() && plan.uniformFill.values[0u]->Resolve()->IsPhi() &&
        plan.controlFlow[0u].condition->Resolve()->IsPhi(), "resource plan cloned an invalidated successor Phi");
    for (const auto& value : plan.valueStorage) Require(value->Opcode() != IrOpcode::Unreachable, "resource plan contains an invalidated value");
    Require(Execute(fixture.program, 7u, words).trace == before.trace, "successor Phi trace changed");
    ValidateProgram(fixture.program, true);
}

void CheckLoopReentry() {
    Fixture fixture;
    auto& root = fixture.program.CreateBlock();
    auto& done = fixture.program.CreateBlock();
    fixture.program.SetEntryBlock(root);
    fixture.program.BlockOrder().insert(fixture.program.BlockOrder().begin(), &root);
    fixture.program.BlockOrder().push_back(&done);
    auto& storage = fixture.program.Blocks();
    std::rotate(storage.begin(), storage.end() - 2u, storage.end() - 1u);
    BlockInfo rootInfo;
    rootInfo.id = 20u;
    rootInfo.terminator.kind = TerminatorKind::Branch;
    rootInfo.terminator.trueBlock = 10u;
    BlockInfo doneInfo;
    doneInfo.id = 21u;
    fixture.program.Metadata().blockInfo.push_back(rootInfo);
    fixture.program.Metadata().blockInfo.push_back(doneInfo);
    auto& infos = fixture.program.Metadata().blockInfo;
    std::rotate(infos.begin(), infos.end() - 2u, infos.end() - 1u);
    root.AddBranch(fixture.entry);
    fixture.exit->AddBranch(fixture.entry);
    fixture.exit->AddBranch(&done);
    auto& outer = fixture.program.CreateValue(IrOpcode::Phi, IrType::U32);
    outer.AddPhiOperand(&root, &fixture.ir.Constant(0u));
    fixture.entry->InsertInstructionBefore(nullptr, &outer);
    for (auto* load : fixture.loads) load->ReplaceArgument(1u, &outer);
    auto& outerHeader = infos[1u].terminator;
    outerHeader.loopHeader = true;
    outerHeader.mergeBlock = 21u;
    outerHeader.continueBlock = 14u;
    fixture.ir.SetInsertionPoint(*fixture.exit);
    auto& next = fixture.ir.IAdd(outer, fixture.ir.Constant(1u));
    outer.AddPhiOperand(fixture.exit, &next);
    infos[5u].condition = &fixture.ir.ULessThan(next, fixture.ir.Constant(3u));
    (void)fixture.ir.Emit(IrOpcode::Reference, IrType::Void, {infos[5u].condition});
    infos[5u].terminator.kind = TerminatorKind::ConditionalBranch;
    infos[5u].terminator.condition = BranchCondition::ScalarInstruction;
    infos[5u].terminator.trueBlock = 10u;
    infos[5u].terminator.falseBlock = 21u;
    const std::array<std::uint32_t, 9u> words{0x80000000u, 0x7fc12345u, 1u};
    const auto expected = Execute(fixture.program, 4u, words);
    Require(LoopInvariantCache{}.Cache(fixture.program).cachedSlices == 1u, "reentered inner loop was not cached");
    const auto actual = Execute(fixture.program, 4u, words);
    Require(actual.trace == expected.trace && actual.trace.size() == 36u, "reentry reused values from the preceding loop execution");
    Require(actual.loads == 27u, "reentry did not reset the first-use flag");
    ValidateProgram(fixture.program, true);
}

void CheckAddressRefusal() {
    Fixture fixture;
    fixture.ir.SetInsertionPoint(*fixture.latch);
    auto& address = fixture.ir.Emit(IrOpcode::GetAddressResource, IrType::AddressResource,
        {&fixture.ir.Constant(0u), &fixture.ir.Constant(0u)});
    MemoryInfo memory;
    memory.kind = ResourceKind::ScalarAddress;
    const auto index = static_cast<std::uint32_t>(fixture.program.Resources().memoryInfo.size());
    fixture.program.Resources().memoryInfo.push_back(memory);
    auto& load = fixture.ir.Emit(IrOpcode::LoadAddressU32, IrType::U32,
        {&address, &fixture.ir.Constant(0u), &fixture.ir.Constant(0u), &fixture.ir.ConstantBool(true)});
    load.SetFlags(MemoryFlags{index, 0x80u});
    fixture.Negative();
}

void CheckSharedRefusals() {
    for (const auto opcode : {IrOpcode::LoadSharedU32, IrOpcode::SharedAtomicIAdd32}) {
        Fixture fixture;
        fixture.ir.SetInsertionPoint(*fixture.latch);
        MemoryInfo memory;
        memory.kind = ResourceKind::Lds;
        const auto index = static_cast<std::uint32_t>(fixture.program.Resources().memoryInfo.size());
        fixture.program.Resources().memoryInfo.push_back(memory);
        auto& operation = fixture.program.CreateValue(opcode, IrType::U32);
        operation.AddArgument(&fixture.ir.Constant(0u));
        if (opcode == IrOpcode::SharedAtomicIAdd32) operation.AddArgument(&fixture.ir.Constant(1u));
        operation.AddArgument(&fixture.ir.ConstantBool(true));
        operation.SetFlags(MemoryFlags{index, 0x80u});
        fixture.latch->AppendInstruction(&operation);
        fixture.Negative();
    }
}

void CheckBufferEffects() {
    for (const auto opcode : {IrOpcode::StoreBufferU32, IrOpcode::BufferAtomicIAdd32}) {
        Fixture fixture;
        fixture.ir.SetInsertionPoint(*fixture.latch);
        MemoryInfo memory;
        memory.kind = ResourceKind::Buffer;
        const auto index = static_cast<std::uint32_t>(fixture.program.Resources().memoryInfo.size());
        fixture.program.Resources().memoryInfo.push_back(memory);
        auto& operation = fixture.ir.Emit(opcode, opcode == IrOpcode::StoreBufferU32 ? IrType::Void : IrType::U32,
            {fixture.handle, &fixture.ir.Constant(0u), &fixture.ir.Constant(0u), &fixture.ir.Constant(0u), &fixture.ir.Constant(1u), &fixture.ir.ConstantBool(true)});
        operation.SetFlags(MemoryFlags{index, 0x80u});
        fixture.Negative();
    }
}

void CheckFutureSrtGuard() {
    Fixture fixture;
    fixture.ir.SetInsertionPoint(*fixture.entry);
    auto& srt = fixture.ir.Emit(IrOpcode::GetSrtResource, IrType::SrtResource, {});
    auto& read = fixture.ir.Emit(IrOpcode::ReadConst, IrType::U32, {&srt, &fixture.ir.Constant(999u)});
    fixture.handle->ReplaceArgument(0u, &read);
    fixture.program.Resources().srtReads.push_back({fixture.loads[0u], 0u});
    Require(fixture.program.Resources().guardedSrtSlots.empty(), "future guard fixture already has runtime guards");
    fixture.Negative();
}

void CheckRefusals() {
    Fixture varying;
    varying.loads[0u]->ReplaceArgument(1u, varying.counter);
    varying.Negative();
    Fixture written;
    written.program.Info().buffers[0u].written = true;
    written.Negative();
    Fixture coherent;
    coherent.program.Resources().memoryInfo[0u].coherent = true;
    coherent.Negative();
    Fixture gpu;
    gpu.program.Resources().memoryInfo[0u].gpuDescriptor = true;
    gpu.Negative();
    Fixture poisoned;
    poisoned.program.Resources().guardedSrtSlots.push_back(0u);
    poisoned.Negative();
    Fixture barrier;
    barrier.ir.SetInsertionPoint(*barrier.latch);
    (void)barrier.ir.Emit(IrOpcode::Barrier, IrType::Void, {});
    barrier.Negative();
    Fixture clock;
    clock.ir.SetInsertionPoint(*clock.latch);
    (void)clock.ir.Emit(IrOpcode::ShaderClock, IrType::U64, {});
    clock.Negative();
    Fixture unsupported;
    unsupported.program.Metadata().blockInfo[1u].terminator.loopHeader = false;
    unsupported.Negative();
    Fixture cycle;
    cycle.body->AddBranch(cycle.body);
    auto& bodyInfo = cycle.program.Metadata().blockInfo[2u];
    bodyInfo.terminator.kind = TerminatorKind::ConditionalBranch;
    bodyInfo.terminator.condition = BranchCondition::ScalarInstruction;
    bodyInfo.terminator.trueBlock = 12u;
    bodyInfo.terminator.falseBlock = 13u;
    bodyInfo.condition = &cycle.ir.ConstantBool(true);
    cycle.Negative();
    Fixture skip;
    skip.body->Successors().clear();
    skip.latch->Predecessors().clear();
    skip.body->AddBranch(skip.latch);
    skip.header->AddBranch(skip.latch);
    auto& skipInfo = skip.program.Metadata().blockInfo[1u];
    skipInfo.terminator.falseBlock = 13u;
    skip.header->Successors().erase(skip.header->Successors().begin() + 1u);
    skip.exit->Predecessors().clear();
    skip.latch->AddBranch(skip.exit);
    auto& latchInfo = skip.program.Metadata().blockInfo[3u];
    latchInfo.terminator.kind = TerminatorKind::ConditionalBranch;
    latchInfo.terminator.condition = BranchCondition::ScalarInstruction;
    latchInfo.terminator.falseBlock = 14u;
    latchInfo.condition = &skip.ir.ConstantBool(true);
    skip.Negative();
}

}

int main() {
    try {
        CheckExecution();
        CheckPlanningOnlyReferences();
        CheckSuccessorPhis();
        CheckLoopReentry();
        CheckAddressRefusal();
        CheckSharedRefusals();
        CheckBufferEffects();
        CheckFutureSrtGuard();
        CheckRefusals();
        std::cout << "loop invariant cache CPU tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
