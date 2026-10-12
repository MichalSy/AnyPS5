#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureSwizzleEquations.hpp"
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

class DepthSurface : public std::enable_shared_from_this<DepthSurface> {
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
        Require(target.samples == VK_SAMPLE_COUNT_1_BIT || target.samples == VK_SAMPLE_COUNT_2_BIT || target.samples == VK_SAMPLE_COUNT_4_BIT || target.samples == VK_SAMPLE_COUNT_8_BIT, "depth sample count greater than eight is unsupported");
        Require(target.samples == VK_SAMPLE_COUNT_1_BIT || target.mipCount == 1u, "multisampled depth mip chains are unsupported");
        Require((context.limits.framebufferDepthSampleCounts & target.samples) != 0, "depth sample count exceeds framebuffer capabilities");
        Require(target.stencilAddress == 0 || (context.limits.framebufferStencilSampleCounts & target.samples) != 0, "stencil sample count exceeds framebuffer capabilities");
        if (!target.sampleLocations.empty()) {
            Require(target.samples != VK_SAMPLE_COUNT_1_BIT, "custom single-sample depth locations are unsupported");
            Require(context.sampleLocations && (context.sampleLocationProperties.sampleLocationSampleCounts & target.samples) != 0, "custom depth sample locations are unsupported on this device");
            const auto& grid = context.sampleLocationGridSizes[std::countr_zero(static_cast<std::uint32_t>(target.samples))];
            Require(target.sampleLocationsGrid.width == 1u && target.sampleLocationsGrid.height == 1u && grid.width >= 1u && grid.height >= 1u && target.sampleLocations.size() == static_cast<std::uint32_t>(target.samples), "custom depth sample location grid is unsupported");
            Require(context.sampleLocationProperties.sampleLocationSubPixelBits >= 4u, "depth sample locations require four subpixel bits");
            const auto scale = std::ldexp(1.0f, static_cast<int>(std::min(context.sampleLocationProperties.sampleLocationSubPixelBits, 23u)));
            for (const auto& location : target.sampleLocations) {
                Require(std::isfinite(location.x) && std::isfinite(location.y), "depth sample locations must be finite");
                Require(std::round(location.x * scale) == location.x * scale && std::round(location.y * scale) == location.y * scale, "depth sample locations exceed device subpixel precision");
                Require(location.x >= context.sampleLocationProperties.sampleLocationCoordinateRange[0] && location.x <= context.sampleLocationProperties.sampleLocationCoordinateRange[1] &&
                    location.y >= context.sampleLocationProperties.sampleLocationCoordinateRange[0] && location.y <= context.sampleLocationProperties.sampleLocationCoordinateRange[1], "depth sample location exceeds device coordinates");
            }
        }
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
            info.samples = static_cast<VkSampleCountFlagBits>(target.samples);
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
            locations.sampleLocationsPerPixel = static_cast<VkSampleCountFlagBits>(target.samples);
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
        return !retired && htileAddress != 0 && (!htileStencil || target.stencilAddress != 0) && target.address != 0 &&
            target.mipCount == 1u && target.mip == 0u && htileRegistry == GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    }

    std::shared_ptr<StorageTexture> NoteHtileFill(std::uint64_t address, std::size_t bytes, std::uint32_t pattern) {
        const auto footprint = HtileSliceBytes(surfaceExtent(target));
        if (htileAddress == 0 || footprint == 0 || bytes == 0 ||
            (address >= htileAddress ? address - htileAddress >= footprint : htileAddress - address >= bytes)) return nullptr;
        pendingClear = false;
        if (!AcceptsHtileFill() || !HtileFillCovers(htileAddress, surfaceExtent(target), address, bytes)) return nullptr;
        const auto aspects = HtileFillClears(pattern, htileStencil);
        if (aspects == 0) return nullptr;
        const auto generation = GuestMemory::CollectWritesUncached(htileAddress, static_cast<std::size_t>(footprint));
        auto consumed = TakeWrites();
        pendingGeneration = generation;
        pendingClear = aspects;
        pendingTracked = generation != 0 && GuestMemory::Watched(htileAddress, static_cast<std::size_t>(footprint));
        return consumed;
    }

    void ApplyFastClear() {
        const auto aspects = std::exchange(pendingClear, 0u);
        if (aspects == 0) return;
        const auto footprint = static_cast<std::size_t>(HtileSliceBytes(surfaceExtent(target)));
        if (!AcceptsHtileFill()) return;
        if (pendingTracked && (GuestMemory::CollectWritesUncached(htileAddress, footprint) == 0 ||
            GuestMemory::StoredOver(htileAddress, footprint, pendingGeneration))) return;
        auto* recorder = recorderFor(context);
        std::unique_ptr<CommandBatch> batch;
        if (recorder == nullptr) batch = std::make_unique<CommandBatch>(context);
        const auto commands = recorder != nullptr ? recorder->Commands() : batch->Handle();
        const VkImageSubresourceRange range{aspects, 0, 1, 0, 1};
        constexpr VkAccessFlags access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, access, VK_ACCESS_TRANSFER_WRITE_BIT);
        const VkClearDepthStencilValue clear{clearDepth, clearStencil};
        context.Function<PFN_vkCmdClearDepthStencilImage>("vkCmdClearDepthStencilImage")(commands, image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, access);
        if (batch) batch->SubmitAndWait();
        else Recorder::CountBarriers(Recorder::CommandClass::Draw, 2);
    }

    bool StorageAccepts(const StorageTexture& storage, std::uint32_t layer) const {
        const auto& descriptor = storage.Descriptor();
        const auto geometry = DescribeSurface(descriptor);
        const bool d16 = target.format == VK_FORMAT_D16_UNORM || target.format == VK_FORMAT_D16_UNORM_S8_UINT;
        const bool d32 = target.format == VK_FORMAT_D32_SFLOAT || target.format == VK_FORMAT_D32_SFLOAT_S8_UINT;
        const auto extent = surfaceExtent(target);
        const auto offset = geometry.GuestLayerOffset(layer);
        const bool stencil = target.stencilAddress != 0 && offset <= std::numeric_limits<std::uint64_t>::max() - descriptor.baseAddress && descriptor.baseAddress + offset == target.stencilAddress;
        const auto planeAddress = stencil ? target.stencilAddress : target.address;
        const auto elementBytes = stencil ? 1u : d16 ? 2u : 4u;
        const auto stride = DepthSliceBytes(extent, elementBytes);
        const bool sized = !IsConvertedTextureFormat(descriptor.format) && BlockWidth(descriptor.format) == 1u &&
            BlockHeight(descriptor.format) == 1u && BytesPerElement(descriptor.format) == elementBytes &&
            storage.StorageFormat() == ResolveTextureFormat(descriptor.format);
        return !(target.samples != VK_SAMPLE_COUNT_1_BIT || target.mipCount != 1u || target.mip != 0u || planeAddress == 0 ||
            (!d16 && !d32) || !sized || !storage.CanWriteBackLayer(layer) || offset > std::numeric_limits<std::uint64_t>::max() - descriptor.baseAddress ||
            descriptor.baseAddress + offset != planeAddress || (storage.ImageLayers() > 1u && geometry.layerBytes != stride) || descriptor.width != extent.width ||
            descriptor.height != extent.height || descriptor.mipCount != 1u || descriptor.baseLevel != 0u ||
            descriptor.lastLevel != 0u || descriptor.baseArray >= storage.ImageLayers() || layer >= storage.ImageLayers() ||
            (descriptor.dimension != TextureDimension::k2D && descriptor.dimension != TextureDimension::k2DArray) ||
            (descriptor.dimension == TextureDimension::k2D && descriptor.depthOrLastArray != 0u) || storage.ImageDepth() != 1u);
    }

    std::optional<GuestTextureResource> CanonicalResource() const {
        const bool d16 = target.format == VK_FORMAT_D16_UNORM;
        const bool d32 = target.format == VK_FORMAT_D32_SFLOAT;
        const auto extent = surfaceExtent(target);
        if ((!d16 && !d32) || target.stencilAddress != 0 || target.samples != VK_SAMPLE_COUNT_1_BIT || target.mipCount != 1u || target.mip != 0u ||
            ((target.zInfo >> 4u) & 0x1fu) != 24u || (target.zInfo & 3u) != (d16 ? 1u : 3u) || (target.zInfo & 0x000f100cu) != 0u ||
            target.encodedSurfaceAddress == 0 || (target.encodedSurfaceAddress & 0xffu) != 0u || target.encodedSurfaceAddress >= (1ull << 48u) || target.depthSlice > 0x1fffu ||
            extent.width == 0u || extent.height == 0u || extent.width > 16384u || extent.height > 16384u) return std::nullopt;
        const auto elementBytes = d16 ? 2u : 4u;
        const auto bytes = DepthSliceBytes(extent, elementBytes);
        const auto base = target.encodedSurfaceAddress & ~0xffffull;
        const auto offset = static_cast<std::uint64_t>(target.depthSlice) * bytes;
        if (base == 0 || offset > std::numeric_limits<std::uint64_t>::max() - base || target.address != base + offset ||
            bytes > std::numeric_limits<std::uint64_t>::max() - target.address) return std::nullopt;
        const auto* equation = FindTextureSwizzleEquation(24u, elementBytes);
        if (equation == nullptr) return std::nullopt;
        auto xorBytes = static_cast<std::uint32_t>(target.encodedSurfaceAddress & 0xffffu);
        for (std::uint32_t bit = 0; bit < equation->bits.size(); ++bit) {
            xorBytes ^= (std::popcount((target.depthSlice << 24u) & equation->bits[bit]) & 1u) << bit;
        }
        const auto encoded = target.address | xorBytes;
        const std::array<std::uint32_t, 8> words{
            static_cast<std::uint32_t>(encoded >> 8u), static_cast<std::uint32_t>((encoded >> 40u) & 0xffu) |
                ((d16 ? 7u : 22u) << 20u) | (((extent.width - 1u) & 3u) << 30u),
            ((extent.width - 1u) >> 2u) | ((extent.height - 1u) << 14u), 0xfacu | (24u << 20u) | (9u << 28u), 0, 0, 0, 0
        };
        auto resource = DecodeTextureResource(words);
        const auto geometry = DescribeSurface(resource);
        if (resource.baseAddress != target.address || geometry.imageLayers != 1u || geometry.imageDepth != 1u || geometry.layerBytes != bytes) return std::nullopt;
        return resource;
    }

    bool CoversStorage(const GuestTextureResource& canonical, const StorageTexture& storage) const {
        const auto& descriptor = storage.Descriptor();
        if (descriptor.baseAddress < canonical.baseAddress || storage.GuestBytes() > std::numeric_limits<std::uint64_t>::max() - descriptor.baseAddress) return false;
        const auto first = descriptor.baseAddress - canonical.baseAddress;
        const auto last = first + storage.GuestBytes();
        const auto geometry = DescribeSurface(canonical);
        const auto ranges = CoveredMipBytes(canonical.tileMode, BytesPerElement(canonical.format), geometry.mips.front());
        return std::any_of(ranges.begin(), ranges.end(), [&](const auto& range) { return first >= range.first && last <= range.second; });
    }

    void Transfer(StorageTexture& storage, bool intoStorage, std::uint32_t layer) {
        if (!StorageAccepts(storage, layer)) refuseStorage(storage, OverwrittenInMemory());
        const bool d16 = target.format == VK_FORMAT_D16_UNORM || target.format == VK_FORMAT_D16_UNORM_S8_UINT;
        const auto extent = surfaceExtent(target);
        if (transferBuffer == nullptr) transferBuffer = std::make_unique<DeviceBuffer>(context, static_cast<std::size_t>(extent.width) * extent.height * (d16 ? 2u : 4u), VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        auto* recorder = recorderFor(context);
        std::unique_ptr<CommandBatch> batch;
        if (recorder == nullptr) batch = std::make_unique<CommandBatch>(context);
        const auto commands = recorder != nullptr ? recorder->Commands() : batch->Handle();
        if (recorder != nullptr) {
            if (auto self = storage.weak_from_this().lock()) recorder->Keep(std::move(self));
            recorder->Keep(shared_from_this());
        }
        constexpr VkAccessFlags access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
            VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, access, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy depthRegion{};
        const auto storageAddress = storage.Descriptor().baseAddress + DescribeSurface(storage.Descriptor()).GuestLayerOffset(layer);
        const bool stencil = target.stencilAddress != 0 && storageAddress == target.stencilAddress;
        depthRegion.imageSubresource = {stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
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

    std::uint32_t WriterLayer() const { return writerLayer; }
    bool HasWriter(const std::shared_ptr<StorageTexture>& storage) const { return writer == storage; }
    bool HasPendingWriter() const { return writer != nullptr && writer->PendingWrites(); }

    void RequireCanonicalPublication(const StorageTexture& storage) const {
        if (storage.Descriptor().dccAddress != 0 || depthWritten == 0 || !GuestMemory::Watched(target.address, depthBytes()) ||
            !GuestMemory::UnchangedSince(target.address, depthBytes(), depthWritten)) refuseStorage(storage, OverwrittenInMemory());
    }

    bool StorageOwnsSample(const GuestTextureResource& resource) const {
        if (writer == nullptr || !Texture::CanCopyFrom(*writer, resource)) return false;
        const auto format = ResolveTextureFormat(resource.format);
        return writer->StorageFormat() == format && format != VK_FORMAT_R32_SFLOAT && format != VK_FORMAT_R16_UNORM;
    }

    std::shared_ptr<StorageTexture> SeedStorage(const std::shared_ptr<StorageTexture>& storage, std::uint32_t layer) {
        if (!StorageAccepts(*storage, layer)) {
            const bool newerGuestWrites = OverwrittenInMemory();
            if (!newerGuestWrites) refuseStorage(*storage, newerGuestWrites);
            Retire();
            return nullptr;
        }
        if (writer == storage && writerLayer == layer && !pendingClear) return nullptr;
        auto consumed = TakeWrites();
        ApplyFastClear();
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
        return resource.samples == static_cast<std::uint32_t>(target.samples) && (format == expected || depthBits) && resource.dimension == TextureDimension::k2D && resource.width == extent.width && resource.height == extent.height &&
            resource.baseLevel <= resource.lastLevel && resource.lastLevel < target.mipCount && resource.baseArray == 0 && (target.mipCount == 1u || resource.tileMode == TextureTileMode::kZ64KBX);
    }

    std::shared_ptr<Texture> Sampled(std::span<const std::uint32_t> words, const GuestTextureResource& resource, VkComponentMapping components) {
        std::array<std::uint32_t, 12> key{};
        std::copy_n(words.begin(), std::min<std::size_t>(words.size(), 8), key.begin());
        key[8] = components.r;
        key[9] = components.g;
        key[10] = components.b;
        key[11] = components.a;
        if (const auto found = textures.find(key); found != textures.end()) return found->second;
        Require(target.samples == 1, "sampling a multisampled depth surface is not implemented");
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
        if (StorageOwnsSample(resource) || !SampledAccepts(words, resource)) return false;
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
    bool guestPublished = false;
    VkImageAspectFlags pendingClear = 0;
    bool pendingTracked = false;
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

    [[noreturn]] void refuseStorage(const StorageTexture& storage, bool newerGuestWrites) const {
        const auto& descriptor = storage.Descriptor();
        const auto extent = surfaceExtent(target);
        char text[640];
        std::snprintf(text, sizeof(text), "AGC graphics: storage image access to depth surface 0x%llx (%ux%u, vk format %d, %u samples, %u mips) as a %ux%u image of vk format %d, dimension %d, %u mips is not implemented; newer guest writes=%u; DB_Z_INFO=0x%08x encodedBase=0x%llx originalSlice=%u generation=%llu watched=%u aliasDcc=0x%llx",
            static_cast<unsigned long long>(target.address), extent.width, extent.height, static_cast<int>(target.format),
            static_cast<unsigned>(target.samples), target.mipCount, descriptor.width, descriptor.height,
            static_cast<int>(storage.StorageFormat()), static_cast<int>(descriptor.dimension), descriptor.mipCount,
            static_cast<unsigned>(newerGuestWrites), target.zInfo, static_cast<unsigned long long>(target.encodedSurfaceAddress), target.depthSlice,
            static_cast<unsigned long long>(depthWritten), static_cast<unsigned>(GuestMemory::Watched(target.address, depthBytes())),
            static_cast<unsigned long long>(descriptor.dccAddress));
        throw std::runtime_error(text);
    }

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
    return a.address == b.address && a.stencilAddress == b.stencilAddress && aExtent.width == bExtent.width && aExtent.height == bExtent.height && a.mipCount == b.mipCount && a.format == b.format && a.samples == b.samples &&
        (a.zInfo & 0x1f0u) == (b.zInfo & 0x1f0u) && a.encodedSurfaceAddress == b.encodedSurfaceAddress && a.depthSlice == b.depthSlice;
}

std::mutex& surfacesMutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<std::shared_ptr<DepthSurface>>& surfaces() {
    static auto* list = new std::vector<std::shared_ptr<DepthSurface>>();
    return *list;
}

std::map<std::uint64_t, std::uint64_t>& clearedHtiles() {
    static auto* cleared = new std::map<std::uint64_t, std::uint64_t>();
    return *cleared;
}

bool registeredSurface(const std::shared_ptr<DepthSurface>& surface) {
    return !surface->mappingInvalidated && std::find(surfaces().begin(), surfaces().end(), surface) != surfaces().end();
}

std::shared_ptr<StorageTexture> canonicalStorage(const Context& context, const GuestTextureResource& resource, const StorageTexture* except = nullptr) {
    Require(context.detiler != nullptr, "native depth publication requires a texture detiler");
    const auto bytes = static_cast<std::size_t>(DescribeSurface(resource).guestBytes);
    const auto ranges = GuestMemory::CommittedRanges(resource.baseAddress, bytes, true);
    Require(ranges.size() == 1u && ranges.front().first == resource.baseAddress && ranges.front().second - resource.baseAddress == bytes,
        "native depth publication requires a fully committed canonical plane");
    StorageTexture::FlushPending(resource.baseAddress, bytes, except, "native depth layout", PublishScope::None);
    auto storage = StorageTexture::Create(context, *context.detiler, resource, 0);
    if (auto* recorder = recorderFor(context); recorder != nullptr) recorder->Keep(storage);
    return storage;
}

void reloadPublishedDepth(std::unique_lock<std::mutex>& lock, const std::shared_ptr<DepthSurface>& surface) {
    if (!surface->retired || !surface->guestPublished) return;
    const auto resource = surface->CanonicalResource();
    Require(resource.has_value(), "retired depth surface lost its canonical guest layout");
    lock.unlock();
    auto storage = canonicalStorage(surface->context, *resource);
    lock.lock();
    Require(registeredSurface(surface), "depth mapping changed during canonical reload");
    surface->Transfer(*storage, false, 0);
    surface->guestPublished = false;
}

void finishStorageWrites(std::unique_lock<std::mutex>& lock, DepthSurface* surface, const std::shared_ptr<StorageTexture>& consumed, std::uint32_t consumedLayer) {
    if (consumed == nullptr) return;
    const bool publishAll = !consumed->Cached() && std::none_of(surfaces().begin(), surfaces().end(), [&](const auto& current) {
        return current.get() != surface && !current->retired && !current->mappingInvalidated && current->HasWriter(consumed);
    });
    // Write-back acquires the allocation registry, whose mapping callback acquires this mutex.
    lock.unlock();
    if (publishAll) consumed->WriteBack();
    else consumed->WriteBackLayer(consumedLayer);
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

bool SampleLocationsCompatibleDepth(const Context& context, VkFormat format, std::uint32_t samples) {
    if (samples <= 1 || (context.sampleLocationSampleCounts & samples) == 0) return false;
    VkImageFormatProperties properties{};
    const auto result = context.imageFormatProperties(context.physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_CREATE_SAMPLE_LOCATIONS_COMPATIBLE_DEPTH_BIT_EXT, &properties);
    if (result == VK_ERROR_FORMAT_NOT_SUPPORTED) return false;
    Check(result, "vkGetPhysicalDeviceImageFormatProperties sample locations depth");
    return (properties.sampleCounts & samples) != 0;
}

VkImageView DepthSurfaceView(const Context& context, const DepthTarget& target) {
    static_cast<void>(validatedSurfaceExtent(target));
    GuestAllocations::GuestAllocationsSetMappingInvalidator_nid_postfix(&invalidateDepthMappings);
    std::lock_guard gpu(GuestMemory::GpuMutex());
    std::unique_lock lock(surfacesMutex());
    std::shared_ptr<DepthSurface> bound;
    for (const auto& surface : surfaces()) {
        if (!surface->mappingInvalidated && surface->context.device == context.device && sameSurface(surface->target, target)) {
            Require(surface->target.sampleLocationsGrid.width == target.sampleLocationsGrid.width && surface->target.sampleLocationsGrid.height == target.sampleLocationsGrid.height &&
                surface->target.sampleLocations.size() == target.sampleLocations.size() && std::equal(surface->target.sampleLocations.begin(), surface->target.sampleLocations.end(), target.sampleLocations.begin(),
                    [](const auto& a, const auto& b) { return a.x == b.x && a.y == b.y; }), "changing sample locations for an existing depth/stencil surface is unsupported");
            bound = surface;
            break;
        }
    }
    if (bound == nullptr) {
        surfaces().push_back(std::make_shared<DepthSurface>(context, target));
        bound = surfaces().back();
    }
    reloadPublishedDepth(lock, bound);
    bound->Bind(target);
    bound->retired = false;
    if (const auto cleared = clearedHtiles().find(target.htileAddress); cleared != clearedHtiles().end()) {
        if (cleared->second == GuestAllocations::GuestAllocationsGeneration_nid_postfix() && bound->AcceptsHtileFill()) {
            bound->pendingClear |= VK_IMAGE_ASPECT_DEPTH_BIT;
            bound->pendingTracked = false;
        }
        clearedHtiles().erase(cleared);
    }
    for (const auto& surface : surfaces()) {
        if (surface != bound && surface->context.device == context.device && surface->Overlaps(*bound)) surface->Retire();
    }
    const auto consumedLayer = bound->WriterLayer();
    auto consumed = bound->TakeWrites();
    bound->ApplyFastClear();
    bound->NoteWritten();
    const auto view = bound->View(target.mip);
    finishStorageWrites(lock, bound.get(), consumed, consumedLayer);
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
    return htile != 0 && footprint != 0 && address <= htile &&
        bytes <= std::numeric_limits<std::uint64_t>::max() - address && htile - address <= bytes && footprint <= bytes - (htile - address);
}

void NoteDepthMetadataFill(VkDevice device, std::uint64_t address, std::size_t bytes, std::uint32_t pattern) {
    std::lock_guard gpu(GuestMemory::GpuMutex());
    std::unique_lock lock(surfacesMutex());
    // An SDK clear remembered before the first bind must not outlive a later metadata fill.
    std::erase_if(clearedHtiles(), [&](const auto& clear) {
        return bytes != 0 && (address >= clear.first ? address - clear.first < 32768u : clear.first - address < bytes);
    });
    std::size_t candidates = 0;
    for (const auto& surface : surfaces()) {
        if ((device == VK_NULL_HANDLE || surface->context.device == device) && surface->AcceptsHtileFill() && HtileFillCovers(surface->htileAddress, surfaceExtent(surface->target), address, bytes)) ++candidates;
    }
    DepthSurface* owner = nullptr;
    std::shared_ptr<StorageTexture> consumed;
    std::uint32_t consumedLayer = 0;
    for (const auto& surface : surfaces()) {
        if (device != VK_NULL_HANDLE && surface->context.device != device) continue;
        if (auto writer = surface->NoteHtileFill(address, bytes, candidates == 1u ? pattern : 0xffffffffu)) {
            owner = surface.get();
            consumed = std::move(writer);
            consumedLayer = surface->WriterLayer();
        }
    }
    finishStorageWrites(lock, owner, consumed, consumedLayer);
}

std::uint64_t HtileDepthClearAddress(std::span<const std::uint32_t> code, std::span<const std::uint32_t> userData, const std::array<std::uint32_t, 3>& numThreads) {
    static constexpr std::array<std::uint32_t, 17> htileMaskKernel{0xd7460000u, 0x04010c06u, 0x34000084u, 0xdc388000u, 0x04020000u, 0xbf8c3f70u, 0xd7710008u, 0x00120805u, 0xd7710009u, 0x00120a05u, 0xd771000au, 0x00120c05u, 0xd771000bu, 0x00120e05u, 0xdc788000u, 0x00020800u, 0xbf810000u};
    if (userData.size() < 6 || numThreads[0] != 64 || numThreads[1] != 1 || numThreads[2] != 1) return 0;
    if (code.size() < htileMaskKernel.size() || !std::equal(htileMaskKernel.begin(), htileMaskKernel.end(), code.begin())) return 0;
    if ((userData[5] & 0xfu) != 0 || (userData[4] & 0xfu) != 0) return 0;
    return userData[2] | (static_cast<std::uint64_t>(userData[3]) << 32u);
}

void NoteDepthMetadataFill(std::uint64_t address, std::size_t bytes, std::uint32_t pattern) {
    NoteDepthMetadataFill(VK_NULL_HANDLE, address, bytes, pattern);
}

VkImageAspectFlags HtileFillClears(std::uint32_t pattern, bool stencilInHtile) {
    VkImageAspectFlags cleared = (pattern & 0xfu) == 0 ? VK_IMAGE_ASPECT_DEPTH_BIT : 0u;
    if (stencilInHtile && ((pattern >> 8u) & 0x3u) == 0) cleared |= VK_IMAGE_ASPECT_STENCIL_BIT;
    return cleared;
}

std::uint64_t HtileMetadataBytes(VkExtent2D extent) {
    return HtileSliceBytes(extent);
}

void NoteHtileDepthClear(std::uint64_t htileAddress) {
    std::lock_guard lock(surfacesMutex());
    bool existing = false;
    for (const auto& surface : surfaces()) {
        if (surface->htileAddress != htileAddress || !surface->AcceptsHtileFill()) continue;
        surface->pendingClear |= VK_IMAGE_ASPECT_DEPTH_BIT;
        surface->pendingTracked = false;
        existing = true;
    }
    if (!existing) clearedHtiles()[htileAddress] = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
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
    const auto consumedLayer = (*found)->WriterLayer();
    auto consumed = (*found)->TakeWrites();
    (*found)->ApplyFastClear();
    finishStorageWrites(lock, found->get(), consumed, consumedLayer);
    return texture;
}

bool DepthSurfaceHolds(const Context& context, std::span<const std::uint32_t> words, VkComponentMapping components, const Texture* texture) {
    if (texture == nullptr || words.size() != 8u || ((words[3] >> 28u) != 9u && (words[3] >> 28u) != 14u)) return false;
    const auto resource = DecodeTextureResource(words);
    std::lock_guard gpu(GuestMemory::GpuMutex());
    std::unique_lock lock(surfacesMutex());
    const auto& list = surfaces();
    const auto found = std::find_if(list.rbegin(), list.rend(), [&](const auto& surface) {
        return !surface->retired && surface->context.device == context.device && (surface->target.address == resource.baseAddress ||
            (surface->target.stencilAddress != 0 && surface->target.stencilAddress == resource.baseAddress));
    });
    if (found == list.rend() || !(*found)->Holds(words, resource, components, texture)) return false;
    const auto consumedLayer = (*found)->WriterLayer();
    auto consumed = (*found)->TakeWrites();
    (*found)->ApplyFastClear();
    finishStorageWrites(lock, found->get(), consumed, consumedLayer);
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
        const auto planeAddress = containsPlane(surface->target.address) ? surface->target.address : surface->target.stencilAddress;
        Require(planeAddress >= descriptor.baseAddress && planeAddress - descriptor.baseAddress < geometry.guestBytes &&
            (planeAddress - descriptor.baseAddress) % geometry.layerBytes == 0u,
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
            return !surface->retired && !surface->mappingInvalidated && surface->context.device == context.device && (surface->target.address == address || (surface->target.stencilAddress != 0 && surface->target.stencilAddress == address));
        });
        if (found == list.rend()) continue;
        auto surface = *found;
        if (!surface->StorageAccepts(*storage, layer)) {
            if (surface->target.stencilAddress == address) {
                surface->SeedStorage(storage, layer);
                continue;
            }
            const auto bytes = static_cast<std::size_t>(DepthMipChainBytes(surfaceExtent(surface->target),
                surface->target.format == VK_FORMAT_D16_UNORM || surface->target.format == VK_FORMAT_D16_UNORM_S8_UINT ? 2u : 4u, surface->target.mipCount));
            GuestMemory::CollectWritesUncached(surface->target.address, bytes);
            if (surface->OverwrittenInMemory()) {
                surface->Retire();
                lock.unlock();
                storage->Refresh();
                lock.lock();
                continue;
            }
            const auto resource = surface->CanonicalResource();
            if (!resource.has_value()) {
                surface->SeedStorage(storage, layer);
                lock.unlock();
                storage->Refresh();
                lock.lock();
                continue;
            }
            surface->RequireCanonicalPublication(*storage);
            const bool covered = surface->CoversStorage(*resource, *storage);
            lock.unlock();
            storage->ReplaceFromMemory(covered, [&] {
                auto canonical = canonicalStorage(context, *resource, storage.get());
                try {
                    lock.lock();
                    Require(registeredSurface(surface) && !surface->retired, "depth mapping changed during canonical publication");
                    GuestMemory::CollectWritesUncached(surface->target.address, bytes);
                    if (surface->OverwrittenInMemory()) {
                        surface->Retire();
                        lock.unlock();
                        return false;
                    }
                    surface->RequireCanonicalPublication(*storage);
                    Require(!surface->HasPendingWriter(), "native depth publication encountered an unpublished storage writer");
                    const auto consumedLayer = surface->WriterLayer();
                    auto consumed = surface->TakeWrites();
                    surface->ApplyFastClear();
                    surface->Transfer(*canonical, true, 0);
                    lock.unlock();
                    if (consumed != nullptr) consumed->WriteBackLayer(consumedLayer);
                    canonical->MarkDirty();
                    canonical->WriteBackLayer(0);
                    lock.lock();
                    Require(registeredSurface(surface) && !surface->retired, "depth mapping changed while publishing canonical pixels");
                    surface->guestPublished = true;
                    surface->Retire();
                    lock.unlock();
                    return true;
                } catch (...) {
                    if (lock.owns_lock()) lock.unlock();
                    throw;
                }
            });
            lock.lock();
            continue;
        }
        const auto consumedLayer = surface->WriterLayer();
        auto consumed = surface->SeedStorage(storage, layer);
        finishStorageWrites(lock, surface.get(), consumed, consumedLayer);
    }
}

bool DepthSurfaceAt(const Context& context, std::span<const std::uint32_t> words, const GuestTextureResource& resource) {
    std::lock_guard lock(surfacesMutex());
    const auto& list = surfaces();
    const auto found = std::find_if(list.rbegin(), list.rend(), [&](const auto& surface) {
        return !surface->retired && surface->context.device == context.device && (surface->target.address == resource.baseAddress || (surface->target.stencilAddress != 0 && surface->target.stencilAddress == resource.baseAddress));
    });
    if (found == list.rend()) return false;
    const auto& target = (*found)->target;
    if (resource.width != target.extent.width || resource.height != target.extent.height) return false;
    const bool stencil = target.stencilAddress != 0 && target.stencilAddress == resource.baseAddress;
    const bool d16 = target.format == VK_FORMAT_D16_UNORM || target.format == VK_FORMAT_D16_UNORM_S8_UINT;
    const auto expected = stencil ? VK_FORMAT_R8_UINT : d16 ? VK_FORMAT_R16_UNORM : VK_FORMAT_R32_SFLOAT;
    const bool depthBits = !stencil && words.size() >= 4 && ShaderRecompiler::DepthBitsTextureWidth(words[1], words[3]) == (d16 ? 16u : 32u);
    return ResolveTextureFormat(resource.format) == expected || depthBits;
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
