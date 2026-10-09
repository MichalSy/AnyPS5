#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
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
#include <cstddef>
#include <cmath>
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
            context.sampleRateShading = features.sampleRateShading;
            capabilities.push_back(static_cast<std::uint32_t>(spv::CapabilityShader));
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
        mutation.Add(data, bytes, true, true);
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
            samplerInfo.magFilter = VK_FILTER_NEAREST;
            samplerInfo.minFilter = VK_FILTER_NEAREST;
            samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
            Check(context.Function<PFN_vkCreateSampler>("vkCreateSampler")(context.device, &samplerInfo, nullptr, &sampler), "vkCreateSampler");
            const VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
            VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
            poolInfo.maxSets = 1;
            poolInfo.poolSizeCount = 2;
            poolInfo.pPoolSizes = sizes;
            Check(context.Function<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(context.device, &poolInfo, nullptr, &pool), "vkCreateDescriptorPool");
        } catch (...) {
            release();
            throw;
        }
    }
    ~SampleProgram() { release(); }
    SampleProgram(const SampleProgram&) = delete;
    SampleProgram& operator=(const SampleProgram&) = delete;

    float Red(const Texture& texture, float lod) {
        VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocateInfo.descriptorPool = pool;
        allocateInfo.descriptorSetCount = 1;
        allocateInfo.pSetLayouts = &setLayout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        Check(context.Function<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(context.device, &allocateInfo, &set), "vkAllocateDescriptorSets");
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
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        context.Function<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        context.Function<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, nullptr);
        context.Function<PFN_vkCmdPushConstants>("vkCmdPushConstants")(commands, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(lod), &lod);
        context.Function<PFN_vkCmdDispatch>("vkCmdDispatch")(commands, 1, 1, 1);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        batch.SubmitAndWait();
        Check(context.Function<PFN_vkFreeDescriptorSets>("vkFreeDescriptorSets")(context.device, pool, 1, &set), "vkFreeDescriptorSets");
        float value;
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
};

DepthTarget Target(std::uint64_t address, VkExtent2D extent, std::uint32_t mip) {
    DepthTarget target{address, 0, {std::max(extent.width >> mip, 1u), std::max(extent.height >> mip, 1u)}, VK_FORMAT_D32_SFLOAT, 1.0f, 0};
    target.surfaceExtent = extent;
    target.mipCount = 3;
    target.mip = mip;
    return target;
}

std::array<std::uint32_t, 8> Descriptor(const DepthTarget& target, std::uint32_t first, std::uint32_t last) {
    const auto extent = target.surfaceExtent;
    return {
        static_cast<std::uint32_t>(target.address >> 8u),
        static_cast<std::uint32_t>((target.address >> 40u) & 0xffu) | (Format32Float << 20u) | (((extent.width - 1u) & 3u) << 30u),
        ((extent.width - 1u) >> 2u) | ((extent.height - 1u) << 14u),
        0xfacu | (first << 12u) | (last << 16u) | (0x18u << 20u) | (9u << 28u),
        0, (target.mipCount - 1u) << 4u, 0, 0
    };
}

std::shared_ptr<Texture> Sample(const Context& context, const DepthTarget& target, std::uint32_t first, std::uint32_t last) {
    const auto words = Descriptor(target, first, last);
    return DepthSurfaceTexture(context, words, DecodeTextureResource(words), Identity);
}

alignas(256) constexpr std::array<std::uint32_t, 6> VertexCode{
    0xe0382000, 0x80000005, 0xbf8c3f70, 0xf80008cf, 0x03020100, 0xbf810000
};
alignas(256) constexpr std::array<std::uint32_t, 1> PixelCode{0xbf810000};
alignas(256) std::array<std::array<float, 4>, 3> Vertices{{
    {-1, -1, 0.5f, 1}, {3, -1, 0.5f, 1}, {-1, 3, 0.5f, 1}
}};

