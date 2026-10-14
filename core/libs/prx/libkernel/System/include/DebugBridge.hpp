#ifndef CORE_LIBS_PRX_LIBKERNEL_SYSTEM_INCLUDE_DEBUGBRIDGE_HPP
#define CORE_LIBS_PRX_LIBKERNEL_SYSTEM_INCLUDE_DEBUGBRIDGE_HPP

#include <cstdint>

struct DebugBridgeFrame {
    std::uint64_t frame = 0;
    std::uint32_t outputHandle = 0;
    std::uint64_t outputGeneration = 0;
    std::uint64_t completedFlipCount = 0;
    std::uint64_t timestampUs = 0;
};

extern "C" void DebugBridgeStart_nid_no_patch() noexcept;
extern "C" void DebugBridgeStop_nid_no_patch() noexcept;
extern "C" void DebugBridgePublishFrame_nid_no_patch(const DebugBridgeFrame& frame) noexcept;

#endif
