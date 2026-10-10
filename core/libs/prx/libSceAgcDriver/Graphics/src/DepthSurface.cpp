#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "RdnaDecoder/include/RdnaDecoder/RdnaDescriptorFormat.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include <limits>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {
namespace {

VkExtent2D surfaceExtent(const DepthTarget& target) {
    Require((target.surfaceExtent.width == 0) == (target.surfaceExtent.height == 0), "incomplete depth surface extent");
    return target.surfaceExtent.width != 0 ? target.surfaceExtent : target.extent;
}

VkExtent2D validatedSurfaceExtent(const DepthTarget& target) {
    const auto extent = surfaceExtent(target);
    Require(extent.width != 0 && extent.height != 0 && target.mipCount != 0 && target.mipCount <= 16u && target.mipCount <= std::bit_width(std::max(extent.width, extent.height)), "invalid depth surface mip chain");
    Require(target.mip < target.mipCount && target.extent.width == std::max(extent.width >> target.mip, 1u) && target.extent.height == std::max(extent.height >> target.mip, 1u), "depth view extent does not match its mip");
    return extent;
}

Recorder* recorderFor(const Context& context) {
    auto* recorder = Recorder::Active();
    return recorder != nullptr && recorder->Device() == context.device ? recorder : nullptr;
}

class DepthSurface {
public:
    DepthSurface(const Context& context, const DepthTarget& target) : context(context), target(target) {
        this->context.bufferPool.reset();
        const auto extent = validatedSurfaceExtent(target);
        VkFormatProperties properties{};
        context.formatProperties(context.physical, target.format, &properties);
        Require((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0, "depth/stencil format " + std::to_string(target.format) + " cannot be an attachment on this device");
        Require((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0, "depth/stencil format " + std::to_string(target.format) + " cannot be sampled on this device");
        Require(target.extent.width <= context.limits.maxFramebufferWidth && target.extent.height <= context.limits.maxFramebufferHeight, "depth target exceeds framebuffer limits");
        const VkImageAspectFlags aspects = VK_IMAGE_ASPECT_DEPTH_BIT | (target.stencilAddress != 0 ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u);
        const auto usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            (target.samples == VK_SAMPLE_COUNT_1_BIT ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0u);
        Require(target.samples == VK_SAMPLE_COUNT_1_BIT || target.samples == VK_SAMPLE_COUNT_8_BIT, "depth sample count other than one or eight is unsupported");
        Require(target.samples == VK_SAMPLE_COUNT_1_BIT || target.mipCount == 1u, "multisampled depth mip chains are unsupported");
        Require((context.limits.framebufferDepthSampleCounts & target.samples) != 0, "depth sample count exceeds framebuffer capabilities");
        Require(target.stencilAddress == 0 || (context.limits.framebufferStencilSampleCounts & target.samples) != 0, "stencil sample count exceeds framebuffer capabilities");
        if (target.samples != VK_SAMPLE_COUNT_1_BIT) {
            Require(context.sampleLocations && (context.sampleLocationProperties.sampleLocationSampleCounts & target.samples) != 0, "custom depth sample locations are unsupported on this device");
            const auto& grid = context.sampleLocationGridSizes[std::countr_zero(static_cast<std::uint32_t>(target.samples))];
            Require(target.sampleLocationsGrid.width == 1u && target.sampleLocationsGrid.height == 1u && grid.width >= 1u && grid.height >= 1u && target.sampleLocations.size() == 8u, "custom depth sample location grid is unsupported");
            Require(context.sampleLocationProperties.sampleLocationSubPixelBits >= 4u, "depth sample locations require four subpixel bits");
            const auto scale = std::ldexp(1.0f, static_cast<int>(std::min(context.sampleLocationProperties.sampleLocationSubPixelBits, 23u)));
            for (const auto& location : target.sampleLocations) {
                Require(std::isfinite(location.x) && std::isfinite(location.y), "depth sample locations must be finite");
                Require(std::round(location.x * scale) == location.x * scale && std::round(location.y * scale) == location.y * scale, "depth sample locations exceed device subpixel precision");
                Require(location.x >= context.sampleLocationProperties.sampleLocationCoordinateRange[0] && location.x <= context.sampleLocationProperties.sampleLocationCoordinateRange[1] &&
                    location.y >= context.sampleLocationProperties.sampleLocationCoordinateRange[0] && location.y <= context.sampleLocationProperties.sampleLocationCoordinateRange[1], "depth sample location exceeds device coordinates");
            }
        } else Require(target.sampleLocations.empty(), "custom single-sample depth locations are unsupported");
        VkImageFormatProperties supported{};
        Check(context.imageFormatProperties(context.physical, target.format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage,
            target.sampleLocations.empty() ? 0u : VK_IMAGE_CREATE_SAMPLE_LOCATIONS_COMPATIBLE_DEPTH_BIT_EXT, &supported), "vkGetPhysicalDeviceImageFormatProperties depth");
        Require((supported.sampleCounts & target.samples) != 0, "depth/stencil image usage does not support the requested sample count");
        Require(extent.width <= supported.maxExtent.width && extent.height <= supported.maxExtent.height, "depth target exceeds format extent limits");
        Require(target.mipCount <= supported.maxMipLevels, "depth target exceeds format mip limits");
        try {
            VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            info.flags = target.sampleLocations.empty() ? 0u : VK_IMAGE_CREATE_SAMPLE_LOCATIONS_COMPATIBLE_DEPTH_BIT_EXT;
            info.imageType = VK_IMAGE_TYPE_2D;
            info.format = target.format;
            info.extent = {extent.width, extent.height, 1};
            info.mipLevels = target.mipCount;
            info.arrayLayers = 1;
            info.samples = target.samples;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = usage;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            Check(context.Function<PFN_vkCreateImage>("vkCreateImage")(context.device, &info, nullptr, &image), "vkCreateImage depth");
            VkMemoryRequirements requirements{};
            context.Function<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(context.device, image, &requirements);
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            Check(context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")(context.device, &allocation, nullptr, &memory), "vkAllocateMemory depth target");
            Check(context.Function<PFN_vkBindImageMemory>("vkBindImageMemory")(context.device, image, memory, 0), "vkBindImageMemory depth");
            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = image;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = target.format;
            views.resize(target.mipCount, VK_NULL_HANDLE);
            for (std::uint32_t mip = 0; mip < target.mipCount; ++mip) {
                viewInfo.subresourceRange = {aspects, mip, 1, 0, 1};
                Check(context.Function<PFN_vkCreateImageView>("vkCreateImageView")(context.device, &viewInfo, nullptr, &views[mip]), "vkCreateImageView depth");
            }
            auto* recorder = recorderFor(context);
            std::unique_ptr<CommandBatch> batch;
            if (recorder == nullptr) batch = std::make_unique<CommandBatch>(context);
            const auto commands = recorder != nullptr ? recorder->Commands() : batch->Handle();
            VkSampleLocationsInfoEXT locations{VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT};
            locations.sampleLocationsPerPixel = target.samples;
            locations.sampleLocationGridSize = target.sampleLocationsGrid;
            locations.sampleLocationsCount = static_cast<std::uint32_t>(target.sampleLocations.size());
            locations.pSampleLocations = target.sampleLocations.data();
            VkImageMemoryBarrier toGeneral{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            toGeneral.pNext = target.sampleLocations.empty() ? nullptr : &locations;
            toGeneral.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toGeneral.image = image;
            toGeneral.subresourceRange = {aspects, 0, target.mipCount, 0, 1};
            const auto barrier = context.Resolved(&DeviceFunctions::cmdPipelineBarrier, "vkCmdPipelineBarrier");
            barrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toGeneral);
            const VkClearDepthStencilValue clear{target.clearDepth, target.clearStencil};
            context.Function<PFN_vkCmdClearDepthStencilImage>("vkCmdClearDepthStencilImage")(commands, image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &toGeneral.subresourceRange);
            RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
            if (batch) batch->SubmitAndWait();
            else Recorder::CountBarriers(Recorder::CommandClass::Draw, 2);
        } catch (...) {
            release();
            throw;
        }
    }

    ~DepthSurface() { release(); }
    DepthSurface(const DepthSurface&) = delete;
    DepthSurface& operator=(const DepthSurface&) = delete;

    void Bind(const DepthTarget& bound) {
        const auto registry = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
        if (retired || htileAddress != bound.htileAddress || htileStencil != bound.htileStencil || registry != htileRegistry) pendingClear = false;
        htileAddress = bound.htileAddress;
        htileStencil = bound.htileStencil;
        htileRegistry = registry;
        clearDepth = bound.clearDepth;
        clearStencil = bound.clearStencil;
    }

    bool AcceptsHtileFill() const {
        return !retired && htileAddress != 0 && !htileStencil && target.address != 0 && target.samples == VK_SAMPLE_COUNT_1_BIT &&
            target.mipCount == 1u && target.mip == 0u && htileRegistry == GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    }

    void NoteHtileFill(std::uint64_t address, std::size_t bytes, std::uint32_t pattern) {
        const auto footprint = HtileSliceBytes(surfaceExtent(target));
        if (htileAddress == 0 || footprint == 0 || bytes == 0 ||
            (address >= htileAddress ? address - htileAddress >= footprint : htileAddress - address >= bytes)) return;
        pendingClear = false;
        if (!AcceptsHtileFill() || !HtileFillCovers(htileAddress, surfaceExtent(target), address, bytes) || (pattern & 0xfu) != 0) return;
        const auto generation = GuestMemory::CollectWritesUncached(htileAddress, static_cast<std::size_t>(footprint));
        if (generation == 0 || !GuestMemory::Watched(htileAddress, static_cast<std::size_t>(footprint))) return;
        pendingGeneration = generation;
        pendingClear = true;
    }

    void ApplyFastClear() {
        if (!pendingClear) return;
        pendingClear = false;
        const auto footprint = static_cast<std::size_t>(HtileSliceBytes(surfaceExtent(target)));
        if (!AcceptsHtileFill() || GuestMemory::CollectWritesUncached(htileAddress, footprint) == 0 ||
            GuestMemory::StoredOver(htileAddress, footprint, pendingGeneration)) return;
        writer.reset();
        auto* recorder = recorderFor(context);
        std::unique_ptr<CommandBatch> batch;
        if (recorder == nullptr) batch = std::make_unique<CommandBatch>(context);
        const auto commands = recorder != nullptr ? recorder->Commands() : batch->Handle();
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        constexpr VkAccessFlags access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, access, VK_ACCESS_TRANSFER_WRITE_BIT);
        const VkClearDepthStencilValue clear{clearDepth, clearStencil};
        context.Function<PFN_vkCmdClearDepthStencilImage>("vkCmdClearDepthStencilImage")(commands, image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, access);
        if (batch) batch->SubmitAndWait();
        else Recorder::CountBarriers(Recorder::CommandClass::Draw, 2);
    }

    void Transfer(StorageTexture& storage, bool intoStorage, std::uint32_t layer) {
        const auto& descriptor = storage.Descriptor();
        const auto geometry = DescribeSurface(descriptor);
        const bool d16 = target.format == VK_FORMAT_D16_UNORM || target.format == VK_FORMAT_D16_UNORM_S8_UINT;
        const bool d32 = target.format == VK_FORMAT_D32_SFLOAT || target.format == VK_FORMAT_D32_SFLOAT_S8_UINT;
        const auto extent = surfaceExtent(target);
        const auto stride = DepthSliceBytes(extent, d16 ? 2u : 4u);
        const auto offset = geometry.GuestLayerOffset(layer);
        const bool sized = !IsConvertedTextureFormat(descriptor.format) && BlockWidth(descriptor.format) == 1u &&
            BlockHeight(descriptor.format) == 1u && BytesPerElement(descriptor.format) == (d16 ? 2u : 4u) &&
            storage.StorageFormat() == ResolveTextureFormat(descriptor.format);
        if (target.samples != VK_SAMPLE_COUNT_1_BIT || target.mipCount != 1u || target.mip != 0u || target.address == 0 ||
            (!d16 && !d32) || !sized || offset > std::numeric_limits<std::uint64_t>::max() - descriptor.baseAddress ||
            descriptor.baseAddress + offset != target.address || (storage.ImageLayers() > 1u && geometry.layerBytes != stride) || descriptor.width != extent.width ||
            descriptor.height != extent.height || descriptor.mipCount != 1u || descriptor.baseLevel != 0u ||
            descriptor.lastLevel != 0u || descriptor.baseArray >= storage.ImageLayers() || layer >= storage.ImageLayers() ||
            (descriptor.dimension != TextureDimension::k2D && descriptor.dimension != TextureDimension::k2DArray) ||
            (descriptor.dimension == TextureDimension::k2D && descriptor.depthOrLastArray != 0u) || storage.ImageDepth() != 1u) {
            char text[320];
            std::snprintf(text, sizeof(text), "AGC graphics: storage image access to depth surface 0x%llx (%ux%u, vk format %d, %u samples, %u mips) as a %ux%u image of vk format %d, dimension %d, %u mips is not implemented",
                static_cast<unsigned long long>(target.address), extent.width, extent.height, static_cast<int>(target.format),
                static_cast<unsigned>(target.samples), target.mipCount, descriptor.width, descriptor.height,
                static_cast<int>(storage.StorageFormat()), static_cast<int>(descriptor.dimension), descriptor.mipCount);
            throw std::runtime_error(text);
        }
        if (transferBuffer == nullptr) transferBuffer = std::make_unique<DeviceBuffer>(context, static_cast<std::size_t>(extent.width) * extent.height * (d16 ? 2u : 4u), VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        auto* recorder = recorderFor(context);
        std::unique_ptr<CommandBatch> batch;
        if (recorder == nullptr) batch = std::make_unique<CommandBatch>(context);
        const auto commands = recorder != nullptr ? recorder->Commands() : batch->Handle();
        if (recorder != nullptr) {
            if (auto self = storage.weak_from_this().lock()) recorder->Keep(std::move(self));
        }
        constexpr VkAccessFlags access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
            VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, access, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy depthRegion{};
        depthRegion.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        depthRegion.imageExtent = {extent.width, extent.height, 1};
        auto storageRegion = depthRegion;
        storageRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        storageRegion.imageSubresource.baseArrayLayer = layer;
        context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(commands, intoStorage ? image : storage.Image(), VK_IMAGE_LAYOUT_GENERAL, transferBuffer->Handle(), 1, intoStorage ? &depthRegion : &storageRegion);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        context.Function<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(commands, transferBuffer->Handle(), intoStorage ? storage.Image() : image, VK_IMAGE_LAYOUT_GENERAL, 1, intoStorage ? &storageRegion : &depthRegion);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, access);
        if (batch) batch->SubmitAndWait();
        else Recorder::CountBarriers(Recorder::CommandClass::Draw, 3);
    }

    std::shared_ptr<StorageTexture> TakeWrites() {
        if (writer == nullptr) return nullptr;
        Transfer(*writer, false, writerLayer);
        return std::exchange(writer, nullptr);
    }

    bool StorageOwnsSample(const GuestTextureResource& resource) const {
        if (writer == nullptr || !Texture::CanCopyFrom(*writer, resource)) return false;
        const auto format = ResolveTextureFormat(resource.format);
        return writer->StorageFormat() == format && format != VK_FORMAT_R32_SFLOAT && format != VK_FORMAT_R16_UNORM;
    }

    std::shared_ptr<StorageTexture> SeedStorage(const std::shared_ptr<StorageTexture>& storage, std::uint32_t layer) {
        ApplyFastClear();
        if (writer == storage && writerLayer == layer) return nullptr;
        auto consumed = TakeWrites();
        Transfer(*storage, true, layer);
        writer = storage;
        writerLayer = layer;
        return consumed;
    }

    void Retire() {
        retired = true;
        writer.reset();
    }

    VkImageView View(std::uint32_t mip) const {
        Require(mip < views.size(), "depth attachment mip is outside its surface");
        return views[mip];
    }

    bool SampledAccepts(std::span<const std::uint32_t> words, const GuestTextureResource& resource) const {
        const bool stencil = target.stencilAddress != 0 && resource.baseAddress == target.stencilAddress;
        const bool d16 = target.format == VK_FORMAT_D16_UNORM || target.format == VK_FORMAT_D16_UNORM_S8_UINT;
        const auto expected = stencil ? VK_FORMAT_R8_UINT : d16 ? VK_FORMAT_R16_UNORM : VK_FORMAT_R32_SFLOAT;
        const auto format = ResolveTextureFormat(resource.format);
        const bool depthBits = !stencil && words.size() >= 4 && ShaderRecompiler::DepthBitsTextureWidth(words[1], words[3]) == (d16 ? 16u : 32u);
        const auto extent = surfaceExtent(target);
        return (format == expected || depthBits) && resource.dimension == TextureDimension::k2D && resource.width == extent.width && resource.height == extent.height &&
            resource.baseLevel <= resource.lastLevel && resource.lastLevel < target.mipCount && resource.baseArray == 0 && (target.mipCount == 1u || resource.tileMode == TextureTileMode::kZ64KBX);
    }

    std::shared_ptr<Texture> Sampled(std::span<const std::uint32_t> words, const GuestTextureResource& resource, VkComponentMapping components) {
        std::array<std::uint32_t, 12> key{};
        std::copy_n(words.begin(), std::min<std::size_t>(words.size(), 8), key.begin());
        key[8] = components.r;
        key[9] = components.g;
        key[10] = components.b;
        key[11] = components.a;
        if (target.samples != VK_SAMPLE_COUNT_1_BIT) {
            char text[768];
            std::snprintf(text, sizeof(text), "AGC graphics: sampling a multisampled depth/stencil surface requires multisampled texture materialization: cached depth=0x%llx stencil=0x%llx %ux%u samples=%u vk=%d; texture=0x%llx %ux%u guest format=%u tile=%u dimension=%u levels=%u-%u slice=%u (T# %08x %08x %08x %08x %08x %08x %08x %08x)",
                static_cast<unsigned long long>(target.address), static_cast<unsigned long long>(target.stencilAddress), target.extent.width, target.extent.height, static_cast<unsigned>(target.samples), static_cast<int>(target.format),
                static_cast<unsigned long long>(resource.baseAddress), resource.width, resource.height, resource.format, static_cast<unsigned>(resource.tileMode), static_cast<unsigned>(resource.dimension), resource.baseLevel, resource.lastLevel, resource.baseArray,
                key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7]);
            throw std::runtime_error(text);
        }
        if (const auto found = textures.find(key); found != textures.end()) return found->second;
        const bool stencil = target.stencilAddress != 0 && resource.baseAddress == target.stencilAddress;
        if (!SampledAccepts(words, resource)) {
            char text[448];
            std::snprintf(text, sizeof(text), "AGC graphics: sampling the %s plane of depth surface 0x%llx (%ux%u, vk format %d) as a %ux%u texture of guest format %u (vk %d), tile mode %u, dimension %d, levels %u-%u, slice %u is not implemented (T# %08x %08x %08x %08x %08x %08x %08x %08x)",
                          stencil ? "stencil" : "depth", static_cast<unsigned long long>(target.address), target.extent.width, target.extent.height, static_cast<int>(target.format), resource.width, resource.height, resource.format, static_cast<int>(ResolveTextureFormat(resource.format)),
                          static_cast<unsigned>(resource.tileMode), static_cast<int>(resource.dimension), resource.baseLevel, resource.lastLevel, resource.baseArray, key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7]);
            throw std::runtime_error(text);
        }
        auto texture = std::make_shared<Texture>(context, image, target.format, stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, components, resource.baseLevel, resource.lastLevel - resource.baseLevel + 1u);
        textures.emplace(key, texture);
        return texture;
    }

