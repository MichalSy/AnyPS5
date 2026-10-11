#include "prx/libSceAgcDriver/Graphics/include/ColorResolve.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleColorSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Pipeline.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <array>
#include <cstdlib>
#include <limits>

namespace AgcDriver::Graphics {

namespace {

void ValidateTarget(const ColorTarget& target) {
    const bool wide = target.format == VK_FORMAT_R16G16B16A16_SFLOAT;
    Require((wide || target.format == VK_FORMAT_R8G8B8A8_UNORM || target.format == VK_FORMAT_B8G8R8A8_UNORM || target.format == VK_FORMAT_R8G8B8A8_SRGB || target.format == VK_FORMAT_B8G8R8A8_SRGB || target.format == VK_FORMAT_B10G11R11_UFLOAT_PACK32) &&
        target.elementBytes == (wide ? 8u : 4u) && target.componentMapping == 0xe4u, "color resolve format or component mapping is unsupported");
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
    resource.pipeBankXor = target.pipeBankXor;
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

void ResolveAttachment(const Context& context, const ColorResolvePass& pass, MultisampleColorSurface& source, StorageTexture& destination) {
    VkRenderPass renderPass = VK_NULL_HANDLE;
    try {
        std::array<VkAttachmentDescription, 2> attachments{};
        for (std::size_t index = 0; index < attachments.size(); ++index) {
            auto& attachment = attachments[index];
            attachment.format = pass.source.format;
            attachment.samples = index == 0u ? pass.source.samples : VK_SAMPLE_COUNT_1_BIT;
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        }
        const VkAttachmentReference color{0u, VK_IMAGE_LAYOUT_GENERAL};
        const VkAttachmentReference resolved{1u, VK_IMAGE_LAYOUT_GENERAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1u;
        subpass.pColorAttachments = &color;
        subpass.pResolveAttachments = &resolved;
        VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        info.attachmentCount = static_cast<std::uint32_t>(attachments.size());
        info.pAttachments = attachments.data();
        info.subpassCount = 1u;
        info.pSubpasses = &subpass;
        Check(context.Function<PFN_vkCreateRenderPass>("vkCreateRenderPass")(context.device, &info, nullptr, &renderPass), "vkCreateRenderPass attachment color resolve");
        const std::array views{source.AttachmentView(pass.source.format), destination.AttachmentView(pass.destination.format)};
        Framebuffer framebuffer(context, renderPass, views, pass.source.extent);
        CommandBatch batch(context);
        const auto commands = batch.Handle();
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass = renderPass;
        begin.framebuffer = framebuffer.Handle();
        begin.renderArea = pass.region;
        context.Function<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass")(commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
        context.Function<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass")(commands);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        batch.SubmitAndWait();
    } catch (...) {
        if (renderPass) context.Function<PFN_vkDestroyRenderPass>("vkDestroyRenderPass")(context.device, renderPass, nullptr);
        throw;
    }
    context.Function<PFN_vkDestroyRenderPass>("vkDestroyRenderPass")(context.device, renderPass, nullptr);
}

}

void ResolveColor(const Context& context, const ColorResolvePass& pass) {
    GuestMemory::AssertGpuLockHeld("ResolveColor");
    ValidateTarget(pass.source);
    ValidateTarget(pass.destination);
    Require((pass.source.samples == VK_SAMPLE_COUNT_2_BIT || pass.source.samples == VK_SAMPLE_COUNT_4_BIT || pass.source.samples == VK_SAMPLE_COUNT_8_BIT) && pass.destination.samples == VK_SAMPLE_COUNT_1_BIT, "color resolve requires a multisampled source and one destination sample");
    Require(pass.source.format == pass.destination.format, "color resolve source and destination formats differ");
    Require(pass.source.extent.width == pass.destination.extent.width && pass.source.extent.height == pass.destination.extent.height, "color resolve source and destination extents differ");
    const MultisampleColorLayout sourceLayout(pass.source.extent.width, pass.source.extent.height, pass.source.elementBytes, static_cast<std::uint32_t>(pass.source.samples), pass.source.pipeBankXor);
    const ColorTargetLayout destinationLayout(pass.destination.extent.width, pass.destination.extent.height, pass.destination.tileMode, pass.destination.elementBytes, pass.destination.pipeBankXor);
    Require(pass.source.bytes == sourceLayout.Bytes() && pass.destination.bytes == destinationLayout.Bytes(), "color resolve guest byte ranges disagree with their layouts");
    Require(pass.source.address + pass.source.bytes <= pass.destination.address || pass.destination.address + pass.destination.bytes <= pass.source.address, "color resolve source and destination guest ranges overlap");
    Require(pass.region.offset.x >= 0 && pass.region.offset.y >= 0, "color resolve region has a negative offset");
    const auto right = static_cast<std::uint64_t>(pass.region.offset.x) + pass.region.extent.width;
    const auto bottom = static_cast<std::uint64_t>(pass.region.offset.y) + pass.region.extent.height;
    Require(right <= pass.source.extent.width && bottom <= pass.source.extent.height, "color resolve region exceeds its image extent");
    GuestMemory::CheckRange(reinterpret_cast<const void*>(pass.source.address), pass.source.bytes, sourceLayout.Alignment(), true);
    GuestMemory::CheckRange(reinterpret_cast<const void*>(pass.destination.address), pass.destination.bytes, destinationLayout.Alignment(), true);
    if (pass.region.extent.width == 0u || pass.region.extent.height == 0u) return;

    const bool attachmentResolve = pass.source.format == VK_FORMAT_R8G8B8A8_SRGB || pass.source.format == VK_FORMAT_B8G8R8A8_SRGB || pass.source.format == VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    VkFormatProperties properties{};
    context.formatProperties(context.physical, attachmentResolve ? pass.source.format :
        pass.source.elementBytes == 8u ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM, &properties);
    const auto required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
        (attachmentResolve ? 0u : VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
    Require((properties.optimalTilingFeatures & required) == required, "color resolve format lacks required transfer or attachment features");
    auto* recorder = Recorder::Active();
    // Transfer and dynamic attachment resolves follow their draws in the same batch. Legacy
    // attachment resolves retain their temporary render pass and framebuffer until completion.
    static const bool syncResolves = std::getenv("APS5_SYNC_COLOR_RESOLVES") != nullptr;
    const bool recorded = recorder != nullptr && (!attachmentResolve || context.graphicsPipelineLibrary) && !syncResolves;
    const auto touches = [&](std::uint64_t begin, std::uint64_t end) {
        return (begin < pass.source.address + pass.source.bytes && pass.source.address < end) ||
            (begin < pass.destination.address + pass.destination.bytes && pass.destination.address < end);
    };
    // Refresh may read guest backing on the CPU. Complete overlapping buffer/label stores first;
    // image-only draws stay on the GPU and are ordered by the resolve's image barriers.
    if (recorder != nullptr && (!recorded || Recorder::SnapshotWriteOverlaps(pass.source.address, pass.source.bytes) ||
        Recorder::SnapshotWriteOverlaps(pass.destination.address, pass.destination.bytes) ||
        recorder->PendingLabelIn(pass.source.address, pass.source.bytes) || recorder->PendingLabelIn(pass.destination.address, pass.destination.bytes) ||
        (recorder->HasQueuedKeyStores() && recorder->AnyQueuedKeyStore(touches)) ||
        (recorder->HasQueuedStores() && recorder->AnyQueuedStore(touches)))) recorder->Sync();
    const auto source = CachedMultisampleColorSurface(context, pass.source);
    const auto destination = CachedStorageSurface(context, DestinationSurface(pass.destination));
    Require(destination->Attachable() && destination->GuestBytes() == pass.destination.bytes, "color resolve destination storage image is incompatible");
    Require(source->Image() != destination->Image(), "color resolve native images alias");

    if (attachmentResolve && !recorded) {
        ResolveAttachment(context, pass, *source, *destination);
        destination->MarkDirty();
        return;
    }
    if (!attachmentResolve) Require(destination->StorageFormat() == source->NativeFormat(), "color resolve native image formats differ");
    std::unique_ptr<CommandBatch> batch;
    if (!recorded) batch = std::make_unique<CommandBatch>(context);
    const auto commands = recorded ? recorder->Commands() : batch->Handle();
    const VkPipelineStageFlags stage = attachmentResolve ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkAccessFlags sourceAccess = attachmentResolve ? VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT;
    const VkAccessFlags destinationAccess = attachmentResolve ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;
    const VkImageMemoryBarrier before[]{
        Barrier(source->Image(), VK_ACCESS_MEMORY_WRITE_BIT, sourceAccess),
        Barrier(destination->Image(), VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, destinationAccess)
    };
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, stage, 0u, 0u, nullptr, 0u, nullptr, 2u, before);
    if (attachmentResolve) {
        VkRenderingAttachmentInfoKHR color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
        color.imageView = source->AttachmentView(pass.source.format);
        color.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        color.resolveImageView = destination->AttachmentView(pass.destination.format);
        color.resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfoKHR rendering{VK_STRUCTURE_TYPE_RENDERING_INFO_KHR};
        rendering.renderArea = pass.region;
        rendering.layerCount = 1u;
        rendering.colorAttachmentCount = 1u;
        rendering.pColorAttachments = &color;
        context.Resolved(&DeviceFunctions::cmdBeginRendering, "vkCmdBeginRenderingKHR")(commands, &rendering);
        context.Resolved(&DeviceFunctions::cmdEndRendering, "vkCmdEndRenderingKHR")(commands);
    } else {
        VkImageResolve region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
        region.srcOffset = {pass.region.offset.x, pass.region.offset.y, 0};
        region.dstOffset = region.srcOffset;
        region.extent = {pass.region.extent.width, pass.region.extent.height, 1u};
        context.Function<PFN_vkCmdResolveImage>("vkCmdResolveImage")(commands, source->Image(), VK_IMAGE_LAYOUT_GENERAL, destination->Image(), VK_IMAGE_LAYOUT_GENERAL, 1u, &region);
    }
    const VkImageMemoryBarrier after[]{
        Barrier(source->Image(), sourceAccess, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT),
        Barrier(destination->Image(), destinationAccess, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT)
    };
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, stage, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u, 0u, nullptr, 0u, nullptr, 2u, after);
    if (recorded) {
        recorder->Keep(source);
        recorder->Keep(destination);
    } else batch->SubmitAndWait();
    destination->MarkDirty();
}

}
