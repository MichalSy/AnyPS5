#include "prx/libSceAgcDriver/Execution/include/ShaderDeviceProfile.hpp"
#include "BdaAbi.hpp"
#include "Optimization/BindingAllocator.hpp"
#include "Optimization/DescriptorBindingBuilder.hpp"
#include "Optimization/ResourceMaterializer.hpp"
#include "PipelineSpecialization.hpp"
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <array>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace {

using AgcDriver::Graphics::Require;

struct ProfileInput {
    std::vector<std::uint32_t> capabilities{spv::CapabilityShader, spv::CapabilityInt64, spv::CapabilityPhysicalStorageBufferAddresses, spv::CapabilityStorageBuffer8BitAccess};
    std::array<std::string, 2> extensionStrings{"SPV_KHR_physical_storage_buffer", "SPV_KHR_8bit_storage"};
    std::array<std::string_view, 2> extensions{extensionStrings[0], extensionStrings[1]};
    VkPhysicalDeviceRobustness2FeaturesEXT robustness{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, nullptr, VK_FALSE, VK_FALSE, VK_TRUE};
    VkPhysicalDeviceDescriptorIndexingFeatures indexing{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES, &robustness};
    VkPhysicalDevice8BitStorageFeatures bytes{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, &indexing, VK_TRUE};
    VkPhysicalDeviceBufferDeviceAddressFeatures bda{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, &bytes, VK_TRUE};
    VkPhysicalDeviceFeatures core{};
    VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    VkPhysicalDeviceLimits limits{};

    ProfileInput() {
        core.shaderInt64 = VK_TRUE;
        core.vertexPipelineStoresAndAtomics = VK_TRUE;
        core.fragmentStoresAndAtomics = VK_TRUE;
        device.pEnabledFeatures = &core;
        device.pNext = &bda;
        limits.maxPushConstantsSize = 128u;
        limits.maxBoundDescriptorSets = 1u;
        limits.maxStorageBufferRange = sizeof(ShaderRecompiler::RuntimeAbi::ShaderData);
    }

    ShaderRecompiler::SpirvTarget Target() const {
        ShaderRecompiler::SpirvTarget target{};
        target.bdaAbiVersion = ShaderRecompiler::BdaAbi::Version;
        target.supportedCapabilities = capabilities;
        target.supportedExtensions = extensions;
        return target;
    }
};

template<typename TAction>
void Reject(TAction action, const char* expected) {
    try {
        action();
    } catch (const std::runtime_error& error) {
        Require(std::string(error.what()).find(expected) != std::string::npos, "unexpected profile validation error");
        return;
    }
    throw std::runtime_error("invalid shader device profile was accepted");
}

void CheckProfile() {
    ProfileInput input;
    const AgcDriver::ShaderDeviceProfile profile(input.Target(), input.device, input.limits);
    input.capabilities.clear();
    input.extensionStrings[0] = "changed";
    input.core.shaderInt64 = VK_FALSE;
    input.robustness.nullDescriptor = VK_FALSE;
    input.limits.maxPushConstantsSize = 0u;
    const auto target = profile.Target();
    Require(target.supportedCapabilities.size() == 4u && std::ranges::is_sorted(target.supportedCapabilities), "profile capabilities were not frozen");
    Require(std::ranges::find(target.supportedExtensions, "SPV_KHR_physical_storage_buffer") != target.supportedExtensions.end(), "profile extension storage is borrowed");
    Require(profile.Limits().maxPushConstantsSize == 128u, "profile limits were not frozen");
    Require(profile.NullDescriptors(), "profile null descriptor support was not frozen");
    static_assert(!std::is_copy_constructible_v<AgcDriver::ShaderDeviceProfile> && !std::is_move_constructible_v<AgcDriver::ShaderDeviceProfile>);
    const auto invalid = [](auto mutate, const char* expected) {
        ProfileInput candidate;
        mutate(candidate);
        Reject([&] { AgcDriver::ShaderDeviceProfile rejected(candidate.Target(), candidate.device, candidate.limits); }, expected);
    };
    invalid([](auto& value) { value.device.pEnabledFeatures = nullptr; }, "enabled core features");
    invalid([](auto& value) { value.bda.bufferDeviceAddress = VK_FALSE; }, "bufferDeviceAddress");
    invalid([](auto& value) { value.device.pNext = nullptr; }, "bufferDeviceAddress");
    invalid([](auto& value) { value.core.shaderInt64 = VK_FALSE; }, "shaderInt64");
    invalid([](auto& value) { value.bytes.storageBuffer8BitAccess = VK_FALSE; }, "storageBuffer8BitAccess");
    invalid([](auto& value) { value.robustness.nullDescriptor = VK_FALSE; }, "nullDescriptor");
    invalid([](auto& value) { value.indexing.pNext = nullptr; }, "nullDescriptor");
    invalid([](auto& value) { value.core.fragmentStoresAndAtomics = VK_FALSE; }, "graphics stores and atomics");
    invalid([](auto& value) { value.limits.maxPushConstantsSize = 127u; }, "exceeds device limits");
    invalid([](auto& value) { value.limits.maxBoundDescriptorSets = 0u; }, "exceeds device limits");
    invalid([](auto& value) { --value.limits.maxStorageBufferRange; }, "ShaderData exceeds device limits");
    invalid([](auto& value) { value.capabilities.clear(); }, "missing runtime capabilities");
    invalid([](auto& value) { value.extensions[0] = "missing"; }, "missing runtime extensions");
    invalid([](auto& value) { value.capabilities.push_back(spv::CapabilitySampledImageArrayDynamicIndexing); }, "shaderSampledImageArrayDynamicIndexing");
    invalid([](auto& value) { value.capabilities.push_back(spv::CapabilityStorageImageArrayNonUniformIndexing); }, "shaderStorageImageArrayNonUniformIndexing");
    invalid([](auto& value) { value.capabilities.push_back(spv::CapabilityStorageImageReadWithoutFormat); }, "shaderStorageImageReadWithoutFormat");
    invalid([](auto& value) { value.capabilities.push_back(spv::CapabilityStorageImageMultisample); }, "shaderStorageImageMultisample");
    invalid([](auto& value) { value.capabilities.push_back(spv::CapabilityFragmentBarycentricKHR); }, "fragmentShaderBarycentric");
    invalid([](auto& value) { value.capabilities.push_back(spv::CapabilityMeshShadingEXT); }, "meshShader");
    ProfileInput enabled;
    enabled.capabilities.push_back(spv::CapabilitySampledImageArrayDynamicIndexing);
    enabled.capabilities.push_back(spv::CapabilityStorageImageArrayNonUniformIndexing);
    enabled.core.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
    enabled.indexing.shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
    const AgcDriver::ShaderDeviceProfile accepted(enabled.Target(), enabled.device, enabled.limits);
    Require(accepted.Target().supportedCapabilities.size() == 6u, "enabled descriptor indexing was rejected");
}

void CheckAbi() {
    using ShaderRecompiler::RuntimeAbi::Binding;
    using ShaderRecompiler::RuntimeAbi::BindingNumber;
    using ShaderRecompiler::RuntimeAbi::Stage;
    std::set<std::uint32_t> bindings;
    for (std::uint32_t stage = 0u; stage < ShaderRecompiler::RuntimeAbi::StageCount; ++stage) {
        for (std::uint32_t binding = 0u; binding < static_cast<std::uint32_t>(Binding::Count); ++binding) {
            Require(bindings.insert(BindingNumber(static_cast<Stage>(stage), static_cast<Binding>(binding))).second, "runtime ABI bindings overlap");
        }
    }
    Require(BindingNumber(Stage::Main, Binding::ShaderData) == 62u && BindingNumber(Stage::Fragment, Binding::ShaderData) == 125u, "runtime ABI binding numbers changed");
    Reject([] { BindingNumber(static_cast<Stage>(4u), Binding::Buffers); }, "invalid stage or binding");
    Reject([] { BindingNumber(Stage::Main, Binding::Count); }, "invalid stage or binding");
    Reject([] { ShaderRecompiler::RuntimeAbi::RequireVersion(0u); }, "incompatible version");
    Reject([] { ShaderRecompiler::RuntimeAbi::RequireVersion(10u); }, "incompatible version");
    ShaderRecompiler::RuntimeAbi::RequireVersion(ShaderRecompiler::RuntimeAbi::Version);
    using namespace ShaderRecompiler;
    Require(RuntimeAbi::Version == 11u && RuntimeAbi::SampledHeapCapacity == 32u && RuntimeAbi::BindlessTableCapacity == 16u, "sampled and bindless heap capacities are not independent");
    Require(RuntimeAbi::StorageHeapCapacity == 4u && RuntimeAbi::SamplerHeapCapacity == 16u, "storage or sampler heap capacity changed");
    Require(PipelineSpecialization::DescriptorIndexStride == 128u && RuntimeAbi::SampledHeapCapacity < PipelineSpecialization::DescriptorIndexStride, "sampled heap exceeds the descriptor specialization stride");
    std::set<std::uint32_t> indices;
    for (std::uint32_t binding = RuntimeAbi::FirstImageBinding; binding <= static_cast<std::uint32_t>(Binding::Samplers); ++binding) {
        for (std::uint32_t element = 0u; element < RuntimeAbi::HeapCapacity(static_cast<Binding>(binding)); ++element) {
            const auto id = PipelineSpecialization::DescriptorIndex(binding, element);
            Require(indices.insert(id).second && id < PipelineSpecialization::ImageModeBase, "typed heap descriptor specialization IDs overlap");
        }
    }
    Require(sizeof(RuntimeAbi::ResourceMetadata) == 48u && sizeof(RuntimeAbi::ShaderData) == 13760u, "runtime metadata size changed");
    Require(RuntimeAbi::UserDataDword == 4u && RuntimeAbi::BufferOffsetsDword == 132u && RuntimeAbi::DispatchThreadLimitDword == 164u && RuntimeAbi::ExportMappingsDword == 3432u, "runtime metadata offsets changed");
    Require(offsetof(RuntimeAbi::ShaderData, images) == 672u && offsetof(RuntimeAbi::ShaderData, samplers) == 12960u, "image or sampler metadata offset changed");
}

void CheckHeaps() {
    using namespace ShaderRecompiler;
    const auto allocate = [](const ImageResource& image, std::uint32_t count, std::uint32_t samplers = 0u) {
        IrProgram program;
        program.Metadata().shaderInfoComplete = true;
        program.Resources().info.images.assign(count, image);
        program.Resources().info.samplers.resize(samplers);
        return BindingAllocator{}.Allocate(program, {0u, 0u, 0u, 128u});
    };
    ImageResource image;
    image.resourceClass = ImageResourceClass::Sampled;
    image.numericClass = IrTextureNumericClass::Float;
    image.dimension = RdnaImageDimension::Dim2D;
    const auto single = allocate(image, 1u);
    const auto nineteen = allocate(image, 19u);
    const auto full = allocate(image, 32u, RuntimeAbi::SamplerHeapCapacity / 2u);
    const auto binding = DescriptorBindingForImage(image);
    Require(BindingAllocator{}.FindBinding(nineteen.layout, binding).resources.size() == 19u, "nineteen direct sampled images were not allocated");
    const auto& sampled = BindingAllocator{}.FindBinding(full.layout, binding).resources;
    Require(sampled.size() == 32u, "the full sampled heap was not allocated");
    for (std::uint32_t slot = 0u; slot < sampled.size(); ++slot) Require(sampled[slot] == slot, "direct sampled image slots were reordered");
    Require(single.layout.ShaderDataDwords() == full.layout.ShaderDataDwords() && single.layout.memoryOffsetDword == full.layout.memoryOffsetDword && !full.layout.UsesPushData(), "runtime layout depends on resource count");
    Require(full.layout.memoryOffsetDword == 0u && full.layout.DispatchThreadLimitDword() == 0u && full.layout.ShaderDataDwords() == 0u, "direct image resources allocated runtime metadata");
    Reject([&] { allocate(image, 33u); }, "heap capacity exceeded");
    Reject([&] { allocate(image, 1u, RuntimeAbi::SamplerHeapCapacity + 1u); }, "metadata capacity");
    image.resourceClass = ImageResourceClass::Storage;
    const auto directStorage = allocate(image, 4u);
    Require(BindingAllocator{}.FindBinding(directStorage.layout, DescriptorBindingForImage(image)).resources.size() == 4u, "four direct storage images were not allocated");
    Reject([&] { allocate(image, 5u); }, "heap capacity exceeded");
    image.mipMode = ImageMipMode::DynamicStorage;
    image.mipCount = RuntimeAbi::StorageHeapCapacity;
    const auto storage = allocate(image, 1u);
    Require(BindingAllocator{}.FindBinding(storage.layout, DescriptorBindingForImage(image)).resources.size() == image.mipCount, "storage heap did not reserve each mip");
    Reject([&] { allocate(image, 2u); }, "heap capacity exceeded");
    Reject([&] { allocate(image, 0u, RuntimeAbi::SamplerHeapCapacity / 2u + 1u); }, "sampler pairs");
    const std::array dimensions{RdnaImageDimension::Dim1D, RdnaImageDimension::Dim1DArray, RdnaImageDimension::Dim2D, RdnaImageDimension::Dim2DArray, RdnaImageDimension::Dim3D, RdnaImageDimension::Dim2DMsaa, RdnaImageDimension::Dim2DMsaaArray};
    std::set<std::uint32_t> classes;
    for (std::uint32_t group = 0u; group < 8u; ++group) {
        for (const auto dimension : dimensions) {
            image = {};
            image.resourceClass = group < 4u ? ImageResourceClass::Sampled : ImageResourceClass::Storage;
            image.numericClass = group == 1u || group >= 5u ? IrTextureNumericClass::Uint : group == 2u ? IrTextureNumericClass::Sint : IrTextureNumericClass::Float;
            image.depthCompare = group == 3u;
            image.atomic = group >= 6u;
            image.atomic64 = group == 7u;
            image.dimension = dimension;
            const auto binding = DescriptorBindingForImage(image);
            Require(classes.insert(static_cast<std::uint32_t>(binding)).second, "typed image classes overlap");
            Require(RuntimeAbi::HeapCapacity(binding) == (group < 4u ? RuntimeAbi::SampledHeapCapacity : RuntimeAbi::StorageHeapCapacity), "typed image class has an invalid capacity");
        }
    }
    Require(classes.size() == RuntimeAbi::ImageBindingCount && *classes.begin() == 1u && *classes.rbegin() == RuntimeAbi::ImageBindingCount, "typed image class mapping is incomplete");
    Reject([] { RuntimeAbi::HeapCapacity(RuntimeAbi::Binding::ShaderData); }, "not a typed heap");
}

void CheckBindlessHeaps() {
    using namespace ShaderRecompiler;
    Require(ResourceMaterializer::BindlessSlots() == 16u, "bindless tables grew with the sampled heap");
    IrProgram program;
    program.Metadata().shaderInfoComplete = true;
    auto& info = program.Resources().info;
    ImageResource image;
    image.resourceClass = ImageResourceClass::Sampled;
    image.numericClass = IrTextureNumericClass::Float;
    image.dimension = RdnaImageDimension::Dim2D;
    info.images.assign(32u, image);
    std::vector<std::uint32_t> expected;
    for (std::uint32_t root = 0u; root < 2u; ++root) {
        auto& table = info.images[root];
        table.indirectRoot = root;
        table.indirectSearchIterations = 5u;
        table.indirectMappingOffset = root * 33u;
        table.indirectResources.push_back(root);
        expected.push_back(root);
        for (std::uint32_t slot = 1u; slot < 16u; ++slot) {
            const auto resource = 1u + root * 15u + slot;
            info.images[resource].indirectRoot = root;
            table.indirectResources.push_back(resource);
            expected.push_back(resource);
        }
    }
    for (const auto& resource : info.images) info.runtimeImageModes.push_back(ResourceMaterializer::RuntimeImageModes(resource));
    const auto allocation = BindingAllocator{}.Allocate(program, {0u, 0u, 0u, 128u});
    const auto kind = DescriptorBindingForImage(image);
    const auto binding = static_cast<std::uint32_t>(kind);
    Require(BindingAllocator{}.FindBinding(allocation.layout, kind).resources == expected && expected[16] == 1u, "two bindless tables did not get consecutive sixteen-slot blocks");
    Require(allocation.layout.runtimeImageCount == 32u && allocation.layout.ImageMetadataDword() == 0u && allocation.layout.ShaderDataDwords() == 384u, "two bindless tables changed the compact metadata layout");
    ResourceSnapshot snapshot;
    snapshot.images.assign(32u, DescriptorValue{{0x00001000u, 0x03800000u, 0x0000c000u, 0x90000facu, 0u, 0u, 0u, 0u}, 8u});
    const auto checkPrepared = [&](const std::vector<std::uint32_t>& resources) {
        const auto prepared = DescriptorBindingBuilder{}.Prepare(allocation.layout, info, IrShaderStage::Compute, snapshot);
        const auto heap = std::ranges::find_if(prepared.bindings, [&](const auto& entry) { return entry.descriptor.binding == binding; });
        Require(heap != prepared.bindings.end() && heap->resources == resources && heap->descriptor.count == resources.size(), "prepared bindless heap slots were not compacted in allocation order");
        for (std::uint32_t element = 0u; element < resources.size(); ++element) {
            const auto resource = resources[element];
            const auto metadata = std::ranges::find_if(prepared.imageMetadata, [&](const auto& entry) { return entry.resource == resource; });
            Require(metadata != prepared.imageMetadata.end() && metadata->metadata.binding == binding && metadata->metadata.firstElement == element && metadata->metadata.elementCount == 1u && metadata->offset == resource * 12u, "bindless metadata names the wrong prepared slot or offset");
        }
        for (std::uint32_t element = 0u; element < expected.size(); ++element) {
            const auto resource = expected[element];
            const auto compact = std::ranges::find(resources, resource);
            const auto value = compact == resources.end() ? 0u : static_cast<std::uint32_t>(compact - resources.begin());
            const auto id = PipelineSpecialization::DescriptorIndex(binding, element);
            Require(std::ranges::find(prepared.specialization, PipelineSpecializationConstant{id, value}) != prepared.specialization.end(), "bindless descriptor specialization names the wrong compact slot");
        }
    };
    checkPrepared(expected);
    snapshot.images[0].dwords[3] = 0xa0000facu;
    auto compact = expected;
    compact.erase(compact.begin());
    checkPrepared(compact);
}

}

int main() {
    try {
        CheckAbi();
        CheckHeaps();
        CheckBindlessHeaps();
        CheckProfile();
        std::cout << "shader runtime ABI and device profile tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