    bool Holds(std::span<const std::uint32_t> words, const GuestTextureResource& resource, VkComponentMapping components, const Texture* texture) const {
        if (target.samples != VK_SAMPLE_COUNT_1_BIT || StorageOwnsSample(resource) || !SampledAccepts(words, resource)) return false;
        std::array<std::uint32_t, 12> key{};
        std::copy(words.begin(), words.end(), key.begin());
        key[8] = components.r;
        key[9] = components.g;
        key[10] = components.b;
        key[11] = components.a;
        const auto found = textures.find(key);
        return found != textures.end() && found->second.get() == texture;
    }

    bool Overlaps(std::uint64_t address, std::uint64_t bytes) const {
        const auto overlaps = [&](std::uint64_t plane, std::uint64_t planeBytes) {
            return plane != 0 && bytes != 0 && (address >= plane ? address - plane < planeBytes : plane - address < bytes);
        };
        return overlaps(target.address, target.samples == VK_SAMPLE_COUNT_1_BIT ? depthBytes() : 1u) ||
            overlaps(target.stencilAddress, target.samples == VK_SAMPLE_COUNT_1_BIT ? stencilBytes() : 1u);
    }

    bool Overlaps(const DepthSurface& other) const {
        return (other.target.address != 0 && Overlaps(other.target.address, other.target.samples == VK_SAMPLE_COUNT_1_BIT ? other.depthBytes() : 1u)) ||
            (other.target.stencilAddress != 0 && Overlaps(other.target.stencilAddress, other.target.samples == VK_SAMPLE_COUNT_1_BIT ? other.stencilBytes() : 1u));
    }

