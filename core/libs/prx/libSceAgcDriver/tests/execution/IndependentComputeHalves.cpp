#include "VulkanTestDevice.hpp"
#include "IntermediateRepresentation/IrBuilder.hpp"
#include "Optimization/BindingAllocator.hpp"
#include "Optimization/DescriptorBindingBuilder.hpp"
#include "Optimization/ResourceMaterializer.hpp"
#include "SpirvBackend/IndependentComputeHalves.hpp"
#include "SpirvBackend/SpirvEmitter.hpp"
#include "SpirvBackend/SpirvOptimizer.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Execution/include/Recipe.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ShaderRecompiler;
namespace {
constexpr std::size_t BlockBytes = 65536u;
constexpr std::uint32_t Width = 8u, Height = 8u, Format32UInt = 20u;
using Words = std::vector<std::uint32_t>;
void Require(bool value, const std::string& reason) {
    if (!value) throw std::runtime_error("independent halves Vulkan: " + reason);
}
struct GuestBlock {
    std::uint8_t* bytes = nullptr;
    GuestBlock() {
#ifdef _WIN32
        bytes = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        bytes = static_cast<std::uint8_t*>(std::aligned_alloc(BlockBytes, BlockBytes));
#endif
        Require(bytes != nullptr, "guest image allocation failed");
        GuestAllocations::Mutation().Add(bytes, BlockBytes, true, true, true);
    }
    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(bytes);
#ifdef _WIN32
        VirtualFree(bytes, 0, MEM_RELEASE);
#else
        std::free(bytes);
#endif
    }
    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;
};
std::array<std::uint32_t, 8> Descriptor(const GuestBlock& block) {
    const auto base = reinterpret_cast<std::uintptr_t>(block.bytes);
    return {static_cast<std::uint32_t>(base >> 8u), static_cast<std::uint32_t>((base >> 40u) & 255u) | (Format32UInt << 20u) | (((Width - 1u) & 3u) << 30u),
        ((Width - 1u) >> 2u) | ((Height - 1u) << 14u), 0x90000facu, 0u, 0u, 0u, 0u};
}
std::uint32_t Seed(std::uint32_t index) { return 0x713579bdu ^ ((index + 1u) * 0x9e3779b1u); }
std::uint32_t Marker(std::uint32_t index) { return 0x101u + index * 0x9e37u; }

