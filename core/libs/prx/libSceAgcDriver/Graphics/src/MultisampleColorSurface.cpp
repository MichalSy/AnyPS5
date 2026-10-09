#include "prx/libSceAgcDriver/Graphics/include/MultisampleColorSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/shaders/MultisampleColorTransfer_spv.h"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <mutex>

namespace AgcDriver::Graphics {
namespace {

thread_local const MultisampleColorSurface* refreshingSurface = nullptr;

std::recursive_mutex& surfaceMutex() {
    static std::recursive_mutex mutex;
    return mutex;
}

struct CachedSurface {
    VkDevice device;
    std::shared_ptr<MultisampleColorSurface> surface;
};

std::vector<CachedSurface>& surfaces() {
    static auto* list = new std::vector<CachedSurface>();
    return *list;
}

bool overlaps(std::uint64_t address, std::size_t bytes, const ColorTarget& target) {
    if (bytes == 0) return false;
    const auto end = bytes > std::numeric_limits<std::uint64_t>::max() - address ? std::numeric_limits<std::uint64_t>::max() : address + bytes;
    return target.address < end && address < target.address + target.bytes;
}

bool sameTarget(const ColorTarget& first, const ColorTarget& second) {
    return first.address == second.address && first.extent.width == second.extent.width && first.extent.height == second.extent.height && first.format == second.format && first.bytes == second.bytes && first.samples == second.samples;
}

}

MultisampleColorSurface::MultisampleColorSurface(const Context& context, const ColorTarget& target)
    : context(context), target(target), geometry(target.extent.width, target.extent.height, target.elementBytes, static_cast<std::uint32_t>(target.samples)) {
    this->context.bufferPool.reset();
    Require(context.shaderStorageImageMultisample, "multisample color transfers require shaderStorageImageMultisample");
    Require(target.samples == VK_SAMPLE_COUNT_8_BIT && target.elementBytes == 4 && target.tileMode == ColorTileMode::RenderTarget, "unsupported multisample color geometry");
    Require(target.depth == 1 && target.depthSlice == 0 && target.mipCount == 1 && target.mip == 0 && !target.mipTail, "multisample color arrays, volumes and mipmaps are unsupported");
    Require(target.dccAddress == 0 && target.cmaskAddress == 0, "multisample color metadata is unsupported");
    Require(target.format == VK_FORMAT_R8G8B8A8_UNORM || target.format == VK_FORMAT_R8G8B8A8_SRGB || target.format == VK_FORMAT_B8G8R8A8_UNORM || target.format == VK_FORMAT_B8G8R8A8_SRGB, "multisample color format is unsupported");
    Require(target.bytes == geometry.Bytes(), "multisample color byte range disagrees with its layout");
    Require(geometry.LinearBytes() <= context.limits.maxStorageBufferRange, "multisample color transfer exceeds storage buffer limits");
    Require(target.extent.width <= context.limits.maxFramebufferWidth && target.extent.height <= context.limits.maxFramebufferHeight, "multisample color exceeds framebuffer limits");
    Require((target.extent.width + 7) / 8 <= context.limits.maxComputeWorkGroupCount[0] && (target.extent.height + 7) / 8 <= context.limits.maxComputeWorkGroupCount[1] && static_cast<std::uint32_t>(target.samples) <= context.limits.maxComputeWorkGroupCount[2], "multisample color transfer exceeds compute limits");
    GuestMemory::CheckRange(reinterpret_cast<const void*>(target.address), target.bytes, geometry.Alignment(), true);
    VkFormatProperties attachmentProperties{};
    context.formatProperties(context.physical, target.format, &attachmentProperties);
    Require((attachmentProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0, "multisample color format cannot be an attachment");
    constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    constexpr VkImageCreateFlags flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    VkImageFormatProperties supported{};
    Check(context.imageFormatProperties(context.physical, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, flags, &supported), "vkGetPhysicalDeviceImageFormatProperties multisample color");
    Require((supported.sampleCounts & target.samples) != 0 && target.extent.width <= supported.maxExtent.width && target.extent.height <= supported.maxExtent.height, "device does not support this multisample color image");
    VkShaderModule module = VK_NULL_HANDLE;
    try {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.flags = flags;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent = {target.extent.width, target.extent.height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = target.samples;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        Check(context.Function<PFN_vkCreateImage>("vkCreateImage")(context.device, &info, nullptr, &image), "vkCreateImage multisample color");
        VkMemoryRequirements requirements{};
        context.Function<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(context.device, image, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")(context.device, &allocation, nullptr, &memory), "vkAllocateMemory multisample color");
        Check(context.Function<PFN_vkBindImageMemory>("vkBindImageMemory")(context.device, image, memory, 0), "vkBindImageMemory multisample color");
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageViewUsageCreateInfo viewUsage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, nullptr, VK_IMAGE_USAGE_STORAGE_BIT};
        viewInfo.pNext = &viewUsage;
        Check(context.Function<PFN_vkCreateImageView>("vkCreateImageView")(context.device, &viewInfo, nullptr, &storageView), "vkCreateImageView multisample storage");
        viewInfo.format = target.format;
        viewUsage.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        Check(context.Function<PFN_vkCreateImageView>("vkCreateImageView")(context.device, &viewInfo, nullptr, &attachmentView), "vkCreateImageView multisample attachment");
        linear = std::make_unique<Buffer>(context, geometry.LinearBytes(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        const std::array<VkDescriptorSetLayoutBinding, 2> bindings{{
            {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}
        }};
        VkDescriptorSetLayoutCreateInfo descriptorInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        descriptorInfo.bindingCount = bindings.size();
        descriptorInfo.pBindings = bindings.data();
        Check(context.Function<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(context.device, &descriptorInfo, nullptr, &descriptorLayout), "vkCreateDescriptorSetLayout multisample color");
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &descriptorLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &push;
        Check(context.Function<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(context.device, &layoutInfo, nullptr, &pipelineLayout), "vkCreatePipelineLayout multisample color");
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = sizeof(MULTISAMPLE_COLOR_TRANSFER_SPV);
        moduleInfo.pCode = MULTISAMPLE_COLOR_TRANSFER_SPV;
        Check(context.Function<PFN_vkCreateShaderModule>("vkCreateShaderModule")(context.device, &moduleInfo, nullptr, &module), "vkCreateShaderModule multisample color");
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = module;
        pipelineInfo.stage.pName = "main";
        pipelineInfo.layout = pipelineLayout;
        Check(context.Function<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(context.device, context.pipelineCache, 1, &pipelineInfo, nullptr, &pipeline), "vkCreateComputePipelines multisample color");
        context.Function<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(context.device, module, nullptr);
        module = VK_NULL_HANDLE;
        const std::array<VkDescriptorPoolSize, 2> sizes{{{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}}};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = sizes.size();
        poolInfo.pPoolSizes = sizes.data();
        Check(context.Function<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(context.device, &poolInfo, nullptr, &descriptorPool), "vkCreateDescriptorPool multisample color");
        VkDescriptorSetAllocateInfo descriptorAllocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        descriptorAllocation.descriptorPool = descriptorPool;
        descriptorAllocation.descriptorSetCount = 1;
        descriptorAllocation.pSetLayouts = &descriptorLayout;
        Check(context.Function<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(context.device, &descriptorAllocation, &descriptorSet), "vkAllocateDescriptorSets multisample color");
        const VkDescriptorImageInfo imageDescriptor{VK_NULL_HANDLE, storageView, VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorBufferInfo bufferDescriptor{linear->Handle(), 0, geometry.LinearBytes()};
        std::array<VkWriteDescriptorSet, 2> writes{};
        for (auto& write : writes) {
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = descriptorSet;
            write.descriptorCount = 1;
        }
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo = &imageDescriptor;
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &bufferDescriptor;
        context.Function<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(context.device, writes.size(), writes.data(), 0, nullptr);
        snapshot.resize(geometry.Bytes());
        GuestMemory::Read(target.address, snapshot, geometry.Alignment());
        upload();
        generation = GuestMemory::CollectWrites(target.address, target.bytes);
    } catch (...) {
        if (module) context.Function<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(context.device, module, nullptr);
        release();
        throw;
    }
}

MultisampleColorSurface::~MultisampleColorSurface() { release(); }

void MultisampleColorSurface::release() noexcept {
    if (descriptorPool) context.Function<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(context.device, descriptorPool, nullptr);
    if (pipeline) context.Function<PFN_vkDestroyPipeline>("vkDestroyPipeline")(context.device, pipeline, nullptr);
    if (pipelineLayout) context.Function<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(context.device, pipelineLayout, nullptr);
    if (descriptorLayout) context.Function<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(context.device, descriptorLayout, nullptr);
    if (attachmentView) context.Function<PFN_vkDestroyImageView>("vkDestroyImageView")(context.device, attachmentView, nullptr);
    if (storageView) context.Function<PFN_vkDestroyImageView>("vkDestroyImageView")(context.device, storageView, nullptr);
    if (image) context.Function<PFN_vkDestroyImage>("vkDestroyImage")(context.device, image, nullptr);
    if (memory) context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, memory, nullptr);
    linear.reset();
    descriptorPool = VK_NULL_HANDLE;
    pipeline = VK_NULL_HANDLE;
    pipelineLayout = VK_NULL_HANDLE;
    descriptorLayout = VK_NULL_HANDLE;
    attachmentView = VK_NULL_HANDLE;
    storageView = VK_NULL_HANDLE;
    image = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
}

void MultisampleColorSurface::transfer(bool store) {
    CommandBatch batch(context);
    const auto commands = batch.Handle();
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = initialized ? VK_ACCESS_MEMORY_WRITE_BIT : 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier.oldLayout = initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    context.Resolved(&DeviceFunctions::cmdPipelineBarrier, "vkCmdPipelineBarrier")(commands, initialized ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    context.Function<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    context.Function<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
    const std::array<std::uint32_t, 4> parameters{target.extent.width, target.extent.height, static_cast<std::uint32_t>(target.samples), store ? 1u : 0u};
    context.Function<PFN_vkCmdPushConstants>("vkCmdPushConstants")(commands, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(parameters), parameters.data());
    context.Function<PFN_vkCmdDispatch>("vkCmdDispatch")(commands, (target.extent.width + 7) / 8, (target.extent.height + 7) / 8, static_cast<std::uint32_t>(target.samples));
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, store ? VK_PIPELINE_STAGE_HOST_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_WRITE_BIT, store ? VK_ACCESS_HOST_READ_BIT : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    batch.SubmitAndWait();
    initialized = true;
}

void MultisampleColorSurface::upload() {
    auto bytes = linear->Bytes();
    const auto samples = static_cast<std::uint32_t>(target.samples);
    for (std::uint32_t y = 0; y < target.extent.height; ++y) {
        for (std::uint32_t x = 0; x < target.extent.width; ++x) {
            for (std::uint32_t sample = 0; sample < samples; ++sample) {
                const auto offset = ((static_cast<std::size_t>(y) * target.extent.width + x) * samples + sample) * 4;
                std::memcpy(bytes.data() + offset, snapshot.data() + geometry.Offset(x, y, sample), 4);
            }
        }
    }
    transfer(false);
}

void MultisampleColorSurface::Refresh() {
    GuestMemory::CollectWrites(target.address, target.bytes);
    if (!needsUpload && ((generation != 0 && GuestMemory::UnchangedSinceCollected(target.address, target.bytes, generation)) || GuestMemory::EqualsCommittedUnsynced(target.address, snapshot))) return;
    Flush();
    Require(GuestMemory::CopyMapped(target.address, snapshot) == GuestMemory::Compare::Equal, "multisample color became unmapped during refresh");
    upload();
    needsUpload = false;
    generation = GuestMemory::CollectWrites(target.address, target.bytes);
}

void MultisampleColorSurface::MarkDirty() {
    std::lock_guard lock(surfaceMutex());
    dirty = true;
    StorageTexture::BumpPendingSerial();
}

bool MultisampleColorSurface::Flush() {
    if (!dirty) return false;
    GuestMemory::CheckRange(reinterpret_cast<const void*>(target.address), target.bytes, geometry.Alignment(), true);
    transfer(true);
    std::vector<std::byte> current(snapshot.size());
    Require(GuestMemory::CopyMapped(target.address, current) == GuestMemory::Compare::Equal, "multisample color became unmapped during writeback");
    auto result = current;
    const auto bytes = linear->Bytes();
    const auto samples = static_cast<std::uint32_t>(target.samples);
    for (std::uint32_t y = 0; y < target.extent.height; ++y) {
        for (std::uint32_t x = 0; x < target.extent.width; ++x) {
            for (std::uint32_t sample = 0; sample < samples; ++sample) {
                const auto tiled = geometry.Offset(x, y, sample);
                const auto offset = ((static_cast<std::size_t>(y) * target.extent.width + x) * samples + sample) * 4;
                for (std::size_t channel = 0; channel < 4; ++channel) {
                    if (current[tiled + channel] == snapshot[tiled + channel]) result[tiled + channel] = bytes[offset + channel];
                    else needsUpload = true;
                }
            }
        }
    }
    GuestMemory::WriteChangedCommitted(target.address, result, current);
    snapshot = std::move(result);
    generation = GuestMemory::CollectWrites(target.address, target.bytes);
    dirty = false;
    StorageTexture::BumpPendingSerial();
    return true;
}

std::shared_ptr<MultisampleColorSurface> CachedMultisampleColorSurface(const Context& context, const ColorTarget& target) {
    GuestMemory::AssertGpuLockHeld("CachedMultisampleColorSurface");
    std::lock_guard lock(surfaceMutex());
    auto& list = surfaces();
    const auto found = std::find_if(list.begin(), list.end(), [&](const auto& entry) { return entry.device == context.device && sameTarget(entry.surface->Target(), target); });
    struct RefreshExemption {
        const MultisampleColorSurface* previous;
        explicit RefreshExemption(const MultisampleColorSurface* surface) : previous(refreshingSurface) { refreshingSurface = surface; }
        ~RefreshExemption() { refreshingSurface = previous; }
    } exemption(found == list.end() ? nullptr : found->surface.get());
    bool published = false;
    const bool flushed = StorageTexture::FlushPending(target.address, target.bytes, nullptr, "multisample color refresh", PublishScope::Whole, &published);
    if (flushed || published) {
        if (auto* recorder = Recorder::Active()) recorder->Sync();
    }
    for (auto& entry : list) {
        if (entry.device == context.device && !sameTarget(entry.surface->Target(), target) && overlaps(target.address, target.bytes, entry.surface->Target())) entry.surface->Flush();
    }
    for (auto& entry : list) {
        if (entry.device != context.device || !sameTarget(entry.surface->Target(), target)) continue;
        entry.surface->Refresh();
        return entry.surface;
    }
    auto result = std::make_shared<MultisampleColorSurface>(context, target);
    list.push_back({context.device, result});
    return result;
}

bool AnyPendingMultisampleColors(std::uint64_t address, std::size_t bytes) {
    std::lock_guard lock(surfaceMutex());
    return std::any_of(surfaces().begin(), surfaces().end(), [&](const auto& entry) { return entry.surface.get() != refreshingSurface && entry.surface->Dirty() && overlaps(address, bytes, entry.surface->Target()); });
}

bool FlushMultisampleColors(std::uint64_t address, std::size_t bytes) {
    if (!AnyPendingMultisampleColors(address, bytes)) return false;
    std::lock_guard gpu(GuestMemory::GpuMutex());
    std::lock_guard lock(surfaceMutex());
    bool flushed = false;
    for (auto& entry : surfaces()) {
        if (entry.surface.get() != refreshingSurface && overlaps(address, bytes, entry.surface->Target())) flushed = entry.surface->Flush() || flushed;
    }
    return flushed;
}

void ClearMultisampleColors(VkDevice device) {
    std::lock_guard gpu(GuestMemory::GpuMutex());
    std::lock_guard lock(surfaceMutex());
    std::erase_if(surfaces(), [&](const auto& entry) { return entry.device == device; });
}

}
