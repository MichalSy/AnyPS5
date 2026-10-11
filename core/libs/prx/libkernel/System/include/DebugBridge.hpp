#ifndef CORE_LIBS_PRX_LIBKERNEL_SYSTEM_INCLUDE_DEBUGBRIDGE_HPP
#define CORE_LIBS_PRX_LIBKERNEL_SYSTEM_INCLUDE_DEBUGBRIDGE_HPP

#include <cstdint>
#include <exception>

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

enum class DebugBridgeWork : std::uint32_t {
    Preparation, Translation, Specialization, PipelineBuild, PipelineOptimization,
    ShaderModule, GpuPresentWait, GpuAcquireWait, GpuIdleWait, GpuGraphicsWait, GpuSubmit, Count
};

extern "C" std::uint64_t DebugBridgeBeginWork_nid_no_patch(DebugBridgeWork work) noexcept;
extern "C" void DebugBridgeEndWork_nid_no_patch(DebugBridgeWork work, std::uint64_t startedUs, bool success) noexcept;

class DebugBridgeWorkActivity {
public:
    explicit DebugBridgeWorkActivity(DebugBridgeWork work) noexcept : work(work) {
#if ANYPS5_ENABLE_DEBUG_BRIDGE
        exceptions = std::uncaught_exceptions();
        startedUs = DebugBridgeBeginWork_nid_no_patch(work);
#endif
    }
    ~DebugBridgeWorkActivity() {
#if ANYPS5_ENABLE_DEBUG_BRIDGE
        Finish(std::uncaught_exceptions() == exceptions);
#endif
    }
    DebugBridgeWorkActivity(const DebugBridgeWorkActivity&) = delete;
    DebugBridgeWorkActivity& operator=(const DebugBridgeWorkActivity&) = delete;
    void Finish(bool success) noexcept {
#if ANYPS5_ENABLE_DEBUG_BRIDGE
        if (!finished) DebugBridgeEndWork_nid_no_patch(work, startedUs, success);
#else
        static_cast<void>(success);
#endif
        finished = true;
    }
private:
    DebugBridgeWork work;
    std::uint64_t startedUs = 0;
    int exceptions = 0;
    bool finished = false;
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
        : activeMetric(activeMetric), completedMetric(completedMetric), work(Work(activeMetric)) {
        DebugBridgeShaderCounter(activeMetric, 1);
    }

    ~DebugBridgeShaderActivity() {
#if ANYPS5_ENABLE_DEBUG_BRIDGE
        work.Finish(completedMetric == DebugBridgeShaderMetric::Count ? std::uncaught_exceptions() == exceptions : completed);
#endif
        DebugBridgeShaderCounter(activeMetric, -1);
        if (completed && completedMetric != DebugBridgeShaderMetric::Count)
            DebugBridgeShaderCounter(completedMetric, 1);
    }

    DebugBridgeShaderActivity(const DebugBridgeShaderActivity&) = delete;
    DebugBridgeShaderActivity& operator=(const DebugBridgeShaderActivity&) = delete;

    void Complete() noexcept { completed = true; }

private:
    static DebugBridgeWork Work(DebugBridgeShaderMetric metric) noexcept {
        switch (metric) {
        case DebugBridgeShaderMetric::PreparationActive: return DebugBridgeWork::Preparation;
        case DebugBridgeShaderMetric::TranslationsActive: return DebugBridgeWork::Translation;
        case DebugBridgeShaderMetric::PipelineBuildsActive: return DebugBridgeWork::PipelineBuild;
        case DebugBridgeShaderMetric::PipelineOptimizationsActive: return DebugBridgeWork::PipelineOptimization;
        default: return DebugBridgeWork::Count;
        }
    }
    DebugBridgeShaderMetric activeMetric;
    DebugBridgeShaderMetric completedMetric;
    bool completed = false;
#if ANYPS5_ENABLE_DEBUG_BRIDGE
    int exceptions = std::uncaught_exceptions();
#else
    int exceptions = 0;
#endif
    DebugBridgeWorkActivity work;
};

#endif