struct OracleProgram {
    IrProgram program;
    IrBuilder ir{program};
    ShaderComputeInputInfo compute;
    IrBlock* entry;
    IrBlock* header = nullptr;
    IrBlock* latch = nullptr;
    IrBlock* branch = nullptr;
    IrBlock* body = nullptr;
    IrBlock* merge = nullptr;
    IrBlock* final;
    IrValue* store;
    OracleProgram(const std::array<std::uint32_t, 8>& descriptor, bool diamond, bool loop, bool seedOnly = false) {
        compute.threadsNum[0] = Width; compute.threadsNum[1] = Height; compute.threadsNum[2] = 1u;
        compute.waveSize = 64u; compute.hostSubgroupSize = 32u;
        program.Resources().stage = IrShaderStage::Compute; program.SetWaveSize(64u);
        const auto block = [&]() { auto& next = program.CreateBlock(); program.BlockOrder().push_back(&next); return &next; };
        entry = block(); if (loop) { header = block(); latch = block(); }
        if (diamond) { branch = block(); body = block(); merge = block(); }
        final = block(); program.SetEntryBlock(*entry);
        program.Metadata().blockInfo.resize(program.BlockOrder().size());
        for (auto* b : program.BlockOrder()) Info(b).id = b->Id();
        program.Info().inputs = {{StageInputKind::LocalInvocationId, 0u, 3u, "local"}, {StageInputKind::WorkgroupId, 0u, 3u, "group"}};
        ir.SetInsertionPoint(*entry);
        auto& x = ir.Emit(IrOpcode::GetBuiltin, IrType::U32, {&ir.Constant(static_cast<std::uint32_t>(StageInputKind::LocalInvocationId)), &ir.Constant(0u)});
        auto& y = ir.Emit(IrOpcode::GetBuiltin, IrType::U32, {&ir.Constant(static_cast<std::uint32_t>(StageInputKind::LocalInvocationId)), &ir.Constant(1u)});
        auto& readHandle = Handle(descriptor); auto& writeHandle = Handle(descriptor);
        auto& oppositeY = ir.BitwiseXor(y, ir.Constant(4u));
        auto& readAddress = Address(x, oppositeY); auto& writeAddress = Address(x, y);
        ImageResource readImage{}; readImage.resourceClass = ImageResourceClass::Sampled; readImage.numericClass = IrTextureNumericClass::Uint;
        readImage.dimension = RdnaImageDimension::Dim2D; readImage.read = true; readImage.fmaskCompatible = false;
        ImageResource storage = readImage; storage.resourceClass = ImageResourceClass::Storage; storage.read = false; storage.written = true;
        storage.dimension = RdnaImageDimension::Dim2DArray;
        if (seedOnly) program.Info().images = {storage};
        else program.Info().images = {readImage, storage};
        for (const auto& image : program.Info().images) program.Info().runtimeImageModes.push_back(ResourceMaterializer::RuntimeImageModes(image));
        MemoryInfo memory{}; memory.kind = ResourceKind::Image; memory.imageDimension = RdnaImageDimension::Dim2D;
        memory.imageAddressComponents = 2u; memory.dmask = 1u;
        auto storageMemory = memory;
        storageMemory.imageDimension = RdnaImageDimension::Dim2DArray;
        storageMemory.imageAddressComponents = 3u;
        if (seedOnly) program.Resources().memoryInfo = {storageMemory};
        else { program.Resources().memoryInfo = {memory, storageMemory}; program.Resources().memoryInfo[1u].resource = 1u; }
        auto& shiftedY = ir.Emit(IrOpcode::ShiftLeftLogical32, IrType::U32, {&y, &ir.Constant(3u)});
        auto& index = ir.IAdd(x, shiftedY);
        IrValue* value = nullptr;
        if (seedOnly) {
            auto& product = ir.IMul(ir.IAdd(index, ir.Constant(1u)), ir.Constant(0x9e3779b1u));
            value = &ir.BitwiseXor(ir.Constant(0x713579bdu), product);
        } else {
            auto& read = ir.Emit(IrOpcode::ImageRead, IrType::U32x4, {&readHandle, &readAddress, &ir.ConstantBool(true)}); read.SetFlags(MemoryFlags{0u, 0u});
            auto& initial = ir.CompositeExtract(read, 0u);
            auto& marker = ir.IAdd(ir.Constant(0x101u), ir.IMul(index, ir.Constant(0x9e37u)));
            value = &ir.IAdd(initial, marker);
        }
        auto* afterLoop = diamond ? branch : final;
        if (loop) {
            Edge(entry, header);
            auto& counter = program.CreateValue(IrOpcode::Phi, IrType::U32); header->AppendInstruction(&counter);
            auto& data = program.CreateValue(IrOpcode::Phi, IrType::U32); header->AppendInstruction(&data);
            ir.SetInsertionPoint(*latch);
            auto& next = ir.IAdd(counter, ir.Constant(1u)); auto& nextData = ir.IAdd(data, ir.Constant(1u));
            counter.AddPhiOperand(entry, &ir.Constant(0u)); counter.AddPhiOperand(latch, &next);
            data.AddPhiOperand(entry, value); data.AddPhiOperand(latch, &nextData); Edge(latch, header);
            ir.SetInsertionPoint(*header); auto& condition = ir.ULessThan(counter, ir.Constant(3u));
            auto& term = Info(header).terminator; term.kind = TerminatorKind::ConditionalBranch; term.condition = BranchCondition::SccNonZero;
            term.trueBlock = latch->Id(); term.falseBlock = afterLoop->Id(); term.mergeBlock = afterLoop->Id(); term.continueBlock = latch->Id(); term.loopHeader = true;
            Info(header).condition = &condition; header->AddBranch(latch); header->AddBranch(afterLoop); value = &data;
        } else Edge(entry, afterLoop);
        if (diamond) {
            ir.SetInsertionPoint(*branch); auto& mask = ir.ULessThan(y, ir.Constant(4u));
            auto& condition = ir.LogicalNot(mask); Info(branch).condition = &condition;
            auto& term = Info(branch).terminator; term.kind = TerminatorKind::ConditionalBranch; term.condition = BranchCondition::ExecZero;
            term.trueBlock = merge->Id(); term.falseBlock = body->Id(); term.mergeBlock = merge->Id(); branch->AddBranch(merge); branch->AddBranch(body);
            ir.SetInsertionPoint(*body); auto& changed = ir.IAdd(*value, ir.Constant(0x10000u)); auto& remasked = ir.Select(mask, changed, *value); Edge(body, merge);
            auto& data = program.CreateValue(IrOpcode::Phi, IrType::U32); merge->AppendInstruction(&data);
            data.AddPhiOperand(branch, value); data.AddPhiOperand(body, &remasked); value = &data; Edge(merge, final);
        }
        ir.SetInsertionPoint(*final);
        auto& data = ir.Emit(IrOpcode::CompositeConstructU32x4, IrType::U32x4, {value, &ir.Constant(0u), &ir.Constant(0u), &ir.Constant(0u)});
        store = &ir.Emit(IrOpcode::ImageWrite, IrType::Void, {&writeHandle, &writeAddress, &data, &ir.ConstantBool(true)}); store->SetFlags(MemoryFlags{seedOnly ? 0u : 1u, 0u});
        Info(final).terminator.kind = TerminatorKind::Return;
        program.Resources().srtPlanComplete = true; program.Resources().resourceTrackingComplete = true; program.Metadata().shaderInfoComplete = true;
    }
    BlockInfo& Info(IrBlock* b) {
        const auto i = std::find(program.BlockOrder().begin(), program.BlockOrder().end(), b) - program.BlockOrder().begin();
        return program.Metadata().blockInfo.at(i);
    }
    void Edge(IrBlock* a, IrBlock* b) { a->AddBranch(b); Info(a).terminator.kind = TerminatorKind::Branch; Info(a).terminator.trueBlock = b->Id(); }
    IrValue& Handle(const std::array<std::uint32_t, 8>& d) {
        return ir.Emit(IrOpcode::GetImageResource, IrType::ImageResource, {&ir.Constant(d[0]), &ir.Constant(d[1]), &ir.Constant(d[2]), &ir.Constant(d[3]), &ir.Constant(d[4]), &ir.Constant(d[5]), &ir.Constant(d[6]), &ir.Constant(d[7])});
    }
    IrValue& Address(IrValue& x, IrValue& y) {
        return ir.Emit(IrOpcode::MakeImageAddress, IrType::ImageAddress, {&x, &y, &ir.Constant(0u), &ir.Constant(0u), &ir.Constant(0u), &ir.Constant(0u), &ir.Constant(0u), &ir.Constant(0u), &ir.Constant(0u), &ir.Constant(0u), &ir.Constant(0u), &ir.Constant(0u), &ir.Constant(0u)});
    }
};