    void NoteWritten() {
        if (target.samples != VK_SAMPLE_COUNT_1_BIT) return;
        depthWritten = target.address != 0 ? GuestMemory::CollectWrites(target.address, depthBytes()) : 0;
        stencilWritten = target.stencilAddress != 0 ? GuestMemory::CollectWrites(target.stencilAddress, stencilBytes()) : 0;
    }

    bool OverwrittenInMemory() const {
        if (target.samples != VK_SAMPLE_COUNT_1_BIT) return false;
        if (target.address != 0) {
            GuestMemory::CollectWrites(target.address, depthBytes());
            if (GuestMemory::WrittenSince(target.address, depthBytes(), depthWritten)) return true;
        }
        if (target.stencilAddress == 0) return false;
        GuestMemory::CollectWrites(target.stencilAddress, stencilBytes());
        return GuestMemory::WrittenSince(target.stencilAddress, stencilBytes(), stencilWritten);
    }

    const Context context;
    const DepthTarget target;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    bool retired = false;
    bool mappingInvalidated = false;
    bool pendingClear = false;
    std::uint64_t htileAddress = 0;
    bool htileStencil = false;
    std::uint64_t htileRegistry = 0;
    std::uint64_t pendingGeneration = 0;
    float clearDepth = 0.0f;
    std::uint8_t clearStencil = 0;

private:
    std::vector<VkImageView> views;
    std::map<std::array<std::uint32_t, 12>, std::shared_ptr<Texture>> textures;
    std::unique_ptr<DeviceBuffer> transferBuffer;
    std::shared_ptr<StorageTexture> writer;
    std::uint32_t writerLayer = 0;
    std::uint64_t depthWritten = 0;
    std::uint64_t stencilWritten = 0;

