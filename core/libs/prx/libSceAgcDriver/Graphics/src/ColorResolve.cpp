#include "prx/libSceAgcDriver/Graphics/include/ColorResolve.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleColorSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <limits>

namespace AgcDriver::Graphics {

namespace {

void ValidateTarget(const ColorTarget& target) {
    Require((target.format == VK_FORMAT_R8G8B8A8_UNORM || target.format == VK_FORMAT_B8G8R8A8_UNORM) && target.elementBytes == 4u && target.componentMapping == 0xe4u, "color resolve format or component mapping is unsupported");
    Require(target.tileMode == ColorTileMode::RenderTarget, "color resolve tile mode is unsupported");
    Require(target.depth == 1u && target.depthSlice == 0u && target.mipCount == 1u && target.mip == 0u && !target.mipTail, "color resolve arrays, volumes or mipmaps are unsupported");
    Require(target.dccAddress == 0u && target.cmaskAddress == 0u, "color resolve metadata is unsupported");
    Require(target.address != 0u && target.bytes != 0u && target.bytes <= std::numeric_limits<std::uint64_t>::max() - target.address, "color resolve guest address range is invalid");
}

GuestTextureResource DestinationSurface(const ColorTarget& target) {
    const auto format = FindGuestColorTargetFormat(target.format, target.elementBytes);
    Require(format.has_value(), "color resolve destination has no guest texture format");
    GuestTextureResource resource{};
    resource.baseAddress = target.address;
    resource.width = target.extent.width;
    resource.height = target.extent.height;
    resource.depthOrLastArray = 0u;
    resource.baseArray = 0u;
    resource.mipCount = 1u;
    resource.baseLevel = 0u;
    resource.lastLevel = 0u;
    resource.tileMode = ColorTextureTileMode(target.tileMode);
    resource.dimension = TextureDimension::k2D;
    resource.format = *format;
    resource.dstSelX = 4u;
    resource.dstSelY = 5u;
    resource.dstSelZ = 6u;
    resource.dstSelW = 7u;
    return resource;
}

VkImageMemoryBarrier Barrier(VkImage image, VkAccessFlags sourceAccess, VkAccessFlags destinationAccess) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = sourceAccess;
    barrier.dstAccessMask = destinationAccess;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
    return barrier;
}

}

void ResolveColor(const Context& context, const ColorResolvePass& pass) {
    GuestMemory::AssertGpuLockHeld("ResolveColor");
    ValidateTarget(pass.source);
    ValidateTarget(pass.destination);
    Require(pass.source.samples == VK_SAMPLE_COUNT_8_BIT && pass.destination.samples == VK_SAMPLE_COUNT_1_BIT, "color resolve requires eight source samples and one destination sample");
    Require(pass.source.format == pass.destination.format, "color resolve source and destination formats differ");
    Require(pass.source.extent.width == pass.destination.extent.width && pass.source.extent.height == pass.destination.extent.height, "color resolve source and destination extents differ");
    const MultisampleColorLayout sourceLayout(pass.source.extent.width, pass.source.extent.height, pass.source.elementBytes, 8u);
    const ColorTargetLayout destinationLayout(pass.destination.extent.width, pass.destination.extent.height, pass.destination.tileMode, pass.destination.elementBytes);
    Require(pass.source.bytes == sourceLayout.Bytes() && pass.destination.bytes == destinationLayout.Bytes(), "color resolve guest byte ranges disagree with their layouts");
    Require(pass.source.address + pass.source.bytes <= pass.destination.address || pass.destination.address + pass.destination.bytes <= pass.source.address, "color resolve source and destination guest ranges overlap");
    Require(pass.region.offset.x >= 0 && pass.region.offset.y >= 0, "color resolve region has a negative offset");
    const auto right = static_cast<std::uint64_t>(pass.region.offset.x) + pass.region.extent.width;
    const auto bottom = static_cast<std::uint64_t>(pass.region.offset.y) + pass.region.extent.height;
    Require(right <= pass.source.extent.width && bottom <= pass.source.extent.height, "color resolve region exceeds its image extent");
    GuestMemory::CheckRange(reinterpret_cast<const void*>(pass.source.address), pass.source.bytes, sourceLayout.Alignment(), true);
    GuestMemory::CheckRange(reinterpret_cast<const void*>(pass.destination.address), pass.destination.bytes, destinationLayout.Alignment(), true);
    if (pass.region.extent.width == 0u || pass.region.extent.height == 0u) return;

    VkFormatProperties properties{};
    context.formatProperties(context.physical, VK_FORMAT_R8G8B8A8_UNORM, &properties);
    constexpr auto required = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    Require((properties.optimalTilingFeatures & required) == required, "color resolve format lacks required transfer or attachment features");
    if (auto* recorder = Recorder::Active(); recorder != nullptr) recorder->Sync();
    const auto source = CachedMultisampleColorSurface(context, pass.source);
    const auto destination = CachedStorageSurface(context, DestinationSurface(pass.destination));
    Require(destination->Attachable() && destination->GuestBytes() == pass.destination.bytes, "color resolve destination storage image is incompatible");
    Require(StorageFormatForGuest(context, destination->Descriptor().format) == VK_FORMAT_R8G8B8A8_UNORM, "color resolve native image formats differ");
    Require(source->Image() != destination->Image(), "color resolve native images alias");

    CommandBatch batch(context);
    const auto commands = batch.Handle();
    const VkImageMemoryBarrier before[]{
        Barrier(source->Image(), VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
        Barrier(destination->Image(), VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT)
    };
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 2u, before);
    VkImageResolve region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
    region.srcOffset = {pass.region.offset.x, pass.region.offset.y, 0};
    region.dstOffset = region.srcOffset;
    region.extent = {pass.region.extent.width, pass.region.extent.height, 1u};
    context.Function<PFN_vkCmdResolveImage>("vkCmdResolveImage")(commands, source->Image(), VK_IMAGE_LAYOUT_GENERAL, destination->Image(), VK_IMAGE_LAYOUT_GENERAL, 1u, &region);
    const VkImageMemoryBarrier after[]{
        Barrier(source->Image(), VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT),
        Barrier(destination->Image(), VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT)
    };
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u, 0u, nullptr, 0u, nullptr, 2u, after);
    batch.SubmitAndWait();
    destination->MarkDirty();
}

}