void Inspect(const Words& words) {
    std::map<std::uint32_t, std::uint32_t> constants;
    std::map<std::uint32_t, std::set<std::uint32_t>> predecessors, successors, dominators;
    std::set<std::uint32_t> labels; std::vector<std::uint32_t> writes, returns;
    std::uint32_t main = 0u, function = 0u, label = 0u, entry = 0u, barrierLabel = 0u, barriers = 0u;
    std::size_t barrierOffset = 0u, lastRead = 0u; bool localSize = false, guardedAfter = false;
    for (std::size_t offset = 5u; offset < words.size();) {
        const auto count = words[offset] >> 16u; const auto op = words[offset] & 65535u; const auto* w = words.data() + offset;
        Require(count != 0u && offset + count <= words.size(), "malformed SPIR-V structure");
        if (op == spv::OpEntryPoint && w[1] == spv::ExecutionModelGLCompute) main = w[2];
        if (op == spv::OpConstant && count == 4u) constants[w[2]] = w[3];
        if (op == spv::OpExecutionMode && w[1] == main && w[2] == spv::ExecutionModeLocalSize) {
            Require(count == 6u && w[3] == 8u && w[4] == 8u && w[5] == 1u, "LocalSize does not preserve all 64 invocations"); localSize = true;
        }
        if (op == spv::OpFunction) function = w[2];
        if (op == spv::OpFunctionEnd) function = 0u;
        if (function == main && main != 0u) {
            if (op == spv::OpLabel) { label = w[1]; labels.insert(label); if (entry == 0u) entry = label; }
            const auto edge = [&](std::uint32_t target) { successors[label].insert(target); predecessors[target].insert(label); };
            if (op == spv::OpBranch) edge(w[1]);
            if (op == spv::OpBranchConditional) { edge(w[2]); edge(w[3]); }
            if (op == spv::OpSwitch) { edge(w[2]); for (std::size_t i = 4u; i < count; i += 2u) edge(w[i]); }
            if (op == spv::OpImageFetch || op == spv::OpImageRead) lastRead = offset;
            if (op == spv::OpImageWrite) { Require(barrierOffset != 0u, "image write precedes barrier"); writes.push_back(label); }
            if (op == spv::OpReturn) returns.push_back(label);
            if (op == spv::OpControlBarrier) {
                Require(count == 4u && constants.at(w[1]) == spv::ScopeWorkgroup && constants.at(w[2]) == spv::ScopeWorkgroup &&
                    constants.at(w[3]) == (spv::MemorySemanticsAcquireReleaseMask | spv::MemorySemanticsImageMemoryMask | spv::MemorySemanticsUniformMemoryMask), "barrier scope/semantics differ");
                ++barriers; barrierLabel = label; barrierOffset = offset;
            }
            if (barrierOffset != 0u && label == barrierLabel && (op == spv::OpSelectionMerge || op == spv::OpSwitch)) guardedAfter = true;
        }
        offset += count;
    }
    Require(localSize && barriers == 1u && !writes.empty() && !returns.empty() && lastRead != 0u && lastRead < barrierOffset && guardedAfter, "read-prefix/barrier/store-guard structure differs");
    for (auto b : labels) dominators[b] = b == entry ? std::set<std::uint32_t>{entry} : labels;
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto b : labels) {
            if (b == entry) continue;
            Require(!predecessors[b].empty(), "unreachable block in oracle CFG");
            auto next = dominators[*predecessors[b].begin()];
            for (auto p : predecessors[b]) for (auto it = next.begin(); it != next.end();) { if (!dominators[p].contains(*it)) it = next.erase(it); else ++it; }
            next.insert(b); if (next != dominators[b]) { dominators[b] = std::move(next); changed = true; }
        }
    }
    for (auto b : writes) Require(dominators[b].contains(barrierLabel), "barrier is inside/below the store guard");
    for (auto b : returns) Require(dominators[b].contains(barrierLabel), "invocation can return before barrier");
}
void CheckInspectorMutant(const Words& original) {
    auto mutant = original;
    for (std::size_t offset = 5u; offset < mutant.size(); offset += mutant[offset] >> 16u) {
        if ((mutant[offset] & 65535u) != spv::OpControlBarrier) continue;
        const auto count = mutant[offset] >> 16u; mutant.erase(mutant.begin() + offset, mutant.begin() + offset + count); break;
    }
    bool rejected = false; try { Inspect(mutant); } catch (const std::exception&) { rejected = true; }
    Require(rejected, "structural inspector accepted a missing-barrier mutant");
}

