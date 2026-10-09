#include "prx/libSceAgcDriver/Execution/include/RegisteredPreparation.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "CompiledVariant.hpp"
#include <algorithm>
#include <cstdlib>
#include <list>
#include <mutex>
#include <string_view>
#include <vector>

namespace AgcDriver::DriverDetail {
namespace {

struct Hints {
    std::mutex mutex;
    std::list<std::weak_ptr<const ShaderRecompiler::SourceHandle>> entries;
};

Hints& CachedHints() {
    static Hints hints;
    return hints;
}

bool SameLayout(const ShaderRecompiler::BindingLayout& left, const ShaderRecompiler::BindingLayout& right) {
    return left.descriptorSet == right.descriptorSet && left.firstBinding == right.firstBinding &&
        left.pushConstantOffsetBytes == right.pushConstantOffsetBytes && left.pushConstantSizeBytes == right.pushConstantSizeBytes;
}

bool Cached(const ShaderRecompiler::RecompileRequest& request) {
    std::vector<std::uint64_t> preparedKey;
    ShaderRecompiler::BuildPreparedShaderKey(request, preparedKey);
    auto& hints = CachedHints();
    {
        std::lock_guard lock(hints.mutex);
        for (auto it = hints.entries.begin(); it != hints.entries.end();) {
            const auto handle = it->lock();
            if (!handle) {
                it = hints.entries.erase(it);
                continue;
            }
            if (handle->artifact && SameLayout(request.layout, handle->artifact->layout) &&
                ShaderRecompiler::MatchesPreparedShader(request, *handle, preparedKey)) {
                hints.entries.splice(hints.entries.begin(), hints.entries, it);
                return true;
            }
            ++it;
        }
    }
    return false;
}

void Remember(const std::array<std::shared_ptr<const ShaderRecompiler::SourceHandle>, 2>& handles) {
    auto& hints = CachedHints();
    std::lock_guard lock(hints.mutex);
    for (const auto& handle : handles) hints.entries.push_front(handle);
    while (hints.entries.size() > 128) hints.entries.pop_back();
}

bool ParallelEnabled() {
    if (std::getenv("APS5_PROBE") != nullptr || ShaderRecompiler::DebugProbeActive()) return false;
    const auto* setting = std::getenv("ANYPS5_PARALLEL_SHADER_PREPARATION");
    return setting == nullptr || std::string_view(setting) != "0";
}

}

std::array<std::shared_ptr<const ShaderRecompiler::SourceHandle>, 2> PrepareComputeTemplates(
    PreparationPool& pool, const ShaderRecompiler::RecompileRequest& request, std::stop_token token) {
    if (request.shader.stage != ShaderRecompiler::ShaderStage::Compute || !request.context.compute ||
        request.context.compute->PartialGroups()) throw std::invalid_argument("full compute template is required");
    auto partial = request;
    partial.context.compute->partialThreads = {1, 1, 1};
    const auto parallel = ParallelEnabled() && !Cached(request) && !Cached(partial);
    auto result = pool.RunPair([&] {
        PerformanceTimer timing("Shader.PrepareArtifact");
        return ShaderRecompiler::PrepareShader(request);
    }, [&] {
        PerformanceContext context(FrameTiming::Preparation());
        PerformanceTimer timing("Shader.PrepareArtifact");
        return ShaderRecompiler::PrepareShader(partial);
    }, parallel, token);
    Remember(result);
    return result;
}

}
