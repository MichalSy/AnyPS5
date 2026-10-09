#include "SpecializedDiskCache.hpp"
#include "CacheKey.hpp"
#include "ShaderCacheDirectory.hpp"
#include "ShaderDiskCache.hpp"
#include "SpirvBackend/SpirvOptimizer.hpp"
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>

namespace ShaderRecompiler::SpecializedDiskCache {

namespace {

constexpr std::uint32_t FileMagic = 0x4d535041u;
constexpr std::size_t HeaderBytes = 48;
std::atomic<std::uint64_t> hits{0}, misses{0}, rejected{0}, writes{0};

class Writer {
public:
    explicit Writer(std::vector<std::byte>& bytes) : bytes(bytes) {}
    void U32(std::uint32_t value) { for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<std::byte>(value >> shift)); }
    void U64(std::uint64_t value) { for (unsigned shift = 0; shift < 64; shift += 8) bytes.push_back(static_cast<std::byte>(value >> shift)); }
    void Words(std::span<const std::uint32_t> values) { U64(values.size()); for (const auto value : values) U32(value); }
    void Text(const char* value) {
        U32(value != nullptr);
        const auto size = value == nullptr ? 0u : std::strlen(value);
        U64(size);
        if (size != 0) bytes.insert(bytes.end(), reinterpret_cast<const std::byte*>(value), reinterpret_cast<const std::byte*>(value) + size);
    }
private:
    std::vector<std::byte>& bytes;
};

class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) : bytes(bytes) {}
    std::uint32_t U32() { return static_cast<std::uint32_t>(Value(4)); }
    std::uint64_t U64() { return Value(8); }
    bool Ok() const { return ok; }
private:
    std::uint64_t Value(std::size_t count) {
        if (!ok || count > bytes.size() - position) { ok = false; return 0; }
        std::uint64_t value = 0;
        for (std::size_t index = 0; index < count; ++index) value |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(bytes[position++])) << (index * 8u);
        return value;
    }
    std::span<const std::byte> bytes;
    std::size_t position = 0;
    bool ok = true;
};

std::string Hex(std::uint64_t value) {
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
    return text;
}

}

bool Enabled() {
    const char* disabled = std::getenv("ANYPS5_NO_SPECIALIZED_SHADER_CACHE");
    return !(disabled != nullptr && *disabled != '\0' && std::strcmp(disabled, "0") != 0) && !DebugProbeActive() && !ShaderCacheDirectory().empty();
}

std::filesystem::path EntryDirectory() {
    const auto root = ShaderCacheDirectory();
    return root.empty() ? root : root / Hex(ShaderDiskCache::SourceVersion()) / "specialized";
}

std::string EntryName(std::span<const std::byte> key) {
    return Hex(HashBytes(key, 0x5eed1001ull)) + Hex(HashBytes(key, 0x5eed1002ull)) + ".bin";
}

void BuildKey(const CompiledShaderArtifact& artifact, std::span<const std::uint32_t> source, std::span<const std::uint32_t> classes, std::span<const PipelineSpecializationConstant> constants, const SpirvTarget& target, std::vector<std::byte>& key) {
    key.clear();
    Writer writer(key);
    writer.U32(FileMagic);
    writer.U32(FormatVersion);
    writer.U64(ShaderDiskCache::SourceVersion());
    writer.U32(artifact.runtimeAbiVersion);
    writer.U32(artifact.bdaAbiVersion);
    RecompileRequest request{};
    request.shader.stage = ShaderStage::Compute;
    request.target = target;
    std::vector<std::uint64_t> targetKey;
    RecompileCacheKey::BuildInterface(request, targetKey);
    writer.U64(targetKey.size());
    for (const auto value : targetKey) writer.U64(value);
    writer.Words(source);
    writer.Words(classes);
    writer.U64(constants.size());
    std::set<std::uint32_t> ids;
    for (const auto& constant : constants) {
        if (!ids.insert(constant.id).second) throw std::runtime_error("duplicate prepared specialization ID");
        writer.U32(constant.id);
        writer.U32(constant.value);
    }
    writer.Text(std::getenv("APS5_SPIRV_OPT"));
}

