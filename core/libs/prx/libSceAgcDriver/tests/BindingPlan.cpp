#include "prx/libSceAgcDriver/Graphics/include/BindingPlan.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {

using namespace AgcDriver::Graphics;
using Role = ShaderRecompiler::DescriptorRole;
using Kind = ShaderRecompiler::DescriptorKind;
using Stage = ShaderRecompiler::ShaderStage;

Context testContext() {
    Context context{};
    context.limits.maxBoundDescriptorSets = 1;
    context.limits.maxPerStageResources = 32;
    context.limits.maxPerStageDescriptorStorageBuffers = 16;
    context.limits.maxDescriptorSetStorageBuffers = 32;
    context.limits.maxPerStageDescriptorSampledImages = 16;
    context.limits.maxDescriptorSetSampledImages = 32;
    context.limits.maxPerStageDescriptorStorageImages = 16;
    context.limits.maxDescriptorSetStorageImages = 32;
    context.limits.maxPerStageDescriptorSamplers = 16;
    context.limits.maxDescriptorSetSamplers = 32;
    context.descriptorIndexingLimits.maxPerStageUpdateAfterBindResources = 64;
    context.descriptorIndexingLimits.maxPerStageDescriptorUpdateAfterBindStorageBuffers = 32;
    context.descriptorIndexingLimits.maxDescriptorSetUpdateAfterBindStorageBuffers = 64;
    return context;
}

ShaderRecompiler::DescriptorBinding binding(Role role, std::uint32_t number, std::uint32_t count, std::uint32_t words) {
    ShaderRecompiler::DescriptorBinding result{};
    result.role = role;
    result.binding = number;
    result.count = count;
    result.kind = Kind::StorageBuffer;
    result.guestDescriptor.resize(words);
    return result;
}

template<class TAction>
void expectFailure(TAction action, const std::string& reason) {
    try {
        action();
    } catch (const std::runtime_error& error) {
        Require(std::string(error.what()).find(reason) != std::string::npos, "unexpected binding-plan failure: " + std::string(error.what()));
        return;
    }
    throw std::runtime_error("missing binding-plan failure: " + reason);
}

void retainedMetadataTests() {
    const auto context = testContext();
    BindingPlanCache cache(context, 2);
    ShaderRecompiler::RecompileResult program;
    program.variantId = 1;
    program.memoryOffsetDword = 1;
    program.bindings.push_back(binding(Role::GuestBuffers, 0, 2, 8));
    program.bindings.front().bufferWritten = {false, true};
    program.bindings.front().bufferAtomic = {false, true};
    program.bindings.front().bufferRead = {true, false};
    program.bindings.push_back(binding(Role::ShaderData, 62, 1, 4));
    CompiledShader shader{Stage::Compute, &program, 0};
    const auto first = cache.Get(std::span(&shader, 1));
    Require(first->buffers.size() == 3 && first->bindings.size() == 2, "array allocation counts changed");
    Require(first->bindings.front().allocations[0] == 0 && first->bindings.front().allocations[1] == 1 && first->bindings.back().allocations.front() == 2, "buffer allocation ranges overlap");
    Require(!first->buffers[0].written && !first->buffers[0].atomic && first->buffers[0].read && first->buffers[1].written && first->buffers[1].atomic && !first->buffers[1].read, "buffer access metadata changed");
    Require(first->buffers[0].dataAllocation == 2 && first->buffers[0].dataByte == 4 && first->buffers[1].dataByte == 5, "shader data patch positions changed");
    program.bindings.front().guestDescriptor = {5, 6, 7, 8, 9, 10, 11, 12};
    program.bindings.back().guestDescriptor = {4, 3, 2, 1};
    Require(cache.Get(std::span(&shader, 1)) == first, "live buffer addresses or data replaced the plan");
    program.variantId = 2;
    program.pushConstants.resize(8);
    shader.pushConstantOffset = 16;
    const auto pushed = cache.Get(std::span(&shader, 1));
    Require(pushed->buffers[0].pushByte == 20 && pushed->buffers[1].pushByte == 21, "push patch positions changed");
    shader.pushConstantOffset = 24;
    Require(cache.Get(std::span(&shader, 1))->buffers[0].pushByte == 28 && cache.Size() == 2, "push offsets or capacity were ignored");
    program.bindings.clear();
    Require(first->bindings.back().layout.binding == 62 && first->buffers[0].dataAllocation == 2, "retained plan depends on its shader lifetime");
}

