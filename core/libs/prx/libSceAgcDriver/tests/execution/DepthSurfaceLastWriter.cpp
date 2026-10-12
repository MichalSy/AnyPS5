#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureSwizzleEquations.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "VulkanTestDevice.hpp"
#include "SampleLod_spv.h"
#include <SDL_loadso.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <array>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace AgcDriver::Graphics;

constexpr std::uint64_t DepthAddress = 0x7f0000000000ull;
constexpr VkExtent2D Extent{64, 64};
constexpr std::uint32_t Format32Float = 22;

class Device {
public:
    Device() {
#ifdef _WIN32
        library = SDL_LoadObject("vulkan-1.dll");
#else
        library = SDL_LoadObject("libvulkan.so.1");
#endif
        Require(library != nullptr, "cannot load Vulkan");
        try {
            instanceProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_LoadFunction(library, "vkGetInstanceProcAddr"));
            Require(instanceProc != nullptr, "missing Vulkan instance resolver");
            VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
            application.apiVersion = VK_API_VERSION_1_1;
            VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
            info.pApplicationInfo = &application;
            Check(function<PFN_vkCreateInstance>("vkCreateInstance")(&info, nullptr, &instance), "vkCreateInstance");
            std::uint32_t count = 0;
            const auto enumerate = function<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
            Check(enumerate(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
            Require(count != 0, "no Vulkan device");
            std::vector<VkPhysicalDevice> devices(count);
            Check(enumerate(instance, &count, devices.data()), "vkEnumeratePhysicalDevices");
            context.physical = devices.front();
            const auto queues = function<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
            queues(context.physical, &count, nullptr);
            std::vector<VkQueueFamilyProperties> families(count);
            queues(context.physical, &count, families.data());
            std::uint32_t family = 0;
            while (family < count && (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) ++family;
            Require(family < count, "no Vulkan graphics queue");
            const float priority = 1;
            VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            queue.queueFamilyIndex = family;
            queue.queueCount = 1;
            queue.pQueuePriorities = &priority;
            VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
            device.queueCreateInfoCount = 1;
            device.pQueueCreateInfos = &queue;
            const auto enumerateExtensions = function<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
            Check(enumerateExtensions(context.physical, nullptr, &count, nullptr), "vkEnumerateDeviceExtensionProperties");
            std::vector<VkExtensionProperties> extensions(count);
            Check(enumerateExtensions(context.physical, nullptr, &count, extensions.data()), "vkEnumerateDeviceExtensionProperties");
            context.sampleLocations = std::any_of(extensions.begin(), extensions.end(), [](const auto& extension) {
                return std::strcmp(extension.extensionName, VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME) == 0;
            });
            const char* sampleLocationsExtension = VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME;
            if (context.sampleLocations) {
                device.enabledExtensionCount = 1;
                device.ppEnabledExtensionNames = &sampleLocationsExtension;
                VkPhysicalDeviceProperties2 locations{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &context.sampleLocationProperties};
                function<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2")(context.physical, &locations);
                if (context.sampleLocationProperties.sampleLocationSampleCounts & VK_SAMPLE_COUNT_8_BIT) {
                    VkMultisamplePropertiesEXT grid{VK_STRUCTURE_TYPE_MULTISAMPLE_PROPERTIES_EXT};
                    function<PFN_vkGetPhysicalDeviceMultisamplePropertiesEXT>("vkGetPhysicalDeviceMultisamplePropertiesEXT")(context.physical, VK_SAMPLE_COUNT_8_BIT, &grid);
                    context.sampleLocationGridSizes[std::countr_zero(static_cast<std::uint32_t>(VK_SAMPLE_COUNT_8_BIT))] = grid.maxSampleLocationGridSize;
                }
            }
            VkPhysicalDeviceFeatures features{};
            function<PFN_vkGetPhysicalDeviceFeatures>("vkGetPhysicalDeviceFeatures")(context.physical, &features);
            device.pEnabledFeatures = &features;
            Check(function<PFN_vkCreateDevice>("vkCreateDevice")(context.physical, &device, nullptr, &context.device), "vkCreateDevice");
            context.deviceProc = function<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
            context.formatProperties = function<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties");
            context.imageFormatProperties = function<PFN_vkGetPhysicalDeviceImageFormatProperties>("vkGetPhysicalDeviceImageFormatProperties");
            function<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(context.physical, &context.memory);
            VkPhysicalDeviceProperties properties{};
            function<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(context.physical, &properties);
            context.limits = properties.limits;
            VkPhysicalDeviceProperties2 extended{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &context.subgroup};
            function<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2")(context.physical, &extended);
            context.geometryShader = features.geometryShader;
            context.shaderClipDistance = features.shaderClipDistance;
            context.sampleRateShading = features.sampleRateShading;
            capabilities.push_back(static_cast<std::uint32_t>(spv::CapabilityShader));
            if (features.shaderClipDistance) capabilities.push_back(static_cast<std::uint32_t>(spv::CapabilityClipDistance));
            if (features.shaderInt64) capabilities.push_back(static_cast<std::uint32_t>(spv::CapabilityInt64));
            if (context.subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BASIC_BIT) capabilities.push_back(static_cast<std::uint32_t>(spv::CapabilityGroupNonUniform));
            if (context.subgroup.supportedOperations & VK_SUBGROUP_FEATURE_VOTE_BIT) capabilities.push_back(static_cast<std::uint32_t>(spv::CapabilityGroupNonUniformVote));
            if (context.subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT) capabilities.push_back(static_cast<std::uint32_t>(spv::CapabilityGroupNonUniformBallot));
            if (context.subgroup.supportedOperations & VK_SUBGROUP_FEATURE_SHUFFLE_BIT) capabilities.push_back(static_cast<std::uint32_t>(spv::CapabilityGroupNonUniformShuffle));
            context.Function<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(context.device, family, 0, &context.queue);
            VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pool.queueFamilyIndex = family;
            Check(context.Function<PFN_vkCreateCommandPool>("vkCreateCommandPool")(context.device, &pool, nullptr, &context.pool), "vkCreateCommandPool");
            detiler = std::make_unique<TextureDetiler>(context);
            context.detiler = detiler.get();
        } catch (...) {
            release();
            throw;
        }
    }

    ~Device() { release(); }
    const Context& GetContext() const { return context; }
    ShaderRecompiler::SpirvTarget Target() const {
        return {VK_API_VERSION_1_1, 0x00010300u, context.subgroup.subgroupSize, ShaderRecompiler::BdaAbi::Version, capabilities, {}, false,
            {context.limits.maxComputeWorkGroupSize[0], context.limits.maxComputeWorkGroupSize[1], context.limits.maxComputeWorkGroupSize[2]},
            context.limits.maxComputeWorkGroupInvocations, context.limits.maxComputeSharedMemorySize, {}, {}};
    }

private:
    template<typename TFunction>
    TFunction function(const char* name) const {
        const auto result = reinterpret_cast<TFunction>(instanceProc(instance, name));
        Require(result != nullptr, name);
        return result;
    }

    void release() noexcept {
        if (context.device != VK_NULL_HANDLE) {
            ClearCachedPipelines(context.device);
            ClearCachedTextures(context.device);
            ClearDepthSurfaces(context.device);
        }
        detiler.reset();
        if (context.pool != VK_NULL_HANDLE) context.Function<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(context.device, context.pool, nullptr);
        context.bufferPool.reset();
        if (context.device != VK_NULL_HANDLE) function<PFN_vkDestroyDevice>("vkDestroyDevice")(context.device, nullptr);
        if (instance != VK_NULL_HANDLE) function<PFN_vkDestroyInstance>("vkDestroyInstance")(instance, nullptr);
        if (library != nullptr) SDL_UnloadObject(library);
    }

    void* library = nullptr;
    PFN_vkGetInstanceProcAddr instanceProc = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    Context context{};
    std::vector<std::uint32_t> capabilities;
    std::unique_ptr<TextureDetiler> detiler;
};

constexpr std::size_t Block = 65536;

class GuestBlock {
public:
    explicit GuestBlock(std::size_t bytes) : reservedBytes(bytes), mappedBytes(bytes) {
        data = static_cast<std::byte*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, Block));
        Require(data != nullptr, "cannot allocate the guest surface");
        commit(bytes);
        GuestAllocations::Mutation mutation;
        mutation.Add(data, bytes, true, true, true);
    }
    ~GuestBlock() {
        {
            GuestAllocations::Mutation mutation;
            mutation.Unmap(data, mappedBytes, [&](const void*, std::size_t, const void*, bool) { reset(); });
        }
        GuestArena::GuestArenaRelease_nid_postfix(data, reservedBytes);
    }
    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;
    std::uint64_t Address() const { return reinterpret_cast<std::uintptr_t>(data); }
    void Remap(std::size_t bytes) {
        Require(bytes != 0 && bytes < mappedBytes && bytes % Block == 0, "the replacement must leave an inaccessible tail");
        {
            GuestAllocations::Mutation mutation;
            mutation.Unmap(data, mappedBytes, [&](const void*, std::size_t, const void*, bool) { reset(); });
        }
        Require(!AgcDriver::GuestMemory::Accessible(data, 1), "unmap left the old surface readable");
        commit(bytes);
        mappedBytes = bytes;
        {
            GuestAllocations::Mutation mutation;
            mutation.Add(data, bytes, true, true, true);
        }
        Require(!AgcDriver::GuestMemory::Accessible(data + bytes, 1), "the old allocation tail survived the smaller remap");
    }
    std::byte* data = nullptr;

private:
    void commit(std::size_t bytes) {
        GuestArena::GuestArenaCommit_nid_postfix(data, bytes, 0x04u, Block);
#ifndef _WIN32
        GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(data, bytes);
#endif
    }
    void reset() {
#ifndef _WIN32
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(data, mappedBytes);
#endif
        GuestArena::GuestArenaReset_nid_postfix(data, reservedBytes);
    }
    std::size_t reservedBytes;
    std::size_t mappedBytes;
};

