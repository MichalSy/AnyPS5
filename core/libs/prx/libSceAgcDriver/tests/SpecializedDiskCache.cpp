#include "SpecializedDiskCache.hpp"
#include "ShaderDiskCache.hpp"
#include "ShaderCacheDirectory.hpp"
#include "RuntimeAbi.hpp"
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace ShaderRecompiler;

void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

void Environment(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

std::string Quote(std::string_view text) {
#ifdef _WIN32
    Require(text.find('"') == std::string_view::npos && text.find('%') == std::string_view::npos, "unsupported test executable path");
    return '"' + std::string(text) + '"';
#else
    std::string result = "'";
    for (const auto character : text) result += character == '\'' ? "'\\''" : std::string(1, character);
    return result + '\'';
#endif
}

struct Compute {
    std::vector<std::uint32_t> code{0x7e020281u, 0xe0700000u, 0x80000100u, 0xbf810000u};
    std::array<std::uint32_t, 4> userData{0x10000000u, 0x00040000u, 0x40u, 0x00027facu};
    std::array<std::uint32_t, 4> capabilities{1u, 11u, 5347u, 4448u};
    std::array<std::string_view, 2> extensions{"SPV_KHR_physical_storage_buffer", "SPV_KHR_8bit_storage"};
    RecompileRequest request{};
    explicit Compute(bool cached) {
        request.shader = {ShaderStage::Compute, 0x20000u, code, 0, {}};
        request.context.waveSize = 64;
        request.context.userData = userData;
        request.context.compute = ShaderComputeStageInfo{{64u, 1u, 1u}, 0u, {false, false, false}, false, 1u};
        request.target.vulkanVersion = 0x00401000u;
        request.target.spirvVersion = 0x00010300u;
        request.target.subgroupSize = 32;
        request.target.bdaAbiVersion = 1;
        request.target.supportedCapabilities = capabilities;
        request.target.supportedExtensions = extensions;
        request.layout.pushConstantSizeBytes = 128;
        request.useCache = cached;
    }
};

void SpilledImageInterface() {
    const std::array<std::uint32_t, 13> code{
        0x34060082u, 0x7e020280u, 0x7e040281u, 0xd8340000u, 0x00000203u,
        0xbf8c0000u, 0xbf8a0000u, 0xd8d80000u, 0x02000003u, 0xbf8c0000u,
        0xf0201108u, 0x00000200u, 0xbf810000u
    };
    const std::array<std::uint32_t, 8> image{
        0x00100000u, (static_cast<std::uint32_t>(IrBufferFormat::Format32UInt) << 20u) | (3u << 30u),
        7u, 0x90000facu, 0u, 0u, 0u, 0u
    };
    Compute fixture(false);
    fixture.request.shader = {ShaderStage::Compute, 0x24000u, code, 0, {}};
    fixture.request.context.waveSize = 32u;
    fixture.request.context.userData = image;
    fixture.request.context.compute = ShaderComputeStageInfo{{32u, 1u, 1u}, 1024u, {false, false, false}, false, 1u};
    fixture.request.target.maxWorkgroupSharedMemoryBytes = 2048u;
    const auto compiled = Recompile(fixture.request);
    Require(compiled.workgroupMemoryDwords == 1025u, "image-only LDS fixture did not spill");
    Require(compiled.specializationId != 0u, "image-only LDS fixture was not specialized");
    Require(std::none_of(compiled.bindings.begin(), compiled.bindings.end(), [](const auto& binding) { return binding.role == DescriptorRole::GuestBuffers; }), "image-only LDS fixture acquired guest buffers");
    Require(std::any_of(compiled.bindings.begin(), compiled.bindings.end(), [](const auto& binding) { return binding.kind == DescriptorKind::StorageImage; }), "image-only LDS fixture lost its image");
    const auto interface = SpecializedDiskCache::ReadInterface(compiled.spirv.Words());
    Require(std::find(interface.bindings.begin(), interface.bindings.end(), static_cast<std::uint32_t>(RuntimeAbi::Binding::Buffers)) == interface.bindings.end(), "workgroup memory was mistaken for guest buffers");
    for (const auto& binding : compiled.bindings) Require(std::find(interface.bindings.begin(), interface.bindings.end(), binding.binding) != interface.bindings.end(), "image-only LDS interface omitted a runtime binding");
}

void SameInvocation(const RecompileResult& left, const RecompileResult& right) {
    Require(left.spirv.Words() == right.spirv.Words(), "loaded specialized SPIR-V differs from uncached compilation");
    Require(left.pushConstants == right.pushConstants, "loaded module retained stale push data");
    Require(left.bindings.size() == right.bindings.size(), "loaded module selected a different binding interface");
    for (std::size_t index = 0; index < left.bindings.size(); ++index) {
        const auto& a = left.bindings[index];
        const auto& b = right.bindings[index];
        Require(a.kind == b.kind && a.role == b.role && a.descriptorSet == b.descriptorSet && a.binding == b.binding && a.count == b.count && a.guestDescriptor == b.guestDescriptor && a.readOnly == b.readOnly && a.bufferWritten == b.bufferWritten && a.bufferAtomic == b.bufferAtomic, "loaded module retained stale descriptor data");
    }
}

void NoDiskActivity(const SpecializedDiskCache::Counters& before) {
    const auto after = SpecializedDiskCache::Totals();
    Require(after.hits == before.hits && after.misses == before.misses && after.rejected == before.rejected && after.writes == before.writes, "disabled request accessed the specialized disk cache");
}

int Child(std::string_view mode, const std::filesystem::path& root) {
    Environment("ANYPS5_SHADER_CACHE_DIR", (root / "cache").string());
    Environment("ANYPS5_NO_SHADER_CACHE", mode == "--disabled" ? "1" : "0");
    Environment("ANYPS5_NO_SPECIALIZED_SHADER_CACHE", mode == "--specialized-disabled" ? "1" : "0");
    if (mode == "--disabled" || mode == "--specialized-disabled") {
        Compute fixture(true);
        static_cast<void>(Recompile(fixture.request));
        NoDiskActivity({});
        if (mode == "--disabled") Require(ShaderDiskCache::Totals().hits == 0 && ShaderDiskCache::Totals().writes == 0, "global cache switch did not disable generic caching");
        else Require(ShaderDiskCache::Totals().hits == 1, "specialized cache switch invalidated the generic entry");
        return 0;
    }
    Compute prime(false);
    const std::array<std::uint32_t, 1> end{0xbf810000u};
    prime.request.shader.code = end;
    static_cast<void>(Recompile(prime.request));
    Compute fixture(true);
    fixture.userData[0] += 0x1000u;
    const auto loaded = Recompile(fixture.request);
    const auto counters = SpecializedDiskCache::Totals();
    if (mode == "--load") Require(counters.hits == 1 && counters.writes == 0 && counters.rejected == 0, "second process did not load the specialized module");
    else Require(counters.hits == 0 && counters.rejected == 1 && counters.writes == 1, "corrupt specialized entry did not rebuild and replace the entry");
    Require(ShaderDiskCache::Totals().hits == 1, "second process did not load the generic artifact");
    std::uint64_t parentId = 0;
    std::ifstream(root / "parent-id") >> parentId;
    Require(loaded.specializationId != 0 && loaded.specializationId != parentId && loaded.specializationId > loaded.variantId, "loaded module reused a persisted specialization identity");
    Compute fresh(false);
    fresh.userData[0] = fixture.userData[0];
    const auto before = SpecializedDiskCache::Totals();
    SameInvocation(loaded, Recompile(fresh.request));
    NoDiskActivity(before);
    fixture.userData[0] += 0x1000u;
    fresh.userData[0] += 0x1000u;
    const auto changed = Recompile(fixture.request);
    SameInvocation(changed, Recompile(fresh.request));
    bool descriptorsChanged = changed.bindings.size() != loaded.bindings.size();
    for (std::size_t index = 0; index < changed.bindings.size() && index < loaded.bindings.size(); ++index) descriptorsChanged |= changed.bindings[index].guestDescriptor != loaded.bindings[index].guestDescriptor;
    Require(changed.pushConstants != loaded.pushConstants || descriptorsChanged, "changing a buffer address did not refresh invocation data");
    const auto probeBefore = SpecializedDiskCache::Totals();
    SetDebugProbeActive(true);
    try { static_cast<void>(Recompile(fixture.request)); }
    catch (...) { SetDebugProbeActive(false); throw; }
    SetDebugProbeActive(false);
    NoDiskActivity(probeBefore);
    return 0;
}

void KeyCoverage(const RecompileResult& module, const SpirvTarget& target) {
    std::array<std::uint32_t, 2> classes{0, 1};
    std::array<PipelineSpecializationConstant, 1> constants{{{7, 2}}};
    auto artifact = static_cast<const CompiledShaderArtifact&>(module);
    const auto originalWords = artifact.spirv.Words();
    std::vector<std::byte> baseline;
    SpecializedDiskCache::BuildKey(artifact, originalWords, classes, constants, target, baseline);
    const auto key = [&](const CompiledShaderArtifact& input, std::span<const std::uint32_t> words, std::span<const std::uint32_t> vertexClasses, std::span<const PipelineSpecializationConstant> values, const SpirvTarget& profile) {
        std::vector<std::byte> result;
        SpecializedDiskCache::BuildKey(input, words, vertexClasses, values, profile, result);
        return result;
    };
    artifact.variantId += 900;
    Require(key(artifact, originalWords, classes, constants, target) == baseline, "persistent key depends on a process-local variant ID");
    auto alteredWords = originalWords;
    alteredWords.back() ^= 1;
    Require(key(artifact, alteredWords, classes, constants, target) != baseline, "persistent key omitted source words");
    classes[0] = 2;
    Require(key(artifact, originalWords, classes, constants, target) != baseline, "persistent key omitted vertex classes");
    classes[0] = 0;
    ++constants[0].value;
    Require(key(artifact, originalWords, classes, constants, target) != baseline, "persistent key omitted specialization values");
    --constants[0].value;
    ++constants[0].id;
    Require(key(artifact, originalWords, classes, constants, target) != baseline, "persistent key omitted specialization IDs");
    --constants[0].id;
    const std::array<std::uint32_t, 1> capabilities{9999};
    const std::array<std::string_view, 1> extensions{"SPV_EXT_key_test"};
    const std::array<std::function<void(SpirvTarget&)>, 15> changes{{
        [](auto& profile) { ++profile.vulkanVersion; },
        [](auto& profile) { ++profile.spirvVersion; },
        [](auto& profile) { ++profile.subgroupSize; },
        [](auto& profile) { ++profile.bdaAbiVersion; },
        [&](auto& profile) { profile.supportedCapabilities = capabilities; },
        [&](auto& profile) { profile.supportedExtensions = extensions; },
        [](auto& profile) { profile.fragmentShaderBarycentricEnabled = !profile.fragmentShaderBarycentricEnabled; },
        [](auto& profile) { ++profile.maxWorkgroupSize[0]; },
        [](auto& profile) { ++profile.maxWorkgroupInvocations; },
        [](auto& profile) { ++profile.maxWorkgroupSharedMemoryBytes; },
        [](auto& profile) { profile.mesh.emplace(); },
        [](auto& profile) { profile.tessellation.emplace(); },
        [](auto& profile) { profile.nonConstantImageOffsets = !profile.nonConstantImageOffsets; },
        [](auto& profile) { ++profile.srgbDecodeFormats; },
        [](auto& profile) { profile.narrowSubgroupClock = !profile.narrowSubgroupClock; }
    }};
    for (const auto& change : changes) {
        auto profile = target;
        change(profile);
        Require(key(artifact, originalWords, classes, constants, profile) != baseline, "persistent key omitted a target field");
    }
    ++artifact.runtimeAbiVersion;
    Require(key(artifact, originalWords, classes, constants, target) != baseline, "persistent key omitted the runtime ABI");
    --artifact.runtimeAbiVersion;
    ++artifact.bdaAbiVersion;
    Require(key(artifact, originalWords, classes, constants, target) != baseline, "persistent key omitted the BDA ABI");
}

std::uint64_t U64(std::span<const std::byte> bytes, std::size_t offset) {
    Require(offset <= bytes.size() && bytes.size() - offset >= 8, "truncated test entry integer");
    std::uint64_t result = 0;
    for (unsigned index = 0; index < 8; ++index) result |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(bytes[offset + index])) << (8u * index);
    return result;
}

