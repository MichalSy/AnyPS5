#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_MULTISAMPLECOLORSURFACE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_MULTISAMPLECOLORSURFACE_HPP

#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleColorLayout.hpp"
#include <memory>
#include <vector>

namespace AgcDriver::Graphics {

class MultisampleColorSurface {
public:
    MultisampleColorSurface(const Context& context, const ColorTarget& target);
    ~MultisampleColorSurface();
    MultisampleColorSurface(const MultisampleColorSurface&) = delete;
    MultisampleColorSurface& operator=(const MultisampleColorSurface&) = delete;

    VkImage Image() const { return image; }
    VkImageView AttachmentView() const { return attachmentView; }
    const ColorTarget& Target() const { return target; }
    void Refresh();
    void MarkDirty();
    bool Flush();
    bool Dirty() const { return dirty; }

private:
    void release() noexcept;
    void transfer(bool store);
    void upload();
    Context context;
    ColorTarget target;
    MultisampleColorLayout geometry;
    std::vector<std::byte> snapshot;
    std::unique_ptr<Buffer> linear;
    VkImage image = VK_NULL_HANDLE;
    VkImageView storageView = VK_NULL_HANDLE;
    VkImageView attachmentView = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    std::uint64_t generation = 0;
    bool initialized = false;
    bool dirty = false;
    bool needsUpload = false;
};

std::shared_ptr<MultisampleColorSurface> CachedMultisampleColorSurface(const Context& context, const ColorTarget& target);
bool AnyPendingMultisampleColors(std::uint64_t address, std::size_t bytes);
bool FlushMultisampleColors(std::uint64_t address, std::size_t bytes);
void ClearMultisampleColors(VkDevice device);

}

#endif