    std::uint64_t depthBytes() const {
        const bool d16 = target.format == VK_FORMAT_D16_UNORM || target.format == VK_FORMAT_D16_UNORM_S8_UINT;
        return DepthMipChainBytes(surfaceExtent(target), d16 ? 2u : 4u, target.mipCount);
    }

    std::uint64_t stencilBytes() const {
        return DepthMipChainBytes(surfaceExtent(target), 1u, target.mipCount);
    }

    void release() noexcept {
        textures.clear();
        writer.reset();
        transferBuffer.reset();
        for (const auto view : views) if (view) context.Function<PFN_vkDestroyImageView>("vkDestroyImageView")(context.device, view, nullptr);
        if (image) context.Function<PFN_vkDestroyImage>("vkDestroyImage")(context.device, image, nullptr);
        if (memory) context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, memory, nullptr);
        views.clear();
        image = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
    }
};

bool sameSurface(const DepthTarget& a, const DepthTarget& b) {
    const auto aExtent = surfaceExtent(a);
    const auto bExtent = surfaceExtent(b);
    return a.address == b.address && a.stencilAddress == b.stencilAddress && aExtent.width == bExtent.width && aExtent.height == bExtent.height && a.mipCount == b.mipCount && a.format == b.format && a.samples == b.samples;
}