void specializationTests() {
    const auto context = testContext();
    BindingPlanCache cache(context);
    ShaderRecompiler::RecompileResult program;
    program.variantId = 10;
    program.specializationId = 11;
    auto sampled = binding(Role::GuestImages, 1, 2, 16);
    sampled.kind = Kind::SampledImage;
    sampled.imageShape = ShaderRecompiler::DescriptorImageShape::Image2D;
    program.bindings.push_back(sampled);
    CompiledShader shader{Stage::Fragment, &program, 0};
    const auto first = cache.Get(std::span(&shader, 1));
    program.specializationId = 12;
    program.bindings.front().count = 1;
    program.bindings.front().guestDescriptor.resize(8);
    const auto compact = cache.Get(std::span(&shader, 1));
    Require(compact != first && compact->sampledImages == 1 && compact->layout.front().descriptorCount == 1, "specialized compact heaps reused a variant plan");
    program.bindings.clear();
    program.specializationId = 13;
    Require(cache.Get(std::span(&shader, 1))->bindings.empty(), "removed specialized heaps reused a binding");
    shader.stage = Stage::Vertex;
    Require(cache.Get(std::span(&shader, 1))->bindings.empty() && cache.Size() == 4, "shader stage was ignored");
}

void samplerRangeTests() {
    const auto context = testContext();
    BindingPlanCache cache(context);
    ShaderRecompiler::RecompileResult vertex;
    ShaderRecompiler::RecompileResult fragment;
    auto sampler = binding(Role::GuestSamplers, 57, 2, 8);
    sampler.kind = Kind::Sampler;
    sampler.samplerDepthCompare = {false, true};
    vertex.bindings.push_back(sampler);
    sampler.binding = 120;
    sampler.count = 1;
    sampler.guestDescriptor.resize(4);
    sampler.samplerDepthCompare.resize(1);
    fragment.bindings.push_back(sampler);
    std::array<CompiledShader, 2> shaders{{{Stage::Vertex, &vertex, 0}, {Stage::Fragment, &fragment, 128}}};
    const auto plan = cache.Get(shaders);
    Require(plan->samplers == 3 && plan->bindings[0].firstSampler == 0 && plan->bindings[0].samplerCount == 2 && plan->bindings[1].firstSampler == 2 && plan->bindings[1].samplerCount == 1, "sampler ranges crossed shader stages");
}