void DrawDepth(const Device& device, const DepthTarget& target, float value) {
    using ShaderRecompiler::ShaderStage;
    for (auto& vertex : Vertices) vertex[2] = value;
    const auto address = reinterpret_cast<std::uintptr_t>(Vertices.data());
    AgcDriver::GuestMemory::MarkWritten(address, sizeof(Vertices));
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const std::array<std::uint32_t, 4> buffer{
        static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (16u << 16u), 3, 0x01016facu
    };
    const std::vector<std::uint32_t> vertexUser(buffer.begin(), buffer.end());
    const std::array<ShaderRecompiler::MemoryRegion, 1> vertexMemory{{{reinterpret_cast<std::uintptr_t>(VertexCode.data()), std::as_bytes(std::span(VertexCode))}}};
    ShaderRecompiler::RecompileRequest vertex{
        {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(VertexCode.data()), VertexCode, 0, {}},
        {64, 0, vertexUser, std::nullopt, std::nullopt, ShaderRecompiler::ShaderVertexStageInfo{}, vertexMemory}, device.Target(), {0, 0, 0, 128}
    };
    vertex.useCache = false;
    const auto vertexResult = ShaderRecompiler::Recompile(vertex);
    const auto vertexPush = static_cast<std::uint32_t>(vertexResult.pushConstants.size());
    const ShaderRecompiler::ShaderPixelStageInfo pixel{};
    const std::array<ShaderRecompiler::MemoryRegion, 1> pixelMemory{{{reinterpret_cast<std::uintptr_t>(PixelCode.data()), std::as_bytes(std::span(PixelCode))}}};
    ShaderRecompiler::RecompileRequest fragment{
        {ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(PixelCode.data()), PixelCode, 0, {}},
        {64, 0, {}, std::nullopt, pixel, std::nullopt, pixelMemory}, device.Target(), {0, 0, vertexPush, 128 - vertexPush}
    };
    fragment.useCache = false;
    const auto pixelResult = ShaderRecompiler::Recompile(fragment);
    const std::array<CompiledShader, 2> shaders{{
        {ShaderStage::Vertex, &vertexResult, 0}, {ShaderStage::Fragment, &pixelResult, vertexPush}
    }};
    State state{};
    state.stages = {ShaderPath::Vertex, 0, 64, 64, std::nullopt, std::nullopt};
    state.depth = target;
    state.depthTest = state.depthWrite = true;
    state.depthCompare = VK_COMPARE_OP_ALWAYS;
    state.renderExtent = target.extent;
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(target.extent.height), static_cast<float>(target.extent.width), -static_cast<float>(target.extent.height), 0, 1};
    state.scissor = {{0, 0}, target.extent};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    const AgcDriver::Pm4::DrawParameters draw{0, 3, 0, 1, 0, false};
    Draw(device.GetContext(), state, draw, shaders);
}

void ExpectValue(SampleProgram& sampler, const Texture& texture, float lod, float expected, const std::string& step) {
    const auto value = sampler.Red(texture, lod);
    Require(std::isfinite(value) && std::abs(value - expected) <= 0.000001f, step + ": sampled " + std::to_string(value) + ", expected " + std::to_string(expected));
}

void RenderAndSample(const Device& device, SampleProgram& sampler, VkExtent2D extent, std::size_t bytes, const std::string& name) {
    GuestBlock memory(bytes);
    std::memset(memory.data, 0x12, bytes);
    AgcDriver::GuestMemory::MarkWritten(memory.Address(), bytes);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto mip0 = Target(memory.Address(), extent, 0);
    const auto mip1 = Target(memory.Address(), extent, 1);
    const auto mip2 = Target(memory.Address(), extent, 2);
    const auto view0 = DepthSurfaceView(device.GetContext(), mip0);
    const auto view1 = DepthSurfaceView(device.GetContext(), mip1);
    Require(view0 != view1 && DepthSurfaceView(device.GetContext(), mip0) == view0, name + ": attachment views did not select and reuse distinct mips");
    auto invalid = mip1;
    ++invalid.extent.width;
    bool refused = false;
    try {
        DepthSurfaceView(device.GetContext(), invalid);
    } catch (const std::runtime_error& error) {
        refused = std::string(error.what()).find("mip") != std::string::npos;
    }
    Require(refused, name + ": a cached depth image bypassed selected-mip extent validation");
    DrawDepth(device, mip0, 0.25f);
    DrawDepth(device, mip1, 0.75f);
    DrawDepth(device, mip2, 0.5f);
    {
        const auto whole = Sample(device.GetContext(), mip0, 0, 2);
        const auto selected = Sample(device.GetContext(), mip0, 1, 1);
        Require(whole != nullptr && selected != nullptr && whole->SampledViewRange(false).levels == 3 && selected->SampledViewRange(false).levels == 1, name + ": sampled views did not expose their requested mip ranges");
        ExpectValue(sampler, *whole, 0, 0.25f, name + " mip0 after mip1/mip2 draws");
        ExpectValue(sampler, *whole, 1, 0.75f, name + " mip1");
        ExpectValue(sampler, *whole, 2, 0.5f, name + " mip2");
        ExpectValue(sampler, *selected, 0, 0.75f, name + " baseLevel1 at lod0");
        DrawDepth(device, mip0, 0.125f);
        ExpectValue(sampler, *whole, 0, 0.125f, name + " rebound mip0");
        ExpectValue(sampler, *whole, 1, 0.75f, name + " mip1 after rebinding mip0");
        ExpectValue(sampler, *whole, 2, 0.5f, name + " mip2 after rebinding mip0");
    }
    ClearDepthSurfaces(device.GetContext().device);
}

