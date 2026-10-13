#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
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
#include <tuple>
#include <vector>

namespace AgcDriver::Graphics {
namespace {
std::uint64_t depthWrites = 0;

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

class DepthSurface;
DepthSurface* drawnDepth = nullptr;

class DepthSurface : public std::enable_shared_from_this<DepthSurface> {
public:
    DepthSurface(const Context& context, const DepthTarget& target) : context(context), target(target), hostFormat(HostDepthFormat(context, target.format)) {
        this->context.bufferPool.reset();
        const auto extent = validatedSurfaceExtent(target);
        VkFormatProperties properties{};
        context.formatProperties(context.physical, hostFormat, &properties);
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
        Check(context.imageFormatProperties(context.physical, hostFormat, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage,
            target.sampleLocations.empty() ? 0u : VK_IMAGE_CREATE_SAMPLE_LOCATIONS_COMPATIBLE_DEPTH_BIT_EXT, &supported), "vkGetPhysicalDeviceImageFormatProperties depth");
        Require((supported.sampleCounts & target.samples) != 0, "depth/stencil image usage does not support the requested sample count");
        Require(extent.width <= supported.maxExtent.width && extent.height <= supported.maxExtent.height, "depth target exceeds format extent limits");
        Require(target.mipCount <= supported.maxMipLevels, "depth target exceeds format mip limits");
        try {
            VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            info.flags = target.sampleLocations.empty() ? 0u : VK_IMAGE_CREATE_SAMPLE_LOCATIONS_COMPATIBLE_DEPTH_BIT_EXT;
            info.imageType = VK_IMAGE_TYPE_2D;
            info.format = hostFormat;
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
            viewInfo.format = hostFormat;
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
        if (aspects != 0) written = ++depthWrites;
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

    std::optional<GuestTextureResource> CanonicalResource(bool stencil = false) const {
        const bool d16 = target.format == VK_FORMAT_D16_UNORM || target.format == VK_FORMAT_D16_UNORM_S8_UINT;
        const bool d32 = target.format == VK_FORMAT_D32_SFLOAT || target.format == VK_FORMAT_D32_SFLOAT_S8_UINT;
        const auto extent = surfaceExtent(target);
        const auto info = stencil ? target.stencilInfo : target.zInfo;
        const auto encodedAddress = stencil ? target.encodedStencilAddress : target.encodedSurfaceAddress;
        const auto address = stencil ? target.stencilAddress : target.address;
        if (hostFormat != target.format || (!d16 && !d32) || (stencil && target.stencilAddress == 0) || target.samples != VK_SAMPLE_COUNT_1_BIT || target.mipCount != 1u || target.mip != 0u ||
            ((info >> 4u) & 0x1fu) != 24u || (info & (stencil ? 1u : 3u)) != (stencil ? 1u : d16 ? 1u : 3u) || (info & 0x000f100cu) != 0u ||
            encodedAddress == 0 || (encodedAddress & 0xffu) != 0u || encodedAddress >= (1ull << 48u) || target.depthSlice > 0x1fffu ||
            extent.width == 0u || extent.height == 0u || extent.width > 16384u || extent.height > 16384u) return std::nullopt;
        const auto elementBytes = stencil ? 1u : d16 ? 2u : 4u;
        const auto bytes = DepthSliceBytes(extent, elementBytes);
        const auto base = encodedAddress & ~0xffffull;
        const auto offset = static_cast<std::uint64_t>(target.depthSlice) * bytes;
        if (base == 0 || offset > std::numeric_limits<std::uint64_t>::max() - base || address != base + offset ||
            bytes > std::numeric_limits<std::uint64_t>::max() - address) return std::nullopt;
        const auto* equation = FindTextureSwizzleEquation(24u, elementBytes);
        if (equation == nullptr) return std::nullopt;
        auto xorBytes = static_cast<std::uint32_t>(encodedAddress & 0xffffu);
        for (std::uint32_t bit = 0; bit < equation->bits.size(); ++bit) {
            xorBytes ^= (std::popcount((target.depthSlice << 24u) & equation->bits[bit]) & 1u) << bit;
        }
        const auto encoded = address | xorBytes;
        const std::array<std::uint32_t, 8> words{
            static_cast<std::uint32_t>(encoded >> 8u), static_cast<std::uint32_t>((encoded >> 40u) & 0xffu) |
                ((stencil ? 5u : d16 ? 7u : 22u) << 20u) | (((extent.width - 1u) & 3u) << 30u),
            ((extent.width - 1u) >> 2u) | ((extent.height - 1u) << 14u), 0xfacu | (24u << 20u) | (9u << 28u), 0, 0, 0, 0
        };
        auto resource = DecodeTextureResource(words);
        const auto geometry = DescribeSurface(resource);
        if (resource.baseAddress != address || geometry.imageLayers != 1u || geometry.imageDepth != 1u || geometry.layerBytes != bytes) return std::nullopt;
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
        Require(storage.Descriptor().baseAddress == target.stencilAddress || hostFormat == target.format, "storage access to a depth plane stored in another host depth format is unsupported");
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
    bool HasWriterPointer(const StorageTexture* storage) const { return writer.get() == storage; }
    bool HasPendingWriter() const { return writer != nullptr && writer->PendingWrites(); }

    void RequireCanonicalPublication(const StorageTexture& storage) const {
        if ((target.stencilAddress != 0 && !CanonicalResource(true).has_value()) || storage.Descriptor().dccAddress != 0 || depthWritten == 0 || !GuestMemory::Watched(target.address, depthBytes()) ||
            !GuestMemory::UnchangedSince(target.address, depthBytes(), depthWritten) ||
            (target.stencilAddress != 0 && (stencilWritten == 0 || !GuestMemory::Watched(target.stencilAddress, stencilBytes()) ||
                !GuestMemory::UnchangedSince(target.stencilAddress, stencilBytes(), stencilWritten)))) refuseStorage(storage, OverwrittenInMemory());
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
        return resource.samples == static_cast<std::uint32_t>(target.samples) && (format == expected || depthBits) && (resource.dimension == TextureDimension::k2D || (resource.dimension == TextureDimension::k2DArray && resource.depthOrLastArray == 0)) && resource.width == extent.width && resource.height == extent.height &&
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
        const bool stencil = target.stencilAddress != 0 && resource.baseAddress == target.stencilAddress;
        if (!SampledAccepts(words, resource)) {
            char text[448];
            std::snprintf(text, sizeof(text), "AGC graphics: sampling the %s plane of depth surface 0x%llx (%ux%u, vk format %d) as a %ux%u texture of guest format %u (vk %d), tile mode %u, dimension %d, levels %u-%u, slice %u is not implemented (T# %08x %08x %08x %08x %08x %08x %08x %08x)",
                          stencil ? "stencil" : "depth", static_cast<unsigned long long>(target.address), target.extent.width, target.extent.height, static_cast<int>(target.format), resource.width, resource.height, resource.format, static_cast<int>(ResolveTextureFormat(resource.format)),
                          static_cast<unsigned>(resource.tileMode), static_cast<int>(resource.dimension), resource.baseLevel, resource.lastLevel, resource.baseArray, key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7]);
            throw std::runtime_error(text);
        }
        auto texture = std::make_shared<Texture>(context, image, hostFormat, stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, components, resource.dimension == TextureDimension::k2DArray ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D, resource.baseLevel, resource.lastLevel - resource.baseLevel + 1u);
        textures.emplace(key, texture);
        return texture;
    }

    bool HoldsTexture(const Texture* texture) const {
        return !retired && !pendingClear && !HasPendingWriter() && std::any_of(textures.begin(), textures.end(), [&](const auto& entry) { return entry.second.get() == texture; });
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
        written = ++depthWrites;
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
    const VkFormat hostFormat;
    std::uint64_t written = ++depthWrites;
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

    std::array<std::shared_ptr<StorageTexture>, 2> storagePlanes;
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
        char text[896];
        std::snprintf(text, sizeof(text), "AGC graphics: storage image access to depth surface 0x%llx (%ux%u, vk format %d, %u samples, %u mips) as a %ux%u image of vk format %d, dimension %d, %u mips is not implemented; newer guest writes=%u; DB_Z_INFO=0x%08x encodedBase=0x%llx originalSlice=%u generation=%llu watched=%u aliasDcc=0x%llx canonicalDepth=%u canonicalOther=%u DB_STENCIL_INFO=0x%08x stencil=0x%llx encodedStencil=0x%llx stencilGeneration=%llu stencilWatched=%u",
            static_cast<unsigned long long>(target.address), extent.width, extent.height, static_cast<int>(target.format),
            static_cast<unsigned>(target.samples), target.mipCount, descriptor.width, descriptor.height,
            static_cast<int>(storage.StorageFormat()), static_cast<int>(descriptor.dimension), descriptor.mipCount,
            static_cast<unsigned>(newerGuestWrites), target.zInfo, static_cast<unsigned long long>(target.encodedSurfaceAddress), target.depthSlice,
            static_cast<unsigned long long>(depthWritten), static_cast<unsigned>(GuestMemory::Watched(target.address, depthBytes())),
            static_cast<unsigned long long>(descriptor.dccAddress), static_cast<unsigned>(CanonicalResource().has_value()),
            static_cast<unsigned>(CanonicalResource(true).has_value()), target.stencilInfo,
            static_cast<unsigned long long>(target.stencilAddress), static_cast<unsigned long long>(target.encodedStencilAddress),
            static_cast<unsigned long long>(stencilWritten), static_cast<unsigned>(target.stencilAddress != 0 && GuestMemory::Watched(target.stencilAddress, stencilBytes())));
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

struct DepthPlaneSlice {
    VkImage image = VK_NULL_HANDLE;
    std::uint64_t written = 0;
};

class DepthPlaneCopy {
public:
    DepthPlaneCopy(const Context& context, VkExtent2D extent, VkFormat format, std::uint32_t layers) : context(context), extent(extent), format(format), layers(layers), guestLayers(layers), surfaceLayers(layers) {
        this->context.bufferPool.reset();
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.flags = layers % 6u == 0 && extent.width == extent.height ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0u;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {extent.width, extent.height, 1};
        info.mipLevels = 1;
        info.arrayLayers = layers;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        try {
            Check(context.Function<PFN_vkCreateImage>("vkCreateImage")(context.device, &info, nullptr, &image), "vkCreateImage depth plane copy");
            VkMemoryRequirements requirements{};
            context.Function<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(context.device, image, &requirements);
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            Check(context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")(context.device, &allocation, nullptr, &memory), "vkAllocateMemory depth plane copy");
            Check(context.Function<PFN_vkBindImageMemory>("vkBindImageMemory")(context.device, image, memory, 0), "vkBindImageMemory depth plane copy");
            staging = std::make_unique<DeviceBuffer>(context, static_cast<std::size_t>(sliceBytes()) * layers, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        } catch (...) {
            release();
            throw;
        }
    }
    ~DepthPlaneCopy() { release(); }
    DepthPlaneCopy(const DepthPlaneCopy&) = delete;
    DepthPlaneCopy& operator=(const DepthPlaneCopy&) = delete;

    std::shared_ptr<Texture> Refresh(std::span<const DepthPlaneSlice> slices, const GuestTextureResource& resource, VkComponentMapping components, VkImageViewType viewType) {
        Require(slices.size() == layers, "depth plane copy slices do not match its layers");
        const auto geometry = DescribeSurface(resource);
        Require(geometry.layers == layers && geometry.sliceLinearBytes == sliceBytes() && !geometry.mips.empty(), "depth plane copy geometry does not match its layers");
        std::vector<std::shared_ptr<Buffer>> uploads;
        std::vector<std::uint32_t> uploadLayers;
        std::vector<std::uint32_t> copiedLayers;
        for (std::uint32_t layer = 0; layer < layers; ++layer) {
            auto& held = guestLayers[layer];
            auto& surface = surfaceLayers[layer];
            if (slices[layer].image != VK_NULL_HANDLE) {
                held = {};
                if (slices[layer].written != 0 && surface.image == slices[layer].image && surface.written == slices[layer].written) continue;
                surface = slices[layer];
                copiedLayers.push_back(layer);
                continue;
            }
            surface = {};
            const GuestLayer current{resource.baseAddress + geometry.GuestLayerOffset(layer), geometry.layerBytes, resource.tileMode, geometry.thick, GuestMemory::CollectWrites(resource.baseAddress + geometry.GuestLayerOffset(layer), static_cast<std::size_t>(geometry.layerBytes))};
            if (held.generation != 0 && held.address == current.address && held.bytes == current.bytes && held.tileMode == current.tileMode && held.thick == current.thick && !Recorder::SnapshotWriteOverlaps(current.address, static_cast<std::size_t>(current.bytes)) && GuestMemory::UnchangedSince(current.address, static_cast<std::size_t>(current.bytes), held.generation)) {
                held.generation = current.generation;
                continue;
            }
            auto upload = std::make_shared<Buffer>(context, static_cast<std::size_t>(geometry.layerBytes), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            GuestMemory::ReadCommitted(current.address, upload->Bytes().first(static_cast<std::size_t>(geometry.layerBytes)));
            uploads.push_back(std::move(upload));
            uploadLayers.push_back(layer);
            held = current;
        }
        if (!copiedLayers.empty() || !uploads.empty()) record(slices, resource, geometry, copiedLayers, uploads, uploadLayers);
        const std::array<std::uint32_t, 5> key{static_cast<std::uint32_t>(components.r), static_cast<std::uint32_t>(components.g), static_cast<std::uint32_t>(components.b), static_cast<std::uint32_t>(components.a), static_cast<std::uint32_t>(viewType)};
        auto& texture = textures[key];
        if (texture == nullptr) {
            texture = std::make_shared<Texture>(context, image, format, aspect(), components, viewType);
            texture->MarkRefreshedPerUse();
        }
        return texture;
    }

    const Context context;
    const VkExtent2D extent;
    const VkFormat format;
    const std::uint32_t layers;

private:
    struct GuestLayer {
        std::uint64_t address = 0;
        std::uint64_t bytes = 0;
        TextureTileMode tileMode = TextureTileMode::kLinear;
        bool thick = false;
        std::uint64_t generation = 0;
    };

    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::unique_ptr<DeviceBuffer> staging;
    std::map<std::array<std::uint32_t, 5>, std::shared_ptr<Texture>> textures;
    std::vector<GuestLayer> guestLayers;
    std::vector<DepthPlaneSlice> surfaceLayers;
    bool filled = false;

    void record(std::span<const DepthPlaneSlice> slices, const GuestTextureResource& resource, const SurfaceGeometry& geometry, std::span<const std::uint32_t> copiedLayers, std::span<const std::shared_ptr<Buffer>> uploads, std::span<const std::uint32_t> uploadLayers) {
        if (!uploads.empty()) {
            Require(context.detiler != nullptr, "depth plane copy requires a texture detiler");
            context.detiler->BeginBatch();
        }
        auto* recorder = recorderFor(context);
        std::unique_ptr<CommandBatch> batch;
        if (recorder == nullptr) batch = std::make_unique<CommandBatch>(context);
        const auto commands = recorder != nullptr ? recorder->Commands() : batch->Handle();
        const auto barrier = context.Resolved(&DeviceFunctions::cmdPipelineBarrier, "vkCmdPipelineBarrier");
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        std::vector<VkBufferImageCopy> regions;
        const auto region = [&](std::uint32_t layer, VkImageAspectFlags aspectMask, std::uint32_t imageLayer) {
            VkBufferImageCopy copy{};
            copy.bufferOffset = sliceBytes() * layer;
            copy.imageSubresource = {aspectMask, 0, imageLayer, 1};
            copy.imageExtent = {extent.width, extent.height, 1};
            return copy;
        };
        for (const auto layer : copiedLayers) {
            const auto copy = region(layer, VK_IMAGE_ASPECT_DEPTH_BIT, 0);
            context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(commands, slices[layer].image, VK_IMAGE_LAYOUT_GENERAL, staging->Handle(), 1, &copy);
            regions.push_back(region(layer, aspect(), layer));
        }
        if (!uploads.empty()) {
            RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            for (std::size_t upload = 0; upload < uploads.size(); ++upload) {
                const auto layer = uploadLayers[upload];
                context.detiler->Dispatch(commands, resource.tileMode, format == VK_FORMAT_D32_SFLOAT ? 4u : 2u, uploads[upload]->Handle(), 0, staging->Handle(), geometry.LinearLayerOffset(layer), geometry.mips.front(), false, layer, geometry.thick);
                regions.push_back(region(layer, aspect(), layer));
            }
            if (recorder != nullptr) {
                for (const auto& upload : uploads) recorder->Keep(upload);
            }
        }
        VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toTransfer.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toTransfer.oldLayout = filled ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = image;
        toTransfer.subresourceRange = {aspect(), 0, 1, 0, layers};
        const VkBufferMemoryBarrier staged{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, staging->Handle(), 0, VK_WHOLE_SIZE};
        barrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &staged, 1, &toTransfer);
        context.Function<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(commands, staging->Handle(), image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<std::uint32_t>(regions.size()), regions.data());
        VkImageMemoryBarrier toGeneral = toTransfer;
        toGeneral.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toGeneral.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &toGeneral);
        filled = true;
        if (batch) batch->SubmitAndWait();
        else Recorder::CountBarriers(Recorder::CommandClass::Draw, 3);
    }

    VkDeviceSize sliceBytes() const {
        return static_cast<VkDeviceSize>(extent.width) * extent.height * (format == VK_FORMAT_D32_SFLOAT ? 4u : 2u);
    }

    VkImageAspectFlags aspect() const {
        return format == VK_FORMAT_R16_UINT ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    }

    void release() noexcept {
        textures.clear();
        staging.reset();
        if (image) context.Function<PFN_vkDestroyImage>("vkDestroyImage")(context.device, image, nullptr);
        if (memory) context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, memory, nullptr);
        image = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
    }
};

std::map<std::tuple<VkDevice, std::uint64_t, std::uint32_t, std::uint32_t, VkFormat, std::uint32_t>, std::unique_ptr<DepthPlaneCopy>>& planeCopies() {
    static auto* copies = new std::map<std::tuple<VkDevice, std::uint64_t, std::uint32_t, std::uint32_t, VkFormat, std::uint32_t>, std::unique_ptr<DepthPlaneCopy>>();
    return *copies;
}


bool sameSurface(const DepthTarget& a, const DepthTarget& b) {
    const auto aExtent = surfaceExtent(a);
    const auto bExtent = surfaceExtent(b);
    return a.address == b.address && a.stencilAddress == b.stencilAddress && aExtent.width == bExtent.width && aExtent.height == bExtent.height && a.mipCount == b.mipCount && a.format == b.format && a.samples == b.samples &&
        (a.zInfo & 0x1f0u) == (b.zInfo & 0x1f0u) && a.encodedSurfaceAddress == b.encodedSurfaceAddress && a.depthSlice == b.depthSlice &&
        (a.stencilInfo & 0x1f0u) == (b.stencilInfo & 0x1f0u) && a.encodedStencilAddress == b.encodedStencilAddress;
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
    if (surface->target.stencilAddress != 0) {
        const auto stencilResource = surface->CanonicalResource(true);
        Require(stencilResource.has_value(), "retired stencil surface lost its canonical guest layout");
        lock.unlock();
        auto stencilStorage = canonicalStorage(surface->context, *stencilResource);
        lock.lock();
        Require(registeredSurface(surface), "depth mapping changed during stencil reload");
        surface->Transfer(*stencilStorage, false, 0);
    }
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

VkFormat HostDepthFormat(const Context& context, VkFormat format) {
    if (format != VK_FORMAT_D16_UNORM_S8_UINT) return format;
    static std::mutex mutex;
    static std::map<VkPhysicalDevice, VkFormat> chosen;
    std::lock_guard lock(mutex);
    if (const auto found = chosen.find(context.physical); found != chosen.end()) return found->second;
    constexpr VkFormatFeatureFlags required = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    auto host = format;
    for (const auto candidate : {VK_FORMAT_D16_UNORM_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT}) {
        VkFormatProperties properties{};
        context.formatProperties(context.physical, candidate, &properties);
        if ((properties.optimalTilingFeatures & required) != required) continue;
        host = candidate;
        break;
    }
    chosen.emplace(context.physical, host);
    return host;
}

float HostDepthBiasConstant(const Context& context, VkFormat format, float constant) {
    const auto host = HostDepthFormat(context, format);
    if (constant == 0.0f || host == format) return constant;
    Require(host == VK_FORMAT_D24_UNORM_S8_UINT, "a constant depth bias on a 16-bit depth target with stencil that the device stores as 32-bit float depth is not implemented");
    return constant * 256.0f;
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
    if (drawnDepth != nullptr && drawnDepth != bound.get()) drawnDepth->NoteWritten();
    drawnDepth = bound.get();
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

std::optional<MemoryValueFill> MatchMemoryValueFill(std::span<const std::uint32_t> code, std::span<const std::uint32_t> userData, const std::array<std::uint32_t, 3>& numThreads, const std::array<std::uint32_t, 3>& groups) {
    static constexpr std::array<std::uint32_t, 9> kernel{0xd7460000u, 0x04010c08u, 0xf4201a82u, 0xfa000000u, 0xbf8cc07fu, 0x7e02026au, 0xe0102000u, 0x80000100u, 0xbf810000u};
    if (userData.size() < 8 || numThreads != std::array<std::uint32_t, 3>{64u, 1u, 1u} || groups[1] != 1u || groups[2] != 1u) return std::nullopt;
    if (code.size() < kernel.size() || !std::equal(kernel.begin(), kernel.end(), code.begin())) return std::nullopt;
    const auto stride = (userData[1] >> 16u) & 0x3fffu;
    const bool swizzled = ((userData[1] >> 31u) & 1u) != 0;
    const bool addTid = ((userData[3] >> 23u) & 1u) != 0;
    const auto type = userData[3] >> 30u;
    const auto format = (userData[3] >> 12u) & 0x7fu;
    const auto valueStride = (userData[5] >> 16u) & 0x3fffu;
    const auto valueBytes = valueStride == 0 ? static_cast<std::uint64_t>(userData[6]) : static_cast<std::uint64_t>(userData[6]) * valueStride;
    if (type != 0 || stride != 4 || swizzled || addTid || format < 20u || format > 22u || valueBytes < 4u) return std::nullopt;
    const auto dwords = std::min<std::uint64_t>(static_cast<std::uint64_t>(groups[0]) * 64u, userData[2]);
    const auto address = userData[0] | (static_cast<std::uint64_t>(userData[1] & 0xffffu) << 32u);
    const auto valueAddress = userData[4] | (static_cast<std::uint64_t>(userData[5] & 0xffffu) << 32u);
    if (dwords == 0 || valueAddress == 0 || (valueAddress & 3u) != 0) return std::nullopt;
    return MemoryValueFill{address, static_cast<std::size_t>(dwords * 4u), valueAddress};
}

void NoteMemoryValueFill(const MemoryValueFill& fill) {
    std::uint32_t value = 0;
    GuestMemory::Read(fill.valueAddress, std::as_writable_bytes(std::span(&value, 1)), 4);
    NoteDepthMetadataFill(fill.address, fill.bytes, value);
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
    if (drawnDepth != nullptr && drawnDepth->context.device == device) drawnDepth = nullptr;
    std::erase_if(surfaces(), [&](const auto& surface) { return surface->context.device == device; });
    std::erase_if(planeCopies(), [&](const auto& entry) { return std::get<0>(entry.first) == device; });
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
    const bool cube = resource.dimension == TextureDimension::kCube;
    const bool layered = cube || (resource.dimension == TextureDimension::k2DArray && resource.depthOrLastArray != 0);
    const auto& base = (*found)->target;
    const auto format = ResolveTextureFormat(resource.format);
    const auto extent = surfaceExtent(base);
    Require(base.samples == VK_SAMPLE_COUNT_1_BIT || (*found)->SampledAccepts(words, resource),
        "sampling this depth view requires multisampled texture materialization");
    if (resource.width != extent.width || resource.height != extent.height) {
        if ((*found)->OverwrittenInMemory()) (*found)->Retire();
        return nullptr;
    }
    const bool stencil = base.stencilAddress != 0 && resource.baseAddress == base.stencilAddress;
    const bool nativeD16 = base.format == VK_FORMAT_D16_UNORM || base.format == VK_FORMAT_D16_UNORM_S8_UINT;
    const auto expectedFormat = nativeD16 ? VK_FORMAT_R16_UNORM : VK_FORMAT_R32_SFLOAT;
    const bool recognizedBits = !stencil && words.size() >= 4 && ShaderRecompiler::DepthBitsTextureWidth(words[1], words[3]) == (nativeD16 ? 16u : 32u);
    if ((!stencil && format != expectedFormat && !recognizedBits && !(nativeD16 && format == VK_FORMAT_R16_UINT)) || (stencil && BytesPerElement(resource.format) != 1u)) return nullptr;
    const bool bitsView = words.size() >= 4 && ShaderRecompiler::DepthBitsTextureWidth(words[1], words[3]) != 0;
    const bool integer = (base.format == VK_FORMAT_D16_UNORM || base.format == VK_FORMAT_D16_UNORM_S8_UINT) && format == VK_FORMAT_R16_UINT && !bitsView;
    if (!stencil && (layered || integer)) {
        Require((*found)->hostFormat == base.format, "sampling several slices or an integer view of a depth plane that the device stores in another depth format is not implemented");
        const bool d16 = base.format == VK_FORMAT_D16_UNORM || base.format == VK_FORMAT_D16_UNORM_S8_UINT;
        const bool depthBits = words.size() >= 4 && ShaderRecompiler::DepthBitsTextureWidth(words[1], words[3]) == (d16 ? 16u : 32u);
        const auto copyFormat = integer ? VK_FORMAT_R16_UINT : d16 ? VK_FORMAT_R16_UNORM : VK_FORMAT_R32_SFLOAT;
        const auto layers = cube ? 6u : layered ? resource.depthOrLastArray + 1u : 1u;
        Require(base.samples == 1u && base.mip == 0u && (*found)->hostFormat == base.format && (format == copyFormat || depthBits) && resource.width == base.extent.width && resource.height == base.extent.height && resource.baseLevel == 0 && resource.lastLevel == 0 && resource.baseArray == 0, "sampling the depth planes of depth surface: unsupported layered or integer view");
        std::vector<DepthPlaneSlice> slices;
        for (std::uint32_t layer = 0; layer < layers; ++layer) {
            const auto address = base.address + static_cast<std::uint64_t>(layer) * DepthSliceBytes(base.extent, d16 ? 2u : 4u);
            const auto slice = std::find_if(list.rbegin(), list.rend(), [&](const auto& surface) { return surface->context.device == context.device && surface->target.address == address && surface->target.extent.width == base.extent.width && surface->target.extent.height == base.extent.height && surface->target.format == base.format; });
            if (slice != list.rend()) {
                (*slice)->ApplyFastClear();
                if (auto* recorder = recorderFor(context)) recorder->Keep(*slice);
            }
            if (slice == list.rend()) slices.push_back({});
            else slices.push_back({(*slice)->image, slice->get() == drawnDepth ? 0u : (*slice)->written});
        }
        const auto imageFormat = integer ? VK_FORMAT_R16_UINT : d16 ? VK_FORMAT_D16_UNORM : VK_FORMAT_D32_SFLOAT;
        auto& copy = planeCopies()[{context.device, base.address, base.extent.width, base.extent.height, imageFormat, layers}];
        if (copy == nullptr) {
            copy = std::make_unique<DepthPlaneCopy>(context, base.extent, imageFormat, layers);
        }
        return copy->Refresh(slices, resource, components, cube || resource.dimension == TextureDimension::k2DArray ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D);
    }
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
            const bool stencilAlias = surface->target.stencilAddress == address;
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
            const auto resource = surface->CanonicalResource(stencilAlias);
            if (!resource.has_value()) {
                Require(surface->target.samples == 1u, "storage image access to a multisampled depth surface is not implemented");
                // An unproven alias keeps the ordinary guest-memory interpretation.
                continue;
            }
            surface->RequireCanonicalPublication(*storage);
            const auto otherResource = stencilAlias ? surface->CanonicalResource() :
                surface->target.stencilAddress != 0 ? surface->CanonicalResource(true) : std::nullopt;
            const bool covered = surface->CoversStorage(*resource, *storage);
            lock.unlock();
            storage->ReplaceFromMemory(covered, [&] {
                auto canonical = canonicalStorage(context, *resource, storage.get());
                auto canonicalOther = otherResource ? canonicalStorage(context, *otherResource, storage.get()) : nullptr;
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
                    if (canonicalOther != nullptr) surface->Transfer(*canonicalOther, true, 0);
                    lock.unlock();
                    if (consumed != nullptr) consumed->WriteBackLayer(consumedLayer);
                    canonical->MarkDirty();
                    canonical->WriteBackLayer(0);
                    if (canonicalOther != nullptr) {
                        canonicalOther->MarkDirty();
                        canonicalOther->WriteBackLayer(0);
                    }
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

bool DepthSurfaceHolds(const Context& context, const GuestTextureResource& resource, const Texture* texture) {
    std::lock_guard gpu(GuestMemory::GpuMutex());
    std::lock_guard lock(surfacesMutex());
    return std::any_of(surfaces().begin(), surfaces().end(), [&](const auto& surface) {
        return surface->context.device == context.device && (surface->target.address == resource.baseAddress || surface->target.stencilAddress == resource.baseAddress) && surface->HoldsTexture(texture);
    });
}

std::shared_ptr<StorageTexture> DepthSurfaceStorage(const Context& context, std::span<const std::uint32_t> words, const GuestTextureResource& resource, std::uint32_t mip) {
    std::shared_ptr<DepthSurface> surface;
    std::shared_ptr<StorageTexture> storage;
    bool stencil = false;
    {
        std::lock_guard lock(surfacesMutex());
        const auto found = std::find_if(surfaces().rbegin(), surfaces().rend(), [&](const auto& entry) {
            return !entry->retired && !entry->mappingInvalidated && entry->context.device == context.device &&
                (entry->target.address == resource.baseAddress || (entry->target.stencilAddress != 0 && entry->target.stencilAddress == resource.baseAddress));
        });
        if (found == surfaces().rend()) return nullptr;
        surface = *found;
        const auto& target = surface->target;
        if (resource.width != target.extent.width || resource.height != target.extent.height) return nullptr;
        stencil = target.stencilAddress != 0 && target.stencilAddress == resource.baseAddress;
        const bool d16 = target.format == VK_FORMAT_D16_UNORM || target.format == VK_FORMAT_D16_UNORM_S8_UINT;
        const auto expected = stencil ? VK_FORMAT_R8_UINT : d16 ? VK_FORMAT_R16_UNORM : VK_FORMAT_R32_SFLOAT;
        const bool depthBits = !stencil && words.size() >= 4 && ShaderRecompiler::DepthBitsTextureWidth(words[1], words[3]) == (d16 ? 16u : 32u);
        if (ResolveTextureFormat(resource.format) != expected && !depthBits) return nullptr;
        Require(stencil || surface->hostFormat == target.format, "storage access to a depth plane stored as another host depth format is not implemented");
        Require(target.samples == 1u, "storage image access to a multisampled depth surface is not implemented");
        // Full mip chains and arrays retain the established storage path.
        if (target.mipCount != 1u) return nullptr;
        if (resource.dimension == TextureDimension::k2DArray && resource.depthOrLastArray != 0 &&
            (target.stencilAddress == 0 || surface->CanonicalResource(stencil))) return nullptr;
        Require(mip == 0 && resource.mipCount == 1 && resource.baseLevel == 0 && resource.lastLevel == 0 && resource.baseArray == 0 &&
            (resource.dimension == TextureDimension::k2D || (resource.dimension == TextureDimension::k2DArray && resource.depthOrLastArray == 0)) && target.samples == 1u,
            "storage image access to depth/stencil surface: this view is not implemented");
        storage = surface->storagePlanes[stencil ? 1u : 0u];
        if (storage && (storage->Descriptor().dimension != resource.dimension || storage->Descriptor().format != resource.format || storage->Descriptor().tileMode != resource.tileMode)) storage.reset();
    }
    if (!storage) {
        storage = StorageTexture::Create(context, *context.detiler, resource, mip);
        std::lock_guard lock(surfacesMutex());
        surface->storagePlanes[stencil ? 1u : 0u] = storage;
    }
    SeedStorageFromDepth(context, storage);
    return storage;
}

bool DepthSurfaceStoreBack(const Context& context, const StorageTexture& storage) {
    std::lock_guard gpu(GuestMemory::GpuMutex());
    std::lock_guard lock(surfacesMutex());
    bool stored = false;
    for (const auto& surface : surfaces()) {
        if (surface->context.device == context.device && !surface->mappingInvalidated && !surface->HasWriterPointer(&storage)) {
            const auto address = storage.Descriptor().baseAddress;
            const auto& current = surface->storagePlanes[address == surface->target.stencilAddress ? 1u : 0u];
            Require((address != surface->target.address && address != surface->target.stencilAddress) || !current || current.get() == &storage,
                "storage image written through a view that another view of the plane replaced");
        }
        if (surface->context.device != context.device || surface->mappingInvalidated ||
            (!surface->HasWriterPointer(&storage) && std::none_of(surface->storagePlanes.begin(), surface->storagePlanes.end(), [&](const auto& plane) { return plane.get() == &storage; }))) continue;
        stored = true;
        if (surface->HasWriterPointer(&storage)) static_cast<void>(surface->TakeWrites());
        else surface->Transfer(*(surface->storagePlanes[0].get() == &storage ? surface->storagePlanes[0] : surface->storagePlanes[1]), false, 0);
        surface->retired = false;
        surface->NoteWritten();
    }
    return stored;
}

}