Module ReadInterface(std::vector<std::uint32_t> words) {
    if (words.size() < 5 || words[0] != spv::MagicNumber || words[3] == 0 || words[4] != 0) throw std::runtime_error("invalid specialized module header");
    std::map<std::uint32_t, std::uint32_t> bindingNumbers;
    for (std::size_t cursor = 5; cursor < words.size();) {
        const auto count = words[cursor] >> 16u;
        const auto op = static_cast<spv::Op>(words[cursor] & 0xffffu);
        if (count == 0 || count > words.size() - cursor) throw std::runtime_error("truncated specialized module instruction");
        if (op == spv::OpDecorate && count == 4 && words[cursor + 2] == spv::DecorationSpecId) throw std::runtime_error("specialized module retains a specialization ID");
        if (op == spv::OpDecorate && count == 4 && words[cursor + 2] == spv::DecorationBinding) bindingNumbers.emplace(words[cursor + 1], words[cursor + 3]);
        cursor += count;
    }
    Module result;
    for (std::size_t cursor = 5; cursor < words.size();) {
        const auto count = words[cursor] >> 16u;
        const auto op = static_cast<spv::Op>(words[cursor] & 0xffffu);
        if (op == spv::OpVariable) {
            if (count < 4) throw std::runtime_error("truncated specialized module variable");
            if (const auto found = bindingNumbers.find(words[cursor + 2]); found != bindingNumbers.end()) result.bindings.push_back(found->second);
            result.pushData |= words[cursor + 3] == spv::StorageClassPushConstant;
        }
        cursor += count;
    }
    result.spirv = std::move(words);
    return result;
}

bool Load(std::span<const std::byte> key, const SpirvTarget& target, Module& module) {
    if (!Enabled() || key.size() > MaxEntryBytes - HeaderBytes) return false;
    std::ifstream file(EntryDirectory() / EntryName(key), std::ios::binary | std::ios::ate);
    if (!file) { misses.fetch_add(1, std::memory_order_relaxed); return false; }
    const auto fail = [] { rejected.fetch_add(1, std::memory_order_relaxed); return false; };
    const auto size = static_cast<std::streamoff>(file.tellg());
    if (size < static_cast<std::streamoff>(HeaderBytes) || size > static_cast<std::streamoff>(MaxEntryBytes)) return fail();
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) return fail();
    Reader header(std::span<const std::byte>(bytes).first(HeaderBytes));
    const auto magic = header.U32();
    const auto format = header.U32();
    const auto version = header.U64();
    const auto keyBytes = header.U64();
    const auto payloadBytes = header.U64();
    const auto keyHash = header.U64();
    const auto payloadHash = header.U64();
    if (!header.Ok() || magic != FileMagic || format != FormatVersion || version != ShaderDiskCache::SourceVersion() || keyBytes != key.size() || keyBytes > bytes.size() - HeaderBytes || payloadBytes != bytes.size() - HeaderBytes - keyBytes) return fail();
    const auto storedKey = std::span<const std::byte>(bytes).subspan(HeaderBytes, static_cast<std::size_t>(keyBytes));
    const auto payload = std::span<const std::byte>(bytes).subspan(HeaderBytes + static_cast<std::size_t>(keyBytes));
    if (HashBytes(storedKey) != keyHash || !std::ranges::equal(storedKey, key) || HashBytes(payload) != payloadHash || payload.size() < 8) return fail();
    Reader reader(payload);
    const auto wordCount = reader.U64();
    if (wordCount < 5 || wordCount > (payload.size() - 8) / 4 || wordCount * 4 != payload.size() - 8) return fail();
    std::vector<std::uint32_t> words(static_cast<std::size_t>(wordCount));
    for (auto& word : words) word = reader.U32();
    if (!reader.Ok() || words[1] > target.spirvVersion) return fail();
    try {
#if ANYPS5_ENABLE_SPIRV_TOOLS
        words = ValidateAndOptimizeSpirv(words, target.vulkanVersion, target.spirvVersion, target.nonConstantImageOffsets, false);
#endif
        auto loaded = ReadInterface(std::move(words));
        module = std::move(loaded);
    } catch (const std::runtime_error&) { return fail(); }
    hits.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void Store(std::span<const std::byte> key, std::span<const std::uint32_t> words) {
    if (!Enabled() || key.size() > MaxEntryBytes - HeaderBytes - 8 || words.size() > (MaxEntryBytes - HeaderBytes - 8 - key.size()) / 4) return;
    std::vector<std::byte> payload;
    payload.reserve(8 + words.size() * 4);
    Writer(payload).Words(words);
    std::vector<std::byte> bytes;
    bytes.reserve(HeaderBytes + key.size() + payload.size());
    Writer header(bytes);
    header.U32(FileMagic);
    header.U32(FormatVersion);
    header.U64(ShaderDiskCache::SourceVersion());
    header.U64(key.size());
    header.U64(payload.size());
    header.U64(HashBytes(key));
    header.U64(HashBytes(payload));
    bytes.insert(bytes.end(), key.begin(), key.end());
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    if (WriteFileAtomically(EntryDirectory() / EntryName(key), bytes)) writes.fetch_add(1, std::memory_order_relaxed);
}

Counters Totals() { return {hits.load(std::memory_order_relaxed), misses.load(std::memory_order_relaxed), rejected.load(std::memory_order_relaxed), writes.load(std::memory_order_relaxed)}; }

}