DepthTarget Depth(std::uint64_t address = DepthAddress, VkExtent2D extent = Extent) {
    return {address, 0, extent, VK_FORMAT_D32_SFLOAT, 1.0f, 0};
}

GuestTextureResource View(std::uint32_t width, std::uint32_t height, std::uint64_t address = DepthAddress) {
    GuestTextureResource resource{};
    resource.baseAddress = address;
    resource.width = width;
    resource.height = height;
    resource.mipCount = 1;
    resource.dimension = TextureDimension::k2D;
    resource.format = Format32Float;
    return resource;
}

constexpr VkComponentMapping Identity{VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};

class SampleProgram {
public:
    explicit SampleProgram(const Context& context) : context(context), result(context, sizeof(float) * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) {
        try {
            const VkDescriptorSetLayoutBinding bindings[]{
                {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}
            };
            VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            layoutInfo.bindingCount = 2;
            layoutInfo.pBindings = bindings;
            Check(context.Function<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(context.device, &layoutInfo, nullptr, &setLayout), "vkCreateDescriptorSetLayout");
            const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(float)};
            VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            pipelineLayoutInfo.setLayoutCount = 1;
            pipelineLayoutInfo.pSetLayouts = &setLayout;
            pipelineLayoutInfo.pushConstantRangeCount = 1;
            pipelineLayoutInfo.pPushConstantRanges = &push;
            Check(context.Function<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(context.device, &pipelineLayoutInfo, nullptr, &pipelineLayout), "vkCreatePipelineLayout");
            VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            moduleInfo.codeSize = sizeof(SAMPLE_LOD_SPV);
            moduleInfo.pCode = SAMPLE_LOD_SPV;
            Check(context.Function<PFN_vkCreateShaderModule>("vkCreateShaderModule")(context.device, &moduleInfo, nullptr, &module), "vkCreateShaderModule");
            VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
            pipelineInfo.layout = pipelineLayout;
            Check(context.Function<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(context.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline), "vkCreateComputePipelines");
            VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_NEAREST;
            samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            Check(context.Function<PFN_vkCreateSampler>("vkCreateSampler")(context.device, &samplerInfo, nullptr, &sampler), "vkCreateSampler");
            const VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
            VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            poolInfo.maxSets = 1;
            poolInfo.poolSizeCount = 2;
            poolInfo.pPoolSizes = sizes;
            Check(context.Function<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(context.device, &poolInfo, nullptr, &pool), "vkCreateDescriptorPool");
            VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            allocateInfo.descriptorPool = pool;
            allocateInfo.descriptorSetCount = 1;
            allocateInfo.pSetLayouts = &setLayout;
            Check(context.Function<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(context.device, &allocateInfo, &set), "vkAllocateDescriptorSets");
        } catch (...) {
            release();
            throw;
        }
    }
    ~SampleProgram() { release(); }
    SampleProgram(const SampleProgram&) = delete;
    SampleProgram& operator=(const SampleProgram&) = delete;

    std::uint32_t RedBits(const Texture& texture) {
        const VkDescriptorImageInfo image{sampler, texture.View(), texture.Layout()};
        const VkDescriptorBufferInfo buffer{result.Handle(), 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet writes[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        writes[0].dstSet = set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &image;
        writes[1].dstSet = set;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &buffer;
        context.Function<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(context.device, 2, writes, 0, nullptr);
        CommandBatch batch(context);
        const auto commands = batch.Handle();
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        context.Function<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        context.Function<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, nullptr);
        const float lod = 0;
        context.Function<PFN_vkCmdPushConstants>("vkCmdPushConstants")(commands, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(lod), &lod);
        context.Function<PFN_vkCmdDispatch>("vkCmdDispatch")(commands, 1, 1, 1);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        batch.SubmitAndWait();
        std::uint32_t value;
        std::memcpy(&value, result.Bytes().data(), sizeof(value));
        return value;
    }

private:
    void release() noexcept {
        if (pool) context.Function<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(context.device, pool, nullptr);
        if (sampler) context.Function<PFN_vkDestroySampler>("vkDestroySampler")(context.device, sampler, nullptr);
        if (pipeline) context.Function<PFN_vkDestroyPipeline>("vkDestroyPipeline")(context.device, pipeline, nullptr);
        if (module) context.Function<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(context.device, module, nullptr);
        if (pipelineLayout) context.Function<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(context.device, pipelineLayout, nullptr);
        if (setLayout) context.Function<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(context.device, setLayout, nullptr);
    }
    const Context& context;
    Buffer result;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
};

