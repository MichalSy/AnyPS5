#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DEPTHSURFACE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DEPTHSURFACE_HPP

#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace AgcDriver::Graphics {

class Texture;

VkImageView DepthSurfaceView(const Context& context, const DepthTarget& target);
std::uint64_t DepthSliceBytes(VkExtent2D extent, std::uint32_t bytesPerTexel);
std::uint64_t DepthMipChainBytes(VkExtent2D extent, std::uint32_t bytesPerTexel, std::uint32_t mipCount);
std::uint64_t HtileSliceBytes(VkExtent2D extent);
bool HtileFillCovers(std::uint64_t htile, VkExtent2D extent, std::uint64_t address, std::size_t bytes);
void NoteDepthMetadataFill(VkDevice device, std::uint64_t address, std::size_t bytes, std::uint32_t pattern);
void ClearDepthSurfaces(VkDevice device);
void RetireDepthSurfaces(VkDevice device, std::uint64_t address, std::uint64_t bytes);
bool DepthSurfaceAt(std::uint64_t address);
bool DepthSurfaceHolds(const Context& context, std::span<const std::uint32_t> words, VkComponentMapping components, const Texture* texture);
std::shared_ptr<Texture> DepthSurfaceTexture(const Context& context, std::span<const std::uint32_t> words, const GuestTextureResource& resource, VkComponentMapping components);

}

#endif