void limitsTests() {
    auto context = testContext();
    context.limits.maxPerStageResources = 2;
    context.limits.maxPerStageDescriptorStorageBuffers = 2;
    context.limits.maxDescriptorSetStorageBuffers = 2;
    BindingPlanCache cache(context);
    ShaderRecompiler::RecompileResult program;
    program.variantId = 20;
    program.pushConstants.resize(4);
    program.bindings.push_back(binding(Role::GuestBuffers, 0, 1, 4));
    const CompiledShader shader{Stage::Fragment, &program, 0};
    const auto normal = cache.Get(std::span(&shader, 1));
    const auto color = cache.Get(std::span(&shader, 1), 2);
    Require(normal != color && !normal->updateAfterBind && color->updateAfterBind, "color attachment limits reused an incompatible plan");
    Require(color->layoutKey.back() == VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, "extended descriptor limits lost their layout flag");
    program.variantId = 21;
    program.bindings.front().count = 3;
    program.bindings.front().guestDescriptor.resize(12);
    const auto extended = cache.Get(std::span(&shader, 1));
    Require(extended->updateAfterBind && extended->descriptorSizes.front().descriptorCount == 3, "extended storage buffer limits were dropped");
    context.descriptorIndexingLimits.maxPerStageDescriptorUpdateAfterBindStorageBuffers = 2;
    expectFailure([&] { BindingPlan plan(context, std::span(&shader, 1)); }, "per-stage limits");
    context = testContext();
    program.memoryOffsetDword = std::numeric_limits<std::uint32_t>::max();
    expectFailure([&] { BindingPlan plan(context, std::span(&shader, 1)); }, "shader data address space");
    program.memoryOffsetDword = 0;
    program.runtimeAbiVersion = 0;
    expectFailure([&] { BindingPlan plan(context, std::span(&shader, 1)); }, "incompatible version");
    expectFailure([&] { BindingPlan plan(context, {}); }, "descriptor set exceeds device limits");
    expectFailure([&] { BindingPlanCache zero(context, 0); }, "capacity is zero");
}

void evictionTests() {
    const auto context = testContext();
    BindingPlanCache cache(context, 2);
    ShaderRecompiler::RecompileResult program;
    CompiledShader shader{Stage::Compute, &program, 0};
    program.variantId = 30;
    const auto first = cache.Get(std::span(&shader, 1));
    program.variantId = 31;
    const auto second = cache.Get(std::span(&shader, 1));
    program.variantId = 30;
    Require(cache.Get(std::span(&shader, 1)) == first, "cached plan was not reused");
    program.variantId = 32;
    cache.Get(std::span(&shader, 1));
    program.variantId = 30;
    Require(cache.Get(std::span(&shader, 1)) == first, "recently used plan was evicted");
    program.variantId = 31;
    Require(cache.Get(std::span(&shader, 1)) != second && cache.Size() == 2 && second->layout.empty(), "eviction changed retained ownership");
    program.variantId = 0;
    const auto unknown = cache.Get(std::span(&shader, 1));
    program.bindings.push_back(binding(Role::FlattenedSrt, 61, 1, 1));
    const auto changed = cache.Get(std::span(&shader, 1));
    Require(unknown != changed && changed->bindings.size() == 1 && cache.Size() == 2, "unknown variants reused a stale plan");
    program.bindings.clear();
    program.variantId = 33;
    std::array<CompiledShader, 7> many{};
    std::fill(many.begin(), many.end(), shader);
    Require(cache.Get(many) != cache.Get(many) && cache.Size() == 2, "oversized stage lists entered the cache");
    BindingPlanCache bounded(context);
    for (std::uint64_t variant = 100; variant < 1125; ++variant) {
        program.variantId = variant;
        bounded.Get(std::span(&shader, 1));
    }
    Require(bounded.Size() == 1024, "default plan capacity is unbounded");
}

void bypassTests() {
    const auto context = testContext();
    BindingPlanCache cache(context);
    ShaderRecompiler::RecompileResult program;
    program.variantId = 50;
    program.bindings.push_back(binding(Role::FlattenedSrt, 61, 1, 1));
    const CompiledShader shader{Stage::Compute, &program, 0};
    const auto first = cache.Get(std::span(&shader, 1));
    Require(cache.Get(std::span(&shader, 1)) != first && cache.Size() == 0, "disabled plan cache retained a known variant");
    program.bindings.front().binding = 62;
    Require(cache.Get(std::span(&shader, 1))->layout.front().binding == 62, "disabled plan cache reused a binding");
}

}

int main() {
    try {
        if (std::getenv("APS5_NO_BINDING_PLAN_CACHE") != nullptr) bypassTests();
        else {
            retainedMetadataTests();
            specializationTests();
            evictionTests();
        }
        samplerRangeTests();
        limitsTests();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