std::shared_ptr<Texture> Sample(const Context& context, const GuestTextureResource& resource) {
    const std::array<std::uint32_t, 8> words{0, 0, 0, 0, resource.width, resource.height, 0, 0};
    return DepthSurfaceTexture(context, words, resource, Identity);
}

bool Refused(const Context& context, const GuestTextureResource& resource) {
    try {
        return Sample(context, resource) == nullptr && DepthSurfaceAt(resource.baseAddress);
    } catch (const std::runtime_error& error) {
        return std::string(error.what()).find("is not implemented") != std::string::npos && DepthSurfaceAt(resource.baseAddress);
    }
}

alignas(256) constexpr std::array<std::uint32_t, 6> VertexCode{
    0xe0382000, 0x80000005, 0xbf8c3f70, 0xf80008cf, 0x03020100, 0xbf810000
};
alignas(256) constexpr std::array<std::uint32_t, 11> PixelCode{
    0x7e0802ff, 0x3f800000, 0x7e0a02ff, 0x3f800000,
    0x7e0c02ff, 0x3f800000, 0x7e0e02ff, 0x3f800000,
    0xf800180f, 0x07060504, 0xbf810000
};
alignas(256) constexpr std::array<std::array<float, 4>, 3> Vertices{{
    {-1, -1, 0.5f, 1}, {3, -1, 0.5f, 1}, {-1, 3, 0.5f, 1}
}};

void DrawColor(const Device& device, const ColorTarget& color) {
    using ShaderRecompiler::ShaderStage;
    constexpr std::uint32_t waveSize = 64;
    const auto address = reinterpret_cast<std::uintptr_t>(Vertices.data());
    const std::array<std::uint32_t, 4> buffer{
        static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (16u << 16u), 3, 0x01016facu
    };
    const std::vector<std::uint32_t> vertexUser(buffer.begin(), buffer.end());
    const std::array<ShaderRecompiler::MemoryRegion, 1> vertexMemory{{{reinterpret_cast<std::uintptr_t>(VertexCode.data()), std::as_bytes(std::span(VertexCode))}}};
    ShaderRecompiler::RecompileRequest vertex{
        {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(VertexCode.data()), VertexCode, 0, {}},
        {waveSize, 0, vertexUser, std::nullopt, std::nullopt, ShaderRecompiler::ShaderVertexStageInfo{}, vertexMemory}, device.Target(), {0, 0, 0, 64}
    };
    vertex.useCache = false;
    const auto vertexResult = ShaderRecompiler::Recompile(vertex);
    const auto vertexPush = static_cast<std::uint32_t>(vertexResult.pushConstants.size());
    ShaderRecompiler::ShaderPixelStageInfo pixel{};
    pixel.targetOutputMode[0] = 9;
    pixel.targetExportMapping.fill(0xe4u);
    const std::vector<std::uint32_t> pixelUser(8, 0);
    const std::array<ShaderRecompiler::MemoryRegion, 1> pixelMemory{{{reinterpret_cast<std::uintptr_t>(PixelCode.data()), std::as_bytes(std::span(PixelCode))}}};
    ShaderRecompiler::RecompileRequest fragment{
        {ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(PixelCode.data()), PixelCode, 0, {}},
        {waveSize, 0, pixelUser, std::nullopt, pixel, std::nullopt, pixelMemory}, device.Target(), {0, 0, vertexPush, 128 - vertexPush}
    };
    fragment.useCache = false;
    const auto pixelResult = ShaderRecompiler::Recompile(fragment);
    const std::array<CompiledShader, 2> shaders{{
        {ShaderStage::Vertex, &vertexResult, 0}, {ShaderStage::Fragment, &pixelResult, vertexPush}
    }};
    State state{};
    state.stages = {ShaderPath::Vertex, 0, waveSize, waveSize, std::nullopt, std::nullopt};
    state.colors = {color};
    state.color = color;
    state.hasColorTarget = true;
    state.renderExtent = color.extent;
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(color.extent.height), static_cast<float>(color.extent.width), -static_cast<float>(color.extent.height), 0, 1};
    state.scissor = {{0, 0}, color.extent};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0xfu;
    state.blends = {blend};
    state.blend = blend;
    const AgcDriver::Pm4::DrawParameters draw{0, 3, 0, 1, 0, false};
    Draw(device.GetContext(), state, draw, shaders);
}

std::array<std::uint32_t, 8> ColorDescriptor(std::uint64_t address, VkExtent2D extent) {
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (50u << 20u) | (((extent.width - 1u) & 3u) << 30u),
        ((extent.width - 1u) >> 2u) | ((extent.height - 1u) << 14u),
        0xfacu | (0x1bu << 20u) | (9u << 28u), 0, 0, 0, 0
    };
}

void RemappedColorTest(const Device& device) {
    constexpr VkExtent2D depthExtent{1920, 1080};
    constexpr VkExtent2D colorExtent{1804, 732};
    const ColorTargetLayout colorLayout(colorExtent.width, colorExtent.height, ColorTileMode::RenderTarget, 4);
    const auto originalBytes = static_cast<std::size_t>(DepthSliceBytes(depthExtent, 4)) + 2u * Block;
    const auto replacementBytes = Block + colorLayout.Bytes();
    GuestBlock memory(originalBytes);
    const auto address = memory.Address() + Block;
    auto depth = Depth(address, depthExtent);
    depth.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    DepthSurfaceView(device.GetContext(), depth);
    const auto words = ColorDescriptor(address, colorExtent);
    const auto view = DecodeTextureResource(words);
    Require(Refused(device.GetContext(), view), "an incompatible color view bypassed an active depth surface");
    memory.Remap(replacementBytes);
    std::memset(memory.data, 0x12, replacementBytes);
    AgcDriver::GuestMemory::MarkWritten(memory.Address(), replacementBytes);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    ColorTarget color{};
    color.address = color.surfaceAddress = address;
    color.extent = color.surfaceExtent = colorExtent;
    color.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    color.bytes = colorLayout.Bytes();
    color.componentMapping = 0xe4u;
    color.tileMode = ColorTileMode::RenderTarget;
    color.elementBytes = 4;
    DrawColor(device, color);
    Require(!DepthSurfaceAt(address), "a real color draw over remapped guest memory left the stale depth surface active");
    Require(Sample(device.GetContext(), view) == nullptr, "a remapped color image was still served from the old depth surface");
    const auto pending = StorageTexture::FindPending(address, color.bytes);
    Require(pending != nullptr, "the color draw did not retain its resident image before publication");
    const auto texture = CachedSampledTexture(device.GetContext(), words);
    Require(texture != nullptr && texture->ViewFormat() == color.format && texture->SharedStorageSource() == pending,
        "the remapped color texture did not reuse the draw's pending resident image");
    FlushCachedTextures(device.GetContext().device);
    Require(!AgcDriver::GuestMemory::Accessible(memory.data + replacementBytes, 1), "color publication made the old allocation tail readable");
    Require(memory.data[Block - 1] == std::byte{0x12}, "the color draw overwrote the allocation prefix");
    for (std::uint32_t y = 0; y < colorExtent.height; ++y) {
        for (std::uint32_t x = 0; x < colorExtent.width; ++x) {
            const auto offset = Block + colorLayout.Offset(x, y);
            for (std::uint32_t channel = 0; channel < 4; ++channel) {
                if (memory.data[offset + channel] != std::byte{0xff}) {
                    std::uint32_t packed = 0;
                    std::memcpy(&packed, memory.data + offset, sizeof(packed));
                    throw std::runtime_error("the real color draw did not publish its packed white pixel: x=" + std::to_string(x) +
                        " y=" + std::to_string(y) + " byte=" + std::to_string(channel) + " offset=" + std::to_string(offset) +
                        " actual=" + std::to_string(std::to_integer<unsigned>(memory.data[offset + channel])) + " packed=" + std::to_string(packed));
                }
            }
        }
    }
    ClearCachedTextures(device.GetContext().device);
}

