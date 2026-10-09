#ifndef CORE_SHADER_RECOMPILER_SPECIALIZEDDISKCACHE_HPP
#define CORE_SHADER_RECOMPILER_SPECIALIZEDDISKCACHE_HPP

#include "Recompiler.hpp"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace ShaderRecompiler::SpecializedDiskCache {

inline constexpr std::uint32_t FormatVersion = 1;
inline constexpr std::size_t MaxEntryBytes = 128u * 1024u * 1024u;

struct Module {
    SharedSpirv spirv;
    std::vector<std::uint32_t> bindings;
    bool pushData = false;
};

struct Counters {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t rejected = 0;
    std::uint64_t writes = 0;
};

[[nodiscard]] bool Enabled();
[[nodiscard]] std::filesystem::path EntryDirectory();
[[nodiscard]] std::string EntryName(std::span<const std::byte> key);
void BuildKey(const CompiledShaderArtifact& artifact, std::span<const std::uint32_t> source, std::span<const std::uint32_t> classes, std::span<const PipelineSpecializationConstant> constants, const SpirvTarget& target, std::vector<std::byte>& key);
[[nodiscard]] Module ReadInterface(std::vector<std::uint32_t> words);
[[nodiscard]] bool Load(std::span<const std::byte> key, const SpirvTarget& target, Module& module);
void Store(std::span<const std::byte> key, std::span<const std::uint32_t> words);
[[nodiscard]] Counters Totals();

}

#endif