void PutU64(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value) {
    Require(offset <= bytes.size() && bytes.size() - offset >= 8, "truncated test entry integer");
    for (unsigned index = 0; index < 8; ++index) bytes[offset + index] = static_cast<std::byte>(value >> (8u * index));
}

void RejectedEntries(const std::filesystem::path& path, const std::vector<std::byte>& original, const RecompileResult& expected, const SpirvTarget& target) {
    const auto keyBytes = U64(original, 16);
    Require(keyBytes <= original.size() - 48, "test entry key exceeds the file");
    const auto key = std::span<const std::byte>(original).subspan(48, static_cast<std::size_t>(keyBytes));
    auto unchanged = SpecializedDiskCache::ReadInterface(expected.spirv.Words());
    auto wrongKey = std::vector<std::byte>(key.begin(), key.end());
    wrongKey.back() ^= std::byte{1};
    const auto wrongPath = SpecializedDiskCache::EntryDirectory() / SpecializedDiskCache::EntryName(wrongKey);
    Require(WriteFileAtomically(wrongPath, original), "mismatched-key entry could not be written");
    Require(!SpecializedDiskCache::Load(wrongKey, target, unchanged), "a matching filename bypassed full-key validation");
    std::filesystem::remove(wrongPath);
    auto invalidModule = original;
    const auto payloadStart = 48 + static_cast<std::size_t>(keyBytes);
    Require(invalidModule.size() >= payloadStart + 28, "test module header was truncated");
    for (std::size_t index = 0; index < 4; ++index) invalidModule[payloadStart + 20 + index] = std::byte{0};
    PutU64(invalidModule, 40, HashBytes(std::span<const std::byte>(invalidModule).subspan(payloadStart)));
    Require(WriteFileAtomically(path, invalidModule), "invalid module with valid checksum could not be written");
    Require(!SpecializedDiskCache::Load(key, target, unchanged), "a valid checksum bypassed module validation");
    {
        std::ofstream oversized(path, std::ios::binary | std::ios::trunc);
        oversized.seekp(static_cast<std::streamoff>(SpecializedDiskCache::MaxEntryBytes));
        oversized.put('\0');
        Require(static_cast<bool>(oversized), "oversized entry could not be created");
    }
    Require(!SpecializedDiskCache::Load(key, target, unchanged), "an oversized entry was accepted");
    Require(unchanged.spirv.Words() == expected.spirv.Words(), "rejected entry modified the output module");
    Require(WriteFileAtomically(path, original), "valid specialized entry could not be restored");
}

}