void SampledSnapshotTest(const Context& context) {
    ClearCachedTextures(context.device);
    GuestBlock memory(Block);
    struct Cleanup {
        VkDevice device;
        ~Cleanup() { ClearCachedTextures(device); }
    } cleanup{context.device};
    const auto address = memory.Address();
    const std::array<std::uint32_t, 8> words{
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (Format32Float << 20u) | (3u << 30u),
        15u | (63u << 14u), 0xfacu | (9u << 28u), 0, 0, 0, 0
    };
    const auto fill = [&](float value) {
        std::fill_n(reinterpret_cast<std::uint32_t*>(memory.data), Block / sizeof(std::uint32_t), std::bit_cast<std::uint32_t>(value));
        AgcDriver::GuestMemory::BumpCollectEpoch();
    };
    const auto lookup = [&] { return CachedSampledTexture(context, words); };
    SampleProgram sampler(context);
    fill(0.25f);
    auto texture = lookup();
    Require(!texture->ViewsStorageImage(), "the sampled snapshot oracle unexpectedly views a storage image");
    Require(sampler.RedBits(*texture) == std::bit_cast<std::uint32_t>(0.25f), "the initial sampled snapshot has wrong GPU pixels");
    if (AgcDriver::GuestMemory::Watched(address, Block)) {
        Require(lookup() == texture, "an unchanged watched sampled snapshot was rebuilt");
        fill(0.5f);
        auto changed = lookup();
        Require(changed != texture && sampler.RedBits(*changed) == std::bit_cast<std::uint32_t>(0.5f),
            "a CPU write to a watched sampled snapshot did not replace its GPU pixels");
        texture = std::move(changed);
        AgcDriver::GuestMemory::Unwatch(address, Block);
        AgcDriver::GuestMemory::BumpCollectEpoch();
        Require(!AgcDriver::GuestMemory::Watched(address, Block), "the sampled snapshot oracle remains watched");
        changed = lookup();
        Require(changed != texture && sampler.RedBits(*changed) == std::bit_cast<std::uint32_t>(0.5f),
            "loss of write watching did not rebuild the sampled snapshot with its current GPU pixels");
        texture = std::move(changed);
    }
    Require(lookup() == texture, "an unchanged untracked sampled snapshot was rebuilt");
    fill(0.75f);
    const auto changed = lookup();
    Require(changed != texture && sampler.RedBits(*changed) == std::bit_cast<std::uint32_t>(0.75f),
        "an untracked sampled snapshot reused stale GPU pixels after a CPU write");
    Require(lookup() == changed, "an unchanged untracked sampled snapshot did not reuse its replacement");
}

void ReadStorageImage(const Context& context, const StorageTexture& storage, Buffer& readback) {
    CommandBatch batch(context);
    const auto commands = batch.Handle();
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, storage.ImageLayers()};
    copy.imageExtent = {storage.Descriptor().width, storage.Descriptor().height, 1};
    context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(commands, storage.Image(), VK_IMAGE_LAYOUT_GENERAL, readback.Handle(), 1, &copy);
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    batch.SubmitAndWait();
}

void WriteStorageImage(const Context& context, StorageTexture& storage, Buffer& pixels, std::uint32_t layer = 0) {
    CommandBatch batch(context);
    const auto commands = batch.Handle();
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
    copy.imageExtent = {storage.Descriptor().width, storage.Descriptor().height, 1};
    context.Function<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(commands, pixels.Handle(), storage.Image(), VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT);
    batch.SubmitAndWait();
    storage.MarkDirty();
}