RecompileResult Compile(AgcDriver::VulkanDevice& device, OracleProgram& oracle,
                        const std::array<std::uint32_t, 8>& descriptor, bool inspect, std::uint64_t variant) {
    auto allocation = BindingAllocator{}.Allocate(oracle.program, {0u, 0u, 0u, 128u});
    ResourceSnapshot snapshot;
    snapshot.images.assign(oracle.program.Info().images.size(), DescriptorValue{descriptor, 8u});
    DescriptorBindingBuilder{}.Populate(allocation, oracle.program, snapshot, {});
    ShaderStageInputInfo inputs{}; inputs.compute = &oracle.compute;
    const auto target = device.ComputeTarget(64u);
    const SpirvTargetOptions options{target.vulkanVersion, target.spirvVersion, target.subgroupSize, target.bdaAbiVersion, target.supportedCapabilities, target.supportedExtensions, target.nonConstantImageOffsets, target.narrowSubgroupClock};
    auto words = SpirvEmitter{}.Emit(oracle.program, inputs, allocation, options);
    if (inspect) { Inspect(words); CheckInspectorMutant(words); }
    words = ValidateAndOptimizeSpirv(words, target.vulkanVersion, target.spirvVersion, false, false);
    RecompileResult result{}; result.spirv = std::move(words); result.hostSubgroupSize = 32u;
    result.bindings = std::move(allocation.bindings); result.specialization = std::move(allocation.specialization); result.pushConstants = std::move(allocation.pushConstants);
    result.variantId = variant;
    return result;
}