std::mutex& surfacesMutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<std::unique_ptr<DepthSurface>>& surfaces() {
    static auto* list = new std::vector<std::unique_ptr<DepthSurface>>();
    return *list;
}

void finishStorageWrites(std::unique_lock<std::mutex>& lock, DepthSurface* surface, const std::shared_ptr<StorageTexture>& consumed) {
    if (consumed == nullptr || consumed->Cached()) return;
    // Write-back acquires the allocation registry, whose mapping callback acquires this mutex.
    lock.unlock();
    consumed->WriteBack();
    lock.lock();
    const auto found = std::find_if(surfaces().begin(), surfaces().end(), [&](const auto& current) { return current.get() == surface; });
    if (found != surfaces().end() && !(*found)->retired && !(*found)->mappingInvalidated) (*found)->NoteWritten();
}

void invalidateDepthMappings(std::uintptr_t address, std::size_t bytes) {
    std::lock_guard lock(surfacesMutex());
    for (const auto& surface : surfaces()) {
        if (!surface->mappingInvalidated && surface->Overlaps(address, bytes)) {
            surface->Retire();
            surface->mappingInvalidated = true;
            surface->pendingClear = false;
        }
    }
}

}

std::uint64_t DepthSliceBytes(VkExtent2D extent, std::uint32_t bytesPerTexel) {
    const std::uint32_t blockWidth = bytesPerTexel == 4 ? 128u : 256u;
    const std::uint32_t blockHeight = bytesPerTexel == 1 ? 256u : 128u;
    const auto width = static_cast<std::uint64_t>((extent.width + blockWidth - 1) / blockWidth * blockWidth);
    const auto height = static_cast<std::uint64_t>((extent.height + blockHeight - 1) / blockHeight * blockHeight);
    return width * height * bytesPerTexel;
}

