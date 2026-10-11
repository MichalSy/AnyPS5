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

enum class DebugBridgeShaderMetric : std::uint32_t {
    TranslationsActive,
    TranslationsCompleted,
    CachedVariantsCount,
    SourceCodeBytes,
    VariantSpirvBytes,
    DiskCacheHits,
    DiskCacheMisses,
    DiskCacheWrites,
    DiskCacheLoadFailures,
    DiskCacheWriteFailures,
    PreparationQueuePending,
    PreparationActive,
    PipelineBuildsActive,
    PipelineBuildsCompleted,
    PipelineOptimizationQueuePending,
    PipelineOptimizationsActive,
    Count
};

extern "C" void DebugBridgeStart_nid_no_patch() noexcept;
extern "C" void DebugBridgeStop_nid_no_patch() noexcept;
extern "C" void DebugBridgePublishFrame_nid_no_patch(const DebugBridgeFrame& frame) noexcept;
extern "C" void DebugBridgeAddShaderMetric_nid_no_patch(DebugBridgeShaderMetric metric, std::int64_t delta) noexcept;
extern "C" void DebugBridgePublishSampledCache_nid_no_patch(std::uint64_t bytes, std::uint64_t budgetBytes) noexcept;
extern "C" void DebugBridgePublishStorageCache_nid_no_patch(std::uint64_t bytes, std::uint64_t budgetBytes) noexcept;
extern "C" void DebugBridgePublishGpuHeap_nid_no_patch(std::uint64_t usage, std::uint64_t budget, std::uint64_t timestampUs) noexcept;

inline void DebugBridgeShaderCounter(DebugBridgeShaderMetric metric, std::int64_t delta) noexcept {
#if ANYPS5_ENABLE_DEBUG_BRIDGE
    DebugBridgeAddShaderMetric_nid_no_patch(metric, delta);
#else
    static_cast<void>(metric);
    static_cast<void>(delta);
#endif
}

class DebugBridgeShaderActivity {
public:
    explicit DebugBridgeShaderActivity(DebugBridgeShaderMetric activeMetric,
        DebugBridgeShaderMetric completedMetric = DebugBridgeShaderMetric::Count) noexcept
        : activeMetric(activeMetric), completedMetric(completedMetric) {
        DebugBridgeShaderCounter(activeMetric, 1);
    }

    ~DebugBridgeShaderActivity() {
        DebugBridgeShaderCounter(activeMetric, -1);
        if (completed && completedMetric != DebugBridgeShaderMetric::Count)
            DebugBridgeShaderCounter(completedMetric, 1);
    }

    DebugBridgeShaderActivity(const DebugBridgeShaderActivity&) = delete;
    DebugBridgeShaderActivity& operator=(const DebugBridgeShaderActivity&) = delete;

    void Complete() noexcept { completed = true; }

private:
    DebugBridgeShaderMetric activeMetric;
    DebugBridgeShaderMetric completedMetric;
    bool completed = false;
};

#endif
