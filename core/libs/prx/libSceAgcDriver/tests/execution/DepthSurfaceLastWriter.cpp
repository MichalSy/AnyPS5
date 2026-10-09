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
            mutation.Add(data, bytes, true, true);
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

std::shared_ptr<Texture> Sample(const Context& context, const GuestTextureResource& resource) {
    const std::array<std::uint32_t, 8> words{0, 0, 0, 0, resource.width, resource.height, 0, 0};
    return DepthSurfaceTexture(context, words, resource, Identity);
}

bool Refused(const Context& context, const GuestTextureResource& resource) {
    try {
        Sample(context, resource);
    } catch (const std::runtime_error& error) {
        return std::string(error.what()).find("is not implemented") != std::string::npos;
    }
    return false;
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
        std::lock_guard gpuLock(AgcDriver::GuestMemory::GpuMutex());
        Run(device->GetContext());
        RemappedColorTest(*device);
        MultisampleRefusalTest(device->GetContext());
        std::puts("depth surface last writer tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