std::uint64_t DepthMipChainBytes(VkExtent2D extent, std::uint32_t bytesPerTexel, std::uint32_t mipCount) {
    Require(bytesPerTexel == 1u || bytesPerTexel == 2u || bytesPerTexel == 4u, "unsupported depth plane element size");
    Require(extent.width != 0 && extent.height != 0 && mipCount != 0 && mipCount <= 16u && mipCount <= std::bit_width(std::max(extent.width, extent.height)), "invalid depth plane mip chain");
    if (mipCount == 1u) return DepthSliceBytes(extent, bytesPerTexel);
    return ComputeSurfaceSize(ComputeElementMipLayout(TextureTileMode::kZ64KBX, bytesPerTexel, extent.width, extent.height, mipCount), 1u);
}

VkImageView DepthSurfaceView(const Context& context, const DepthTarget& target) {
    static_cast<void>(validatedSurfaceExtent(target));
    GuestAllocations::GuestAllocationsSetMappingInvalidator_nid_postfix(&invalidateDepthMappings);
    std::lock_guard gpu(GuestMemory::GpuMutex());
    std::unique_lock lock(surfacesMutex());
    DepthSurface* bound = nullptr;
    for (const auto& surface : surfaces()) {
        if (!surface->mappingInvalidated && surface->context.device == context.device && sameSurface(surface->target, target)) {
            Require(surface->target.sampleLocationsGrid.width == target.sampleLocationsGrid.width && surface->target.sampleLocationsGrid.height == target.sampleLocationsGrid.height &&
                surface->target.sampleLocations.size() == target.sampleLocations.size() && std::equal(surface->target.sampleLocations.begin(), surface->target.sampleLocations.end(), target.sampleLocations.begin(),
                    [](const auto& a, const auto& b) { return a.x == b.x && a.y == b.y; }), "changing sample locations for an existing depth/stencil surface is unsupported");
            bound = surface.get();
            break;
        }
    }
    if (bound == nullptr) {
        surfaces().push_back(std::make_unique<DepthSurface>(context, target));
        bound = surfaces().back().get();
    }
    bound->Bind(target);
    bound->retired = false;
    for (const auto& surface : surfaces()) {
        if (surface.get() != bound && surface->context.device == context.device && surface->Overlaps(*bound)) surface->Retire();
    }
    bound->ApplyFastClear();
    auto consumed = bound->TakeWrites();
    bound->NoteWritten();
    const auto view = bound->View(target.mip);
    finishStorageWrites(lock, bound, consumed);
    return view;
}

std::uint64_t HtileSliceBytes(VkExtent2D extent) {
    if (extent.width == 0 || extent.height == 0) return 0;
    const auto blocksWide = (static_cast<std::uint64_t>(extent.width) + 1023u) / 1024u;
    const auto blocksHigh = (static_cast<std::uint64_t>(extent.height) + 511u) / 512u;
    return blocksWide * blocksHigh * 32768u;
}