int main(int argc, char** argv) {
    std::filesystem::path root;
    try {
        if (argc == 3) return Child(argv[1], argv[2]);
        Require(argc == 1, "invalid specialized cache test arguments");
        SpilledImageInterface();
        root = std::filesystem::temp_directory_path() / ("anyps5-specialized-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
        Environment("ANYPS5_SHADER_CACHE_DIR", (root / "cache").string());
        Environment("ANYPS5_NO_SHADER_CACHE", "0");
        Environment("ANYPS5_NO_SPECIALIZED_SHADER_CACHE", "0");
        Compute fixture(true);
        const auto compiled = Recompile(fixture.request);
        ShaderDiskCache::Flush();
        Require(SpecializedDiskCache::Totals().misses == 1 && SpecializedDiskCache::Totals().writes == 1, "first process did not store a specialized module");
        Require(compiled.specializationId != 0, "fixture did not exercise module specialization");
        bool observedStore = false;
        for (const auto& binding : compiled.bindings) {
            if (binding.role != DescriptorRole::GuestBuffers) continue;
            for (const bool written : binding.bufferWritten) observedStore |= written;
        }
        Require(observedStore, "fixture did not retain an observable buffer store");
        std::ofstream(root / "parent-id") << compiled.specializationId;
        const auto child = [&](std::string_view mode) {
            const auto command = Quote(std::filesystem::absolute(argv[0]).string()) + ' ' + std::string(mode) + ' ' + Quote(root.string());
            Require(std::system(command.c_str()) == 0, "specialized cache child process failed");
        };
        child("--load");
        child("--disabled");
        child("--specialized-disabled");
        KeyCoverage(compiled, fixture.request.target);
        const auto directory = SpecializedDiskCache::EntryDirectory();
        auto entries = std::filesystem::directory_iterator(directory);
        Require(entries != std::filesystem::directory_iterator{}, "specialized entry was not written");
        const auto path = entries->path();
        std::vector<std::byte> bytes;
        Require(ReadWholeFile(path, bytes) && bytes.size() > 48, "specialized entry could not be read");
        const auto original = bytes;
        bytes.back() ^= std::byte{1};
        Require(WriteFileAtomically(path, bytes), "corrupt specialized entry could not be written");
        child("--fallback");
        bytes = original;
        bytes[8] ^= std::byte{1};
        Require(WriteFileAtomically(path, bytes), "stale specialized entry could not be written");
        child("--fallback");
        Require(WriteFileAtomically(path, std::span<const std::byte>(original).first(49)), "truncated specialized entry could not be written");
        child("--fallback");
        RejectedEntries(path, original, compiled, fixture.request.target);
        std::filesystem::remove_all(root);
        std::cout << "specialized shader disk cache tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        if (!root.empty()) { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
        return 1;
    }
}