bool IncompatibleRefused(const Context& context, const DepthTarget& target) {
    const auto words = Descriptor(target, 0, 2);
    auto resource = DecodeTextureResource(words);
    ++resource.width;
    try {
        static_cast<void>(DepthSurfaceTexture(context, words, resource, Identity));
    } catch (const std::runtime_error& error) {
        return std::string(error.what()).find("is not implemented") != std::string::npos;
    }
    return false;
}

void FootprintTests(const Context& context) {
    constexpr VkExtent2D extent{512, 256};
    constexpr std::size_t bytes = 0xb0000;
    Require(DepthMipChainBytes(extent, 4, 3) == bytes, "the D32 mip chain footprint differs from its padded allocation");
    GuestBlock memory(bytes + 2u * Block);
    std::memset(memory.data, 0x12, bytes + 2u * Block);
    AgcDriver::GuestMemory::MarkWritten(memory.Address(), bytes + 2u * Block);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto target = Target(memory.Address() + Block, extent, 1);
    DepthSurfaceView(context, target);
    RetireDepthSurfaces(context.device, target.address + bytes, Block);
    Require(DepthSurfaceAt(target.address) && IncompatibleRefused(context, target), "a writer past the mip chain retired the active depth image");
    AgcDriver::GuestMemory::MarkWritten(target.address + bytes, 1);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    Require(IncompatibleRefused(context, target), "a driver write past the mip chain bypassed its incompatible depth view");
    memory.data[Block + bytes - 1u] = std::byte{0x13};
    AgcDriver::GuestMemory::MarkWritten(target.address + bytes - 1u, 1);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto words = Descriptor(target, 0, 2);
    auto resource = DecodeTextureResource(words);
    ++resource.width;
    Require(DepthSurfaceTexture(context, words, resource, Identity) == nullptr && !DepthSurfaceAt(target.address), "a new write inside the whole mip chain was not recognized beyond the selected mip footprint");
    DepthSurfaceView(context, target);
    RetireDepthSurfaces(context.device, target.address + bytes - 1u, 1);
    Require(!DepthSurfaceAt(target.address), "a color writer in the end of the mip chain left the cached depth image active");
    ClearDepthSurfaces(context.device);
}

void MultisampleMipRefusal(const Context& context) {
    GuestBlock memory(0xb0000);
    auto target = Target(memory.Address(), {512, 256}, 1);
    target.samples = VK_SAMPLE_COUNT_8_BIT;
    bool refused = false;
    try {
        DepthSurfaceView(context, target);
    } catch (const std::runtime_error& error) {
        refused = std::string(error.what()).find("mip") != std::string::npos;
    }
    Require(refused, "a multisampled mipmapped depth target bypassed the explicit unsupported-path guard");
    ClearDepthSurfaces(context.device);
}

}

int main() {
    try {
        std::unique_ptr<Device> device;
        try {
            device = std::make_unique<Device>();
        } catch (const std::exception& error) {
            if (std::getenv("ANYPS5_REQUIRE_VULKAN") != nullptr) throw;
            std::printf("skipped, no usable Vulkan device: %s\n", error.what());
            return VulkanTestSkipped;
        }
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        SampleProgram sampler(device->GetContext());
        RenderAndSample(*device, sampler, {512, 256}, 0xb0000, "macro mip chain");
        RenderAndSample(*device, sampler, {64, 64}, 0x10000, "shared mip tail");
        FootprintTests(device->GetContext());
        MultisampleMipRefusal(device->GetContext());
        std::puts("depth mip rendering, sampling views, tail preservation and whole-chain tracking tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