bool HtileFillCovers(std::uint64_t htile, VkExtent2D extent, std::uint64_t address, std::size_t bytes) {
    const auto footprint = HtileSliceBytes(extent);
    return htile != 0 && (htile & 0x7fffu) == 0 && footprint != 0 && address <= htile &&
        bytes <= std::numeric_limits<std::uint64_t>::max() - address && htile - address <= bytes && footprint <= bytes - (htile - address);
}

void NoteDepthMetadataFill(VkDevice device, std::uint64_t address, std::size_t bytes, std::uint32_t pattern) {
    std::lock_guard gpu(GuestMemory::GpuMutex());
    std::lock_guard lock(surfacesMutex());
    std::size_t candidates = 0;
    for (const auto& surface : surfaces()) {
        if (surface->context.device == device && surface->AcceptsHtileFill() && HtileFillCovers(surface->htileAddress, surfaceExtent(surface->target), address, bytes)) ++candidates;
    }
    for (const auto& surface : surfaces()) {
        if (surface->context.device != device) continue;
        surface->NoteHtileFill(address, bytes, candidates == 1u ? pattern : 0xffffffffu);
    }
}

std::uint64_t HtileDepthClearAddress(std::span<const std::uint32_t> code, std::span<const std::uint32_t> userData, const std::array<std::uint32_t, 3>& numThreads) {
    static constexpr std::array<std::uint32_t, 17> htileMaskKernel{0xd7460000u, 0x04010c06u, 0x34000084u, 0xdc388000u, 0x04020000u, 0xbf8c3f70u, 0xd7710008u, 0x00120805u, 0xd7710009u, 0x00120a05u, 0xd771000au, 0x00120c05u, 0xd771000bu, 0x00120e05u, 0xdc788000u, 0x00020800u, 0xbf810000u};
    if (userData.size() < 6 || numThreads[0] != 64 || numThreads[1] != 1 || numThreads[2] != 1) return 0;
    if (code.size() < htileMaskKernel.size() || !std::equal(htileMaskKernel.begin(), htileMaskKernel.end(), code.begin())) return 0;
    if ((userData[5] & 0xfu) != 0 || (userData[4] & 0xfu) != 0) return 0;
    return userData[2] | (static_cast<std::uint64_t>(userData[3]) << 32u);
}

void NoteHtileDepthClear(std::uint64_t htileAddress) {
    std::lock_guard lock(surfacesMutex());
    for (const auto& surface : surfaces()) {
        if (surface->htileAddress == htileAddress) surface->pendingClear = false;
    }
}

void ClearDepthSurfaces(VkDevice device) {
    std::lock_guard lock(surfacesMutex());
    std::erase_if(surfaces(), [&](const auto& surface) { return surface->context.device == device; });
}

void RetireDepthSurfaces(VkDevice device, std::uint64_t address, std::uint64_t bytes) {
    std::lock_guard lock(surfacesMutex());
    for (const auto& surface : surfaces()) {
        if (surface->context.device == device && surface->Overlaps(address, bytes)) surface->Retire();
    }
}

std::shared_ptr<Texture> DepthSurfaceTexture(const Context& context, std::span<const std::uint32_t> words, const GuestTextureResource& resource, VkComponentMapping components) {
    const auto matches = [&](const auto& surface) {
        return !surface->retired && surface->context.device == context.device && (surface->target.address == resource.baseAddress || (surface->target.stencilAddress != 0 && surface->target.stencilAddress == resource.baseAddress));
    };
    {
        std::lock_guard lock(surfacesMutex());
        if (std::none_of(surfaces().begin(), surfaces().end(), matches)) return nullptr;
    }
    std::lock_guard gpu(GuestMemory::GpuMutex());
    std::unique_lock lock(surfacesMutex());
    const auto& list = surfaces();
    const auto found = std::find_if(list.rbegin(), list.rend(), matches);
    if (found == list.rend()) return nullptr;
    if ((*found)->StorageOwnsSample(resource)) return nullptr;
    if ((*found)->target.samples == VK_SAMPLE_COUNT_1_BIT && !(*found)->SampledAccepts(words, resource)) {
        if ((*found)->OverwrittenInMemory()) (*found)->Retire();
        return nullptr;
    }
    auto texture = (*found)->Sampled(words, resource, components);
    (*found)->ApplyFastClear();
    auto consumed = (*found)->TakeWrites();
    finishStorageWrites(lock, found->get(), consumed);
    return texture;
}

bool DepthSurfaceHolds(const Context& context, std::span<const std::uint32_t> words, VkComponentMapping components, const Texture* texture) {
    if (texture == nullptr || words.size() != 8u || (words[3] >> 28u) != 9u) return false;
    const auto resource = DecodeTextureResource(words);
    std::lock_guard gpu(GuestMemory::GpuMutex());
    std::unique_lock lock(surfacesMutex());
    const auto& list = surfaces();
    const auto found = std::find_if(list.rbegin(), list.rend(), [&](const auto& surface) {
        return !surface->retired && surface->context.device == context.device && (surface->target.address == resource.baseAddress ||
            (surface->target.stencilAddress != 0 && surface->target.stencilAddress == resource.baseAddress));
    });
    if (found == list.rend() || !(*found)->Holds(words, resource, components, texture)) return false;
    (*found)->ApplyFastClear();
    auto consumed = (*found)->TakeWrites();
    finishStorageWrites(lock, found->get(), consumed);
    return true;
}

