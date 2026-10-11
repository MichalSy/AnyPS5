#include "tests/DebugBridgeTestPeer.hpp"
#include "Recompiler.hpp"
#include "CompiledVariant.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <array>

static JsonValue Shaders(BridgeCollector& collector) {
    const auto event = Await(collector, "event", {}, "resource_status");
    return Member(Member(event, "data", JsonType::Object), "shaders", JsonType::Object);
}

int main() {
    try {
        using namespace ShaderRecompiler;
        BridgeCollector collector;
        UnsetEnvironment("APS5_DEBUG_BRIDGE");
        SetEnvironment("FRAMERIFT_BRIDGE_HOST", "127.0.0.1");
        SetEnvironment("FRAMERIFT_BRIDGE_PORT", std::to_string(collector.Port()));
        SetEnvironment("FRAMERIFT_BRIDGE_SESSION", "framerift-debug-bridge-test");
        SetEnvironment("FRAMERIFT_GAME_ID", "PPSA90000");
        SetEnvironment("ANYPS5_NO_SHADER_CACHE", "1");
        constexpr std::array<std::uint32_t, 3> code{0xf80008cfu, 0u, 0xbf810000u};
        RecompileRequest request{};
        request.shader = {ShaderStage::Vertex, 0x10000u, code, 0, {}};
        request.context.waveSize = 64;
        request.context.vertex = ShaderVertexStageInfo{};
        request.target.vulkanVersion = 0x00401000u;
        request.target.spirvVersion = 0x00010300u;
        request.target.subgroupSize = 64;
        request.layout.pushConstantSizeBytes = 128;
        const auto first = PrepareShader(request);
        Require(first && first->artifact && !first->artifact->artifact.spirv.empty(), "Actual shader translation produced no variant");
        DebugBridgeStart_nid_no_patch();
        BridgePacket hello;
        Require(collector.Receive(std::chrono::milliseconds(1000), hello), "Shader bridge did not start");
        NoEvents(collector, std::chrono::milliseconds(120));
        collector.Send(hello.source, Command(1, "shader-enable", R"({"enabled":true})"));
        Await(collector, "command_completed", "shader-enable");
        auto shaders = Shaders(collector);
        const auto payload = first->artifact->artifact.spirv.size() * sizeof(std::uint32_t);
        Require(Unsigned(shaders, "translations_active") == 0 && Unsigned(shaders, "translations_completed") == 2,
            "Actual front-end and SPIR-V emission scopes were not counted before activation");
        Require(Unsigned(shaders, "cached_variants_count") == 1 && Unsigned(shaders, "source_code_bytes") == sizeof(code)
            && Unsigned(shaders, "variant_spirv_bytes") == payload, "Actual shader cache payload accounting differs from retained data");
        collector.Send(hello.source, Command(2, "shader-disable", R"({"enabled":false})"));
        Await(collector, "command_completed", "shader-disable");
        const auto second = PrepareShader(request);
        Require(second->artifact == first->artifact, "Identical request did not reuse its cached variant");
        constexpr std::array<std::uint32_t, 1> invalid{0xffffffffu};
        auto rejected = request;
        rejected.shader.code = invalid;
        bool failed = false;
        try {
            static_cast<void>(PrepareShader(rejected));
        } catch (const std::exception&) {
            failed = true;
        }
        Require(failed, "Invalid RDNA source did not exercise an actual translation failure");
        NoEvents(collector, std::chrono::milliseconds(120));
        collector.Send(hello.source, Command(3, "shader-reenable", R"({"enabled":true})"));
        Await(collector, "command_completed", "shader-reenable");
        shaders = Shaders(collector);
        Require(Unsigned(shaders, "translations_active") == 0 && Unsigned(shaders, "translations_completed") == 2,
            "Cache reuse fabricated translation or failed compilation retained an active scope");
        Require(Unsigned(shaders, "cached_variants_count") == 1 && Unsigned(shaders, "variant_spirv_bytes") == payload
            && Unsigned(shaders, "source_code_bytes") == sizeof(code) + sizeof(invalid), "Rejected cached source changed retained variant accounting");
        Require(Unsigned(shaders, "disk_cache_hits") == 0 && Unsigned(shaders, "disk_cache_misses") == 0,
            "Disabled disk cache fabricated lookup counters");
        DebugBridgeStop_nid_no_patch();
        return 0;
    } catch (const std::exception& error) {
        DebugBridgeStop_nid_no_patch();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