void Run(AgcDriver::VulkanDevice& device, bool diamond, bool loop) {
    GuestBlock block; const auto descriptor = Descriptor(block);
    const auto mip = AgcDriver::Graphics::ComputeMipLayout(AgcDriver::Graphics::TextureTileMode::kLinear, Format32UInt, Width, Height, 1u).at(0);
    std::memset(block.bytes, 0xcdu, BlockBytes);
    for (std::uint32_t y = 0u; y < Height; ++y) for (std::uint32_t x = 0u; x < Width; ++x) {
        const auto seed = Seed(y * Width + x); std::memcpy(block.bytes + mip.tiledOffset + y * mip.pitchBytes + x * 4u, &seed, 4u);
    }
    OracleProgram seed(descriptor, false, false, true);
    auto seedShader = Compile(device, seed, descriptor, false, 0x2ab064100ull);
    std::shared_ptr<const AgcDriver::Recipe> seedRecipe;
    device.Dispatch(seedShader, 1u, 1u, 1u, {}, 0u, nullptr, &seedRecipe); device.WaitIdle();
    Require(seedRecipe != nullptr, "cannot retain the seeded storage owner: enable resource cache and dispatch recipes");
    const auto seedResources = seedRecipe->templateRef.lock();
    Require(seedResources != nullptr && seedResources->Completed(), "seeded storage resource template is unavailable");
    const auto seedImages = seedResources->StorageImages();
    Require(seedImages.size() == 1u && seedImages[0].first != VK_NULL_HANDLE && seedImages[0].second,
        "seed dispatch did not bind exactly one writable native image");
    const auto seedImage = seedImages[0].first;
    OracleProgram oracle(descriptor, diamond, loop);
    Require(IndependentComputeHalfStore(oracle.program, oracle.compute) == oracle.store, "real GPU oracle was not certified");
    auto result = Compile(device, oracle, descriptor, true, 0x2ab064000ull + (diamond ? 1u : 0u) + (loop ? 2u : 0u));
    std::shared_ptr<const AgcDriver::Recipe> recipe;
    device.Dispatch(result, 1u, 1u, 1u, {}, 0u, nullptr, &recipe); device.WaitIdle();
    AgcDriver::Graphics::StorageTexture::FlushPending(reinterpret_cast<std::uintptr_t>(block.bytes), BlockBytes, nullptr, "independent half alias oracle"); device.WaitIdle();
    Require(recipe != nullptr, "cannot inspect actual dispatch owners: enable the existing resource cache and dispatch recipes");
    const auto resources = recipe->templateRef.lock();
    Require(resources != nullptr && resources->Completed(), "actual dispatched resource template is unavailable");
    const auto nativeImages = resources->StorageImages();
    Require(nativeImages.size() == 2u && nativeImages[0].first != VK_NULL_HANDLE &&
        nativeImages[0].first == seedImage && nativeImages[1].first == seedImage &&
        nativeImages[0].second && !nativeImages[1].second,
        "sampled input and storage output do not view the retained seeded VkImage with distinct read/write roles");
    for (std::uint32_t y = 0u; y < Height; ++y) for (std::uint32_t x = 0u; x < Width; ++x) {
        std::uint32_t actual = 0u; std::memcpy(&actual, block.bytes + mip.tiledOffset + y * mip.pitchBytes + x * 4u, 4u);
        const auto expected = Seed((y ^ 4u) * Width + x) + Marker(y * Width + x) + (loop ? 3u : 0u) + (diamond && y < 4u ? 0x10000u : 0u);
        Require(actual == expected, "wrong opposite-half snapshot at (" + std::to_string(x) + "," + std::to_string(y) + "), diamond=" + std::to_string(diamond) + ", loop=" + std::to_string(loop));
    }
}
}
int main() {
    try {
        auto device = OpenVulkanTestDevice(); if (!device) return VulkanTestSkipped;
        if (device->ComputeTarget(64u).subgroupSize != 32u) { std::cout << "skipped: requires a 32-wide compute subgroup\n"; return VulkanTestSkipped; }
        for (bool diamond : {false, true}) for (bool loop : {false, true}) Run(*device, diamond, loop);
        std::cout << "independent halves alias oracle PASS: all 64 pixels, opposite-half reads, marker, pure skip, uniform loop\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