void SeedStorageFromDepth(const Context& context, const std::shared_ptr<StorageTexture>& storage) {
    Require(storage != nullptr, "a depth surface cannot seed a null storage image");
    const auto& descriptor = storage->Descriptor();
    const auto geometry = DescribeSurface(descriptor);
    Require(geometry.layerBytes != 0 && geometry.guestBytes <= std::numeric_limits<std::uint64_t>::max() - descriptor.baseAddress, "invalid storage image depth range");
    std::lock_guard gpu(GuestMemory::GpuMutex());
    std::unique_lock lock(surfacesMutex());
    const auto containsPlane = [&](std::uint64_t address) {
        return address != 0 && (address == descriptor.baseAddress || (descriptor.dimension != TextureDimension::k2D &&
            address >= descriptor.baseAddress && address - descriptor.baseAddress < geometry.guestBytes));
    };
    for (const auto& surface : surfaces()) {
        if (surface->retired || surface->mappingInvalidated || surface->context.device != context.device ||
            (!containsPlane(surface->target.address) && !containsPlane(surface->target.stencilAddress))) continue;
        Require((descriptor.dimension == TextureDimension::k2D || descriptor.dimension == TextureDimension::k2DArray) && storage->ImageDepth() == 1u,
            "storage access to a depth surface requires a 2D image or array");
        Require(surface->target.address >= descriptor.baseAddress && surface->target.address - descriptor.baseAddress < geometry.guestBytes &&
            (surface->target.address - descriptor.baseAddress) % geometry.layerBytes == 0u,
            "storage access to a depth surface requires an aligned depth array layer: depth=" + std::to_string(surface->target.address) +
            " extent=" + std::to_string(surface->target.extent.width) + "x" + std::to_string(surface->target.extent.height) +
            " format=" + std::to_string(surface->target.format) + " storage=" + std::to_string(descriptor.baseAddress) +
            " extent=" + std::to_string(descriptor.width) + "x" + std::to_string(descriptor.height) +
            " format=" + std::to_string(storage->StorageFormat()) + " bytes=" + std::to_string(geometry.guestBytes) +
            " layerBytes=" + std::to_string(geometry.layerBytes));
    }
    for (std::uint32_t layer = 0; layer < storage->ImageLayers(); ++layer) {
        const auto address = descriptor.baseAddress + geometry.GuestLayerOffset(layer);
        const auto& list = surfaces();
        const auto found = std::find_if(list.rbegin(), list.rend(), [&](const auto& surface) {
            return !surface->retired && !surface->mappingInvalidated && surface->context.device == context.device && surface->target.address == address;
        });
        if (found == list.rend()) continue;
        auto* surface = found->get();
        auto consumed = surface->SeedStorage(storage, layer);
        finishStorageWrites(lock, surface, consumed);
    }
}

bool DepthSurfaceAt(std::uint64_t address, std::uint64_t bytes) {
    std::lock_guard lock(surfacesMutex());
    return std::any_of(surfaces().begin(), surfaces().end(), [&](const auto& surface) {
        return !surface->retired && !surface->mappingInvalidated && (bytes != 0 ? surface->Overlaps(address, bytes) :
            surface->target.address == address || (surface->target.stencilAddress != 0 && surface->target.stencilAddress == address));
    });
}

bool DepthSurfaceAt(const GuestTextureResource& resource, bool requireLayerStart) {
    if (DepthSurfaceAt(resource.baseAddress)) return true;
    if (resource.dimension == TextureDimension::k2D) return false;
    if (requireLayerStart && resource.dimension != TextureDimension::k2DArray) {
        Require(!DepthSurfaceAt(resource, false), "storage access to a depth surface requires a 2D image or array");
        return false;
    }
    const auto geometry = DescribeSurface(resource);
    Require(geometry.layerBytes != 0, "invalid storage image depth layer size");
    const auto matches = [&](std::uint64_t address) {
        if (address == 0 || address < resource.baseAddress || address - resource.baseAddress >= geometry.guestBytes) return false;
        const auto offset = address - resource.baseAddress;
        return !requireLayerStart || (offset % geometry.layerBytes == 0u && offset / geometry.layerBytes < geometry.imageLayers);
    };
    std::lock_guard lock(surfacesMutex());
    return std::any_of(surfaces().begin(), surfaces().end(), [&](const auto& surface) {
        return !surface->retired && !surface->mappingInvalidated && (matches(surface->target.address) || matches(surface->target.stencilAddress));
    });
}

}