void CanonicalDepthAliasTest(const Context& context, bool d16, bool pendingAlias, bool combined = false, bool stencilAlias = false) {
    const VkExtent2D extent{d16 ? 272u : 144u, 144u};
    const auto elementBytes = d16 ? 2u : 4u;
    const auto sliceBytes = static_cast<std::size_t>(DepthSliceBytes(extent, elementBytes));
    constexpr std::uint32_t slice = 3u;
    constexpr std::uint32_t xorBytes = 0x900u;
    const auto totalBytes = (slice + 1u) * sliceBytes;
    GuestBlock memory(totalBytes);
    const auto stencilSliceBytes = static_cast<std::size_t>(DepthSliceBytes(extent, 1u));
    GuestBlock stencilMemory(combined ? (slice + 1u) * stencilSliceBytes : Block);
    std::vector<std::byte> expectedStencil((slice + 1u) * stencilSliceBytes, std::byte{0xcd});
    struct Cleanup {
        VkDevice device;
        ~Cleanup() { ClearCachedTextures(device); ClearDepthSurfaces(device); }
    } cleanup{context.device};
    std::vector<std::byte> expected(totalBytes, std::byte{0xcd});
    Buffer pixels(context, extent.width * extent.height * elementBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    const auto* equation = FindTextureSwizzleEquation(24u, elementBytes);
    Require(equation != nullptr, "the canonical depth oracle has no swizzle equation");
    const auto blockWidth = d16 ? 256u : 128u;
    const auto blocksPerRow = (extent.width + blockWidth - 1u) / blockWidth;
    const auto guestOffset = [&](std::uint32_t x, std::uint32_t y) {
        std::uint32_t offset = 0;
        for (std::uint32_t bit = 0; bit < equation->bits.size(); ++bit) {
            const auto mask = equation->bits[bit];
            const auto coordinates = (x & mask & 0xfffu) ^ ((y << 12u) & mask & 0xfff000u) ^ ((slice << 24u) & mask & 0xff000000u);
            offset |= (std::popcount(coordinates) & 1u) << bit;
        }
        const auto block = (y / 128u) * blocksPerRow + x / blockWidth;
        return slice * sliceBytes + block * Block + (offset ^ xorBytes);
    };
    for (std::uint32_t y = 0; y < extent.height; ++y) {
        for (std::uint32_t x = 0; x < extent.width; ++x) {
            const auto pixel = static_cast<std::size_t>(y) * extent.width + x;
            const auto value = d16 ? pendingAlias ? 0xffffu : (0x2000u + (x * 17u + y * 31u) % 0x5000u) :
                std::bit_cast<std::uint32_t>(pendingAlias ? 1.0f : 0.25f + static_cast<float>((x * 17u + y * 31u) % 128u) / 512.0f);
            std::memcpy(expected.data() + guestOffset(x, y), &value, elementBytes);
            std::memcpy(pixels.Bytes().data() + pixel * elementBytes, &value, elementBytes);
        }
    }
    std::memcpy(memory.data, expected.data(), expected.size());
    AgcDriver::GuestMemory::MarkWritten(memory.Address(), totalBytes);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    auto target = Depth(memory.Address() + slice * sliceBytes, extent);
    target.format = d16 ? VK_FORMAT_D16_UNORM : VK_FORMAT_D32_SFLOAT;
    target.clearDepth = pendingAlias ? 1.0f : 0.5f;
    target.zInfo = (24u << 4u) | (d16 ? 1u : 3u);
    target.encodedSurfaceAddress = memory.Address() | xorBytes;
    target.depthSlice = slice;
    if (combined) {
        target.format = d16 ? VK_FORMAT_D16_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
        target.stencilAddress = stencilMemory.Address() + slice * stencilSliceBytes;
        target.encodedStencilAddress = stencilMemory.Address();
        target.stencilInfo = (24u << 4u) | 1u;
        target.clearStencil = 0x7fu;
        std::memcpy(stencilMemory.data, expectedStencil.data(), expectedStencil.size());
        AgcDriver::GuestMemory::MarkWritten(stencilMemory.Address(), expectedStencil.size());
        const auto* stencilEquation = FindTextureSwizzleEquation(24u, 1u);
        Require(stencilEquation != nullptr, "the stencil oracle has no swizzle equation");
        const auto stencilBlocksPerRow = (extent.width + 255u) / 256u;
        for (std::uint32_t y = 0; y < extent.height; ++y) {
            for (std::uint32_t x = 0; x < extent.width; ++x) {
                std::uint32_t offset = 0;
                for (std::uint32_t bit = 0; bit < stencilEquation->bits.size(); ++bit) {
                    const auto mask = stencilEquation->bits[bit];
                    const auto coordinates = (x & mask & 0xfffu) ^ ((y << 12u) & mask & 0xfff000u) ^ ((slice << 24u) & mask & 0xff000000u);
                    offset |= (std::popcount(coordinates) & 1u) << bit;
                }
                const auto block = (y / 256u) * stencilBlocksPerRow + x / 256u;
                expectedStencil[slice * stencilSliceBytes + block * Block + offset] = std::byte{0x7f};
            }
        }
    }
    auto aliasResource = View(16, 16, stencilAlias ? target.stencilAddress : target.address);
    auto& aliasExpected = stencilAlias ? expectedStencil : expected;
    const auto aliasSliceBytes = stencilAlias ? stencilSliceBytes : sliceBytes;
    aliasResource.format = d16 ? 20u : 29u;
    const auto aliasPitch = DescribeSurface(aliasResource).mips.front().pitchBytes;
    const auto aliasRowBytes = aliasResource.width * 4u;
    auto alias = CachedStorageSurface(context, aliasResource);
    if (pendingAlias) {
        Buffer oldPixels(context, alias->GuestBytes(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        std::fill(oldPixels.Bytes().begin(), oldPixels.Bytes().end(), std::byte{0});
        WriteStorageImage(context, *alias, oldPixels);
    }
    DepthSurfaceView(context, target);
    auto original = View(extent.width, extent.height, memory.Address());
    original.format = d16 ? 7u : 22u;
    original.tileMode = TextureTileMode::kZ64KBX;
    original.pipeBankXor = xorBytes;
    original.dimension = TextureDimension::k2DArray;
    original.depthOrLastArray = slice;
    if (!pendingAlias) {
        auto writer = CachedStorageSurface(context, original);
        WriteStorageImage(context, *writer, pixels, slice);
        DepthSurfaceView(context, target);
    }
    alias = CachedStorageSurface(context, aliasResource);
    Require(!DepthSurfaceAt(target.address), "the incompatible alias did not retire its published native depth surface");
    AgcDriver::GuestMemory::FlushGpuWrites(memory.Address(), totalBytes);
    Require(std::equal(expected.begin(), expected.end(), memory.data), "canonical depth publication changed pixel bits, another slice or tile padding");
    if (combined) {
        AgcDriver::GuestMemory::FlushGpuWrites(stencilMemory.Address(), expectedStencil.size());
        Require(std::equal(expectedStencil.begin(), expectedStencil.end(), stencilMemory.data),
            "canonical depth publication lost stencil pixels, another slice or padding");
    }
    Buffer aliasReadback(context, aliasResource.width * aliasResource.height * 4u, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    ReadStorageImage(context, *alias, aliasReadback);
    for (std::uint32_t y = 0; y < aliasResource.height; ++y) {
        Require(std::memcmp(aliasReadback.Bytes().data() + y * aliasRowBytes, aliasExpected.data() + slice * aliasSliceBytes + y * aliasPitch, aliasRowBytes) == 0,
            "the incompatible alias kept older GPU pixels after canonical depth publication");
    }
    Buffer changed(context, aliasReadback.Bytes().size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::fill(changed.Bytes().begin(), changed.Bytes().end(), std::byte{0});
    WriteStorageImage(context, *alias, changed);
    for (std::uint32_t y = 0; y < aliasResource.height; ++y) {
        std::fill_n(aliasExpected.data() + slice * aliasSliceBytes + y * aliasPitch, aliasRowBytes, std::byte{0});
    }
    DepthSurfaceView(context, target);
    auto restored = CachedStorageSurface(context, original);
    Buffer restoredReadback(context, (slice + 1u) * pixels.Bytes().size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    ReadStorageImage(context, *restored, restoredReadback);
    for (std::uint32_t y = 0; y < extent.height; ++y) {
        for (std::uint32_t x = 0; x < extent.width; ++x) {
            const auto pixel = (static_cast<std::size_t>(slice) * extent.height + y) * extent.width + x;
            Require(std::memcmp(restoredReadback.Bytes().data() + pixel * elementBytes, expected.data() + guestOffset(x, y), elementBytes) == 0,
                "rebinding published depth restored incorrect GPU pixel bits");
        }
    }
    AgcDriver::GuestMemory::FlushGpuWrites(memory.Address(), totalBytes);
    Require(std::equal(expected.begin(), expected.end(), memory.data), "rebinding published depth resurrected pixels from before the alias write");
    if (combined) {
        auto stencilResource = original;
        stencilResource.baseAddress = stencilMemory.Address();
        stencilResource.format = 5u;
        stencilResource.pipeBankXor = 0;
        auto restoredStencil = CachedStorageSurface(context, stencilResource);
        Buffer readback(context, (slice + 1u) * extent.width * extent.height, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        ReadStorageImage(context, *restoredStencil, readback);
        const auto layerPixels = static_cast<std::size_t>(extent.width) * extent.height;
        const auto* stencilEquation = FindTextureSwizzleEquation(24u, 1u);
        const auto stencilBlocksPerRow = (extent.width + 255u) / 256u;
        for (std::uint32_t y = 0; y < extent.height; ++y) {
            for (std::uint32_t x = 0; x < extent.width; ++x) {
                std::uint32_t offset = 0;
                for (std::uint32_t bit = 0; bit < stencilEquation->bits.size(); ++bit) {
                    const auto mask = stencilEquation->bits[bit];
                    const auto coordinates = (x & mask & 0xfffu) ^ ((y << 12u) & mask & 0xfff000u) ^ ((slice << 24u) & mask & 0xff000000u);
                    offset |= (std::popcount(coordinates) & 1u) << bit;
                }
                const auto block = (y / 256u) * stencilBlocksPerRow + x / 256u;
                Require(readback.Bytes()[slice * layerPixels + y * extent.width + x] == expectedStencil[slice * stencilSliceBytes + block * Block + offset],
                    "rebinding a published combined surface lost its stencil plane");
            }
        }
    }
    if (stencilAlias) return;
    std::fill_n(memory.data + slice * sliceBytes, sliceBytes, std::byte{0x5a});
    AgcDriver::GuestMemory::MarkWritten(target.address, sliceBytes);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    alias = CachedStorageSurface(context, aliasResource);
    ReadStorageImage(context, *alias, aliasReadback);
    Require(std::all_of(aliasReadback.Bytes().begin(), aliasReadback.Bytes().end(), [](auto value) { return value == std::byte{0x5a}; }),
        "canonical publication replaced newer guest pixels with native depth pixels");
    DepthSurfaceView(context, target);
    AgcDriver::GuestMemory::Unwatch(target.address, sliceBytes);
    std::fill_n(memory.data + slice * sliceBytes, sliceBytes, std::byte{0x6b});
    AgcDriver::GuestMemory::BumpCollectEpoch();
    bool refused = false;
    try { CachedStorageSurface(context, aliasResource); }
    catch (const std::runtime_error& error) { refused = std::string_view(error.what()).find("not implemented") != std::string_view::npos; }
    Require(refused && DepthSurfaceAt(target.address), "unwatched depth pixels were published without an unchanged-memory proof");
    Require(std::all_of(memory.data + slice * sliceBytes, memory.data + totalBytes, [](auto value) { return value == std::byte{0x6b}; }),
        "canonical publication overwrote untracked guest pixels");
}

void StorageRoundTripTest(const Context& context, bool keepCached, bool clearPendingWriter = false, bool skipClearBind = false) {
    GuestBlock memory(2 * Block);
    struct Cleanup {
        VkDevice device;
        ~Cleanup() { ClearCachedTextures(device); ClearDepthSurfaces(device); }
    } cleanup{context.device};
    std::memset(memory.data, 0xcd, 2 * Block);
    AgcDriver::GuestMemory::MarkWritten(memory.Address(), 2 * Block);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    auto target = Depth(memory.Address());
    target.clearDepth = clearPendingWriter && !skipClearBind ? 0.5f : 0.25f;
    const auto initialDepth = target.clearDepth;
    if (keepCached) target.htileAddress = memory.Address() + Block;
    DepthSurfaceView(context, target);
    auto resource = View(Extent.width, Extent.height, memory.Address());
    resource.tileMode = TextureTileMode::kZ64KBX;
    SampleProgram sampler(context);
    const auto initial = Sample(context, resource);
    Require(initial != nullptr && sampler.RedBits(*initial) == std::bit_cast<std::uint32_t>(target.clearDepth),
        "the depth round trip did not start with the GPU clear value");
    if (skipClearBind) {
        target.clearDepth = 0.5f;
        DepthSurfaceView(context, target);
    }

    auto writer = CachedStorageSurface(context, resource);
    {
        Texture sampledStorage(context, writer, resource, Identity);
        Require(sampler.RedBits(sampledStorage) == std::bit_cast<std::uint32_t>(initialDepth),
            "the storage image did not receive the depth clear's pixel bits");
    }
    constexpr float writtenDepth = 0.75f;
    {
        CommandBatch batch(context);
        const auto commands = batch.Handle();
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkClearColorValue clear{};
        clear.float32[0] = writtenDepth;
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        context.Function<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(commands, writer->Image(), VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT);
        batch.SubmitAndWait();
    }
    writer->MarkDirty();
    const std::weak_ptr<StorageTexture> retained = writer;
    if (!keepCached) {
        ClearCachedTextures(context.device);
        Check(context.Function<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(context.device), "vkDeviceWaitIdle");
        writer.reset();
        Require(!retained.expired(), "cache eviction and the last external reference destroyed the depth surface's storage writer");
    }
    auto rebound = initial;
    if (!clearPendingWriter) {
        DepthSurfaceView(context, target);
        rebound = Sample(context, resource);
        Require(rebound != nullptr && sampler.RedBits(*rebound) == std::bit_cast<std::uint32_t>(writtenDepth),
            "rebinding depth did not import the retained storage writer's GPU pixel bits");
    }
    if (!keepCached) Require(retained.expired(), "a completed depth rebind retained the consumed storage writer");
    if (keepCached) {
        Require(writer->Cached(), "the depth writer left the storage cache before the coherence check");
        target.clearDepth = 0.5f;
        if (!clearPendingWriter) DepthSurfaceView(context, target);
        const auto beforeClear = AgcDriver::GuestMemory::CollectWrites(memory.Address(), DepthSliceBytes(Extent, 4));
        const auto metadataBytes = static_cast<std::size_t>(HtileSliceBytes(Extent));
        constexpr std::uint32_t pattern = 0x0003fff0u;
        std::fill_n(reinterpret_cast<std::uint32_t*>(memory.data + Block), metadataBytes / sizeof(pattern), pattern);
        AgcDriver::GuestMemory::MarkWritten(target.htileAddress, metadataBytes);
        AgcDriver::GuestMemory::BumpCollectEpoch();
        NoteDepthMetadataFill(context.device, target.htileAddress, metadataBytes, pattern);
        if (!skipClearBind) {
            DepthSurfaceView(context, target);
            Require(sampler.RedBits(*rebound) == std::bit_cast<std::uint32_t>(target.clearDepth), "the HTILE-only clear did not update native depth pixels");
        }
        if (!clearPendingWriter) Require(!AgcDriver::GuestMemory::WrittenSince(memory.Address(), DepthSliceBytes(Extent, 4), beforeClear),
            "an HTILE-only clear changed the depth plane's guest write generation");
    }

    auto replacement = View(Extent.width / 4, Extent.height / 4, memory.Address());
    replacement.format = 29; // R16G16_SFLOAT in a smaller linear buffer at the reused address.
    bool refused = false;
    try {
        CachedStorageSurface(context, replacement);
    } catch (const std::runtime_error& error) {
        refused = std::string(error.what()).find("is not implemented") != std::string::npos;
    }
    Require(refused && DepthSurfaceAt(target.address), "an incompatible storage view discarded current GPU depth pixels without newer guest writes");
    if (skipClearBind) rebound = Sample(context, resource);
    if (keepCached) Require(sampler.RedBits(*rebound) == std::bit_cast<std::uint32_t>(target.clearDepth),
        "an older cached storage writer replaced the newer native depth clear");
    constexpr std::uint32_t replacementPixel = 0x40003c00u;
    std::fill_n(reinterpret_cast<std::uint32_t*>(memory.data), DepthSliceBytes(Extent, 4) / sizeof(replacementPixel), replacementPixel);
    AgcDriver::GuestMemory::MarkWritten(memory.Address(), DepthSliceBytes(Extent, 4));
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto reused = CachedStorageSurface(context, replacement);
    Require(reused != nullptr && !DepthSurfaceAt(target.address), "newer guest writes did not retire an incompatible depth image at a reused address");
    Buffer replacementReadback(context, replacement.width * replacement.height * sizeof(replacementPixel), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    ReadStorageImage(context, *reused, replacementReadback);
    for (std::size_t offset = 0; offset < replacementReadback.Bytes().size(); offset += sizeof(replacementPixel)) {
        std::uint32_t value;
        std::memcpy(&value, replacementReadback.Bytes().data() + offset, sizeof(value));
        Require(value == replacementPixel, "retiring a reused depth address replaced newer guest pixels with stale depth bits");
    }
}

void StorageArrayRoundTripTest(const Context& context, bool firstLayerResident, bool keepCached = false) {
    constexpr std::uint32_t layers = 3;
    constexpr std::uint32_t texels = Extent.width * Extent.height;
    const auto sliceBytes = DepthSliceBytes(Extent, 2);
    GuestBlock memory(layers * sliceBytes);
    struct Cleanup {
        VkDevice device;
        ~Cleanup() { ClearCachedTextures(device); ClearDepthSurfaces(device); }
    } cleanup{context.device};
    std::memset(memory.data, 0x33, layers * sliceBytes);
    AgcDriver::GuestMemory::MarkWritten(memory.Address(), layers * sliceBytes);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    auto first = Depth(memory.Address());
    first.format = VK_FORMAT_D16_UNORM;
    first.clearDepth = 0.0f;
    auto last = first;
    last.address += 2 * sliceBytes;
    last.clearDepth = 1.0f;
    if (firstLayerResident) DepthSurfaceView(context, first);
    DepthSurfaceView(context, last);
    if (firstLayerResident) {
        auto padding = View(Extent.width, Extent.height, memory.Address() + sliceBytes / 2);
        const auto unrelated = CachedStorageSurface(context, padding);
        Require(unrelated != nullptr && DepthSurfaceAt(first.address), "a storage subrange was mistaken for a native depth array layer");
    }
    auto resource = View(Extent.width, Extent.height, memory.Address());
    resource.format = 7; // R16_UNORM, sharing the D16 depth plane's native pixel bits.
    resource.dimension = TextureDimension::k2DArray;
    resource.depthOrLastArray = layers - 1;
    resource.tileMode = TextureTileMode::kZ64KBX;
    if (!firstLayerResident) {
        auto volume = resource;
        volume.dimension = TextureDimension::k3D;
        bool refused = false;
        try {
            CachedStorageSurface(context, volume);
        } catch (const std::runtime_error& error) {
            refused = std::string(error.what()).find("requires a 2D image or array") != std::string::npos;
        }
        Require(refused && DepthSurfaceAt(last.address), "a volume view bypassed its last resident depth slice");
        refused = false;
        try {
            DepthSurfaceAt(volume);
        } catch (const std::runtime_error& error) {
            refused = std::string(error.what()).find("requires a 2D image or array") != std::string::npos;
        }
        Require(refused && DepthSurfaceAt(last.address), "a volume writer could retire its unsupported depth slice before validation");
    }
    auto writer = CachedStorageSurface(context, resource);
    Buffer readback(context, layers * texels * sizeof(std::uint16_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    ReadStorageImage(context, *writer, readback);
    const std::array<std::uint16_t, layers> expected{firstLayerResident ? std::uint16_t{0} : std::uint16_t{0x3333}, 0x3333, 65535};
    for (std::uint32_t layer = 0; layer < layers; ++layer) {
        for (std::uint32_t pixel = 0; pixel < texels; ++pixel) {
            std::uint16_t value;
            std::memcpy(&value, readback.Bytes().data() + (layer * texels + pixel) * sizeof(value), sizeof(value));
            Require(value == expected[layer], "depth array seeding changed or duplicated a slice: layer=" + std::to_string(layer) + " value=" + std::to_string(value));
        }
    }
    {
        CommandBatch batch(context);
        const auto commands = batch.Handle();
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkClearColorValue clear{};
        clear.float32[0] = 0.5f;
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layers - 1, 1};
        context.Function<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(commands, writer->Image(), VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
        if (!firstLayerResident && !keepCached) {
            clear.float32[0] = 0.0f;
            const VkImageSubresourceRange firstRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            context.Function<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(commands, writer->Image(), VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &firstRange);
        }
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT);
        batch.SubmitAndWait();
    }
    writer->MarkDirty();
    const std::weak_ptr<StorageTexture> retained = writer;
    if (!keepCached) {
        ClearCachedTextures(context.device);
        Check(context.Function<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(context.device), "vkDeviceWaitIdle");
        writer.reset();
        Require(!retained.expired(), "cache eviction lost the depth array's storage writer");
    }
    const auto firstGeneration = AgcDriver::GuestMemory::CollectWritesUncached(first.address, sliceBytes);
    DepthSurfaceView(context, last);
    if (keepCached) {
        Require(!AgcDriver::GuestMemory::WrittenSince(first.address, sliceBytes, firstGeneration),
            "publishing the last depth array slice stamped an unconsumed first slice");
        Require(StorageTexture::FindPending(memory.Address(), layers * sliceBytes) == writer,
            "publishing one depth array slice discarded the other pending slices");
    }
    auto lastView = resource;
    lastView.baseAddress = last.address;
    lastView.dimension = TextureDimension::k2D;
    lastView.depthOrLastArray = 0;
    SampleProgram sampler(context);
    const auto rebound = Sample(context, lastView);
    Require(rebound != nullptr && std::abs(std::bit_cast<float>(sampler.RedBits(*rebound)) - 0.5f) <= 1.0f / 65535.0f,
        "depth array rebind imported another layer's pixel bits");
    if (firstLayerResident) {
        DepthSurfaceView(context, first);
        lastView.baseAddress = first.address;
        const auto preserved = Sample(context, lastView);
        Require(preserved != nullptr && sampler.RedBits(*preserved) == std::bit_cast<std::uint32_t>(0.0f),
            "writing the last depth array layer changed the first layer");
    }
    if (!keepCached) Require(retained.expired(), "consuming all depth array slices retained the storage writer");
    if (!firstLayerResident && !keepCached) {
        const auto restored = CachedStorageSurface(context, resource);
        ReadStorageImage(context, *restored, readback);
        for (std::uint32_t pixel = 0; pixel < texels; ++pixel) {
            std::uint16_t value;
            std::memcpy(&value, readback.Bytes().data() + pixel * sizeof(value), sizeof(value));
            Require(value == 0, "releasing the last depth owner discarded another storage array layer's GPU writes");
        }
    }
}

void MultisampleRefusalTest(const Context& context) {
    if (!context.sampleLocations || (context.sampleLocationProperties.sampleLocationSampleCounts & VK_SAMPLE_COUNT_8_BIT) == 0 ||
        (context.limits.framebufferDepthSampleCounts & VK_SAMPLE_COUNT_8_BIT) == 0 || context.sampleLocationProperties.sampleLocationSubPixelBits < 4) {
        std::puts("programmable eight-sample depth locations are unavailable: multisample sampling rejection is not exercised");
        return;
    }
    GuestBlock memory(4u * Block);
    auto target = Depth(memory.Address());
    target.samples = VK_SAMPLE_COUNT_8_BIT;
    target.sampleLocations = {
        {1.0f / 16, 5.0f / 16}, {15.0f / 16, 11.0f / 16}, {9.0f / 16, 3.0f / 16}, {3.0f / 16, 13.0f / 16},
        {5.0f / 16, 1.0f / 16}, {11.0f / 16, 15.0f / 16}, {13.0f / 16, 7.0f / 16}, {7.0f / 16, 9.0f / 16}
    };
    DepthSurfaceView(context, target);
    memory.data[8] = std::byte{0x78};
    AgcDriver::GuestMemory::MarkWritten(memory.Address() + 8, 1);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    for (const auto width : {Extent.width, 2u * Extent.width}) {
        bool refused = false;
        try {
            Sample(context, View(width, Extent.height, memory.Address()));
        } catch (const std::runtime_error& error) {
            refused = std::string(error.what()).find("requires multisampled texture materialization") != std::string::npos;
        }
        Require(refused && DepthSurfaceAt(memory.Address()), "an active multisampled depth view fell back after a guest write");
    }
}

void Run(const Context& context) {
    const auto wide = View(2 * Extent.width, Extent.height);
    const auto exact = View(Extent.width, Extent.height);
    const auto depthBytes = DepthSliceBytes(Extent, 4);

    DepthSurfaceView(context, Depth());
    Require(DepthSurfaceAt(DepthAddress), "a bound depth surface is not found at its address");
    Require(Refused(context, wide), "depth, then a view of another extent: the live depth surface did not refuse it");
    Require(Sample(context, exact) != nullptr, "depth, then its own view: the depth surface did not serve it");

    RetireDepthSurfaces(context.device, DepthAddress + depthBytes, 0x10000);
    Require(Refused(context, wide), "a write past the depth plane retired the depth surface");

    RetireDepthSurfaces(context.device, DepthAddress + 0x100, 0x1000);
    Require(!DepthSurfaceAt(DepthAddress), "a color target over the depth plane left the depth surface live");
    Require(Sample(context, wide) == nullptr, "depth, then a color target, then a view of another extent: the view was not left to guest memory");
    Require(Sample(context, exact) == nullptr, "depth, then a color target, then its own view: the retired depth surface served it");

    DepthSurfaceView(context, Depth());
    Require(DepthSurfaceAt(DepthAddress), "binding the depth target again did not bring the depth surface back");
    Require(Refused(context, wide), "depth, color target, depth again, then a view of another extent: the live depth surface did not refuse it");

    const auto shared = DepthAddress + 0x1000000;
    const VkExtent2D wideExtent{2 * Extent.width, Extent.height};
    const auto sharedWide = View(wideExtent.width, wideExtent.height, shared);
    const auto sharedExact = View(Extent.width, Extent.height, shared);
    DepthSurfaceView(context, Depth(shared, wideExtent));
    DepthSurfaceView(context, Depth(shared));
    DepthSurfaceView(context, Depth(shared, wideExtent));
    Require(Sample(context, sharedWide) != nullptr, "two depth surfaces at one address, the older one bound last, then its own view: it did not serve it");
    Require(Refused(context, sharedExact), "two depth surfaces at one address, the older one bound last, then the other's view: the live depth surface did not refuse it");
    DepthSurfaceView(context, Depth(shared));
    Require(Sample(context, sharedExact) != nullptr, "two depth surfaces at one address, the newer one bound last, then its own view: it did not serve it");
    Require(Refused(context, sharedWide), "two depth surfaces at one address, the newer one bound last, then the other's view: the live depth surface did not refuse it");

    if (!AgcDriver::GuestMemory::WriteWatched()) {
        Require(Refused(context, wide), "without write watching, a view of another extent was not refused");
        std::puts("guest memory is not write-watched here: the CPU write order is not exercised");
        return;
    }

    GuestBlock block(2 * Block);
    auto* memory = reinterpret_cast<volatile std::uint8_t*>(block.data);
    const auto watched = reinterpret_cast<std::uint64_t>(memory);
    Require(AgcDriver::GuestMemory::Watched(watched, 2 * Block), "the test block is not watched");
    Require(depthBytes <= 2 * Block, "the depth plane does not fit the watched block");
    const auto watchedWide = View(2 * Extent.width, Extent.height, watched);
    const auto watchedExact = View(Extent.width, Extent.height, watched);
    AgcDriver::GuestMemory::CollectWrites(watched, 2 * Block);

    DepthSurfaceView(context, Depth(watched));
    AgcDriver::GuestMemory::BumpCollectEpoch();
    Require(Refused(context, watchedWide), "depth, then a view of another extent: the live depth surface over watched memory did not refuse it");

    memory[8] = 0x5a;
    AgcDriver::GuestMemory::BumpCollectEpoch();
    Require(Sample(context, watchedExact) != nullptr, "depth, then a CPU write, then its own view: the depth surface did not serve it");
    Require(Sample(context, watchedWide) == nullptr, "depth, then a CPU write, then a view of another extent: the view was not left to guest memory");
    Require(!DepthSurfaceAt(watched), "depth, then a CPU write, then a view of another extent: the depth surface stayed live");

    memory[16] = 0xa5;
    AgcDriver::GuestMemory::BumpCollectEpoch();
    DepthSurfaceView(context, Depth(watched));
    AgcDriver::GuestMemory::BumpCollectEpoch();
    Require(Refused(context, watchedWide), "a CPU write, then depth, then a view of another extent: the live depth surface did not refuse it");

    AgcDriver::GuestMemory::MarkWritten(watched + Block, 4);
    Require(Refused(context, watchedWide), "a driver store past the depth plane retired the depth surface");
    AgcDriver::GuestMemory::MarkWritten(watched + 0x100, 4);
    Require(Sample(context, watchedWide) == nullptr, "depth, then a driver store, then a view of another extent: the view was not left to guest memory");
}

}

int main(int argc, char** argv) {
    try {
        std::unique_ptr<Device> device;
        try {
            device = std::make_unique<Device>();
        } catch (const std::exception& error) {
            if (std::getenv("ANYPS5_REQUIRE_VULKAN") != nullptr) throw;
            std::printf("skipped, no usable Vulkan device: %s\n", error.what());
            return VulkanTestSkipped;
        }
        std::lock_guard gpuLock(AgcDriver::GuestMemory::GpuMutex());
        if (argc == 2 && std::string_view(argv[1]) == "--canonical-only") {
            CanonicalDepthAliasTest(device->GetContext(), false, false);
            CanonicalDepthAliasTest(device->GetContext(), true, false);
            CanonicalDepthAliasTest(device->GetContext(), false, true);
            CanonicalDepthAliasTest(device->GetContext(), true, true);
            CanonicalDepthAliasTest(device->GetContext(), false, false, true);
            CanonicalDepthAliasTest(device->GetContext(), false, true, true);
            CanonicalDepthAliasTest(device->GetContext(), false, false, true, true);
            std::puts("canonical depth and stencil alias tests passed");
            return 0;
        }
        Run(device->GetContext());
        RemappedColorTest(*device);
        SampledSnapshotTest(device->GetContext());
        StorageRoundTripTest(device->GetContext(), false);
        StorageRoundTripTest(device->GetContext(), true);
        StorageRoundTripTest(device->GetContext(), true, true);
        StorageRoundTripTest(device->GetContext(), true, true, true);
        CanonicalDepthAliasTest(device->GetContext(), false, false);
        CanonicalDepthAliasTest(device->GetContext(), true, false);
        CanonicalDepthAliasTest(device->GetContext(), false, true);
        CanonicalDepthAliasTest(device->GetContext(), true, true);
        CanonicalDepthAliasTest(device->GetContext(), false, false, true);
        CanonicalDepthAliasTest(device->GetContext(), false, true, true);
        StorageArrayRoundTripTest(device->GetContext(), true);
        StorageArrayRoundTripTest(device->GetContext(), false);
        StorageArrayRoundTripTest(device->GetContext(), true, true);
        MultisampleRefusalTest(device->GetContext());
        std::puts("depth surface last writer tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
