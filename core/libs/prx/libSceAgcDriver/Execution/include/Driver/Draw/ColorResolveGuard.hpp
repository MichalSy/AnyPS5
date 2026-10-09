#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_COLORRESOLVEGUARD_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_COLORRESOLVEGUARD_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include <algorithm>
#include <array>
#include <span>
#include <string>

namespace AgcDriver::DriverDetail {
namespace ColorResolveGuardDetail {

inline constexpr std::array<std::uint32_t, 27> RectangleVertexInstructions{
    0xbfa00001u, 0x93ebff03u, 0x00080008u, 0x8700ff03u, 0x000000ffu,
    0x8f6a8c6bu, 0x887c6a00u, 0xbf900009u, 0x81ea6bc0u, 0x90fe6ac1u,
    0xf8000941u, 0x00000000u, 0x81ea00c0u, 0xbf8cff0fu, 0x90fe6ac1u,
    0x34040a81u, 0x36060ac2u, 0x7e000280u, 0x7e0202f2u, 0x36040482u,
    0x4a0606c1u, 0x4a0404c1u, 0x7e060b03u, 0x7e040b02u,
    0xf80008cfu, 0x01000302u, 0xbf810000u
};
inline constexpr std::array<std::uint32_t, 5> ConstantFragmentInstructions{
    0x7e0002ffu, 0x3c003c00u, 0xf8001c0fu, 0x00000000u, 0xbf810000u
};

inline std::uint32_t Read(const Registers& registers, std::uint32_t offset, Graphics::RegisterBank bank) {
    Graphics::NoteRegisterRead(bank, offset);
    const auto found = registers.find(offset);
    Graphics::Require(found != registers.end(), "fixed color resolve is missing register " + std::to_string(offset));
    return found->second;
}

inline std::span<const std::uint32_t> RegisteredCode(const QueueState& queue, const ShaderRegistry& registry, std::uint32_t addressRegister, std::uint8_t type, std::size_t words) {
    const auto low = Read(queue.shader, addressRegister, Graphics::RegisterBank::Shader);
    const auto high = Read(queue.shader, addressRegister + 1u, Graphics::RegisterBank::Shader);
    Graphics::Require((high & ~0xffu) == 0u, "fixed color resolve has reserved program address bits");
    const auto address = (static_cast<std::uint64_t>(low) << 8u) | (static_cast<std::uint64_t>(high) << 40u);
    auto found = registry.upper_bound(address);
    Graphics::Require(address != 0u && found != registry.begin(), "fixed color resolve program does not belong to a registered shader");
    --found;
    Graphics::Require(found->second != nullptr, "fixed color resolve has a null registered shader");
    const auto& snapshot = *found->second;
    Graphics::Require(found->first == snapshot.codeAddress, "fixed color resolve has inconsistent registered shader identity");
    Graphics::Require(address >= snapshot.codeAddress && (address - snapshot.codeAddress) / sizeof(std::uint32_t) < snapshot.code.size(),
        "fixed color resolve program is outside registered shader code");
    Graphics::Require((address - snapshot.codeAddress) % sizeof(std::uint32_t) == 0u, "fixed color resolve entry point is not dword aligned");
    Graphics::Require(snapshot.type == type, "fixed color resolve program has an incompatible shader binary type");
    const auto offset = static_cast<std::size_t>((address - snapshot.codeAddress) / sizeof(std::uint32_t));
    Graphics::Require(words <= snapshot.code.size() - offset, "fixed color resolve registered program is truncated");
    return std::span(snapshot.code).subspan(offset, words);
}

}

inline void RequireFixedColorResolvePrograms(const QueueState& queue, const ShaderRegistry& registry, const Pm4::DrawParameters& draw) {
    Graphics::Require(!draw.indirect && !draw.indexed && draw.indexCount == 3u && draw.instanceCount == 1u && draw.firstVertex == 0u && draw.firstInstance == 0u,
        "unsupported fixed color resolve draw: count=" + std::to_string(draw.indexCount) + " instances=" + std::to_string(draw.instanceCount) +
        " indexed=" + std::to_string(draw.indexed) + " indirect=" + std::to_string(draw.indirect.has_value()) +
        " firstVertex=" + std::to_string(draw.firstVertex) + " firstInstance=" + std::to_string(draw.firstInstance));
    using namespace ColorResolveGuardDetail;
    const auto context = [&](std::uint32_t offset) { return Read(queue.context, offset, Graphics::RegisterBank::Context); };
    Graphics::Require(Read(queue.userConfig, 0x242u, Graphics::RegisterBank::UserConfig) == 7u,
        "fixed color resolve requires a 2D rectangle primitive");
    Graphics::Require(context(0x29bu) == 3u, "fixed color resolve requires rectangle primitive output");
    Graphics::Require(context(0x2d5u) == 0x02002000u && context(0x2d6u) == 0u && context(0x1b6u) == 0u,
        "fixed color resolve requires the supported wave64 vertex and fragment routing");
    Graphics::Require(Read(queue.shader, 0x8bu, Graphics::RegisterBank::Shader) == 0x00030000u &&
        Read(queue.shader, 0xbu, Graphics::RegisterBank::Shader) == 0u,
        "fixed color resolve requires zero user SGPRs without scratch or additional resources");
    Graphics::Require(Read(queue.shader, 0x8au, Graphics::RegisterBank::Shader) == 0x622c0042u &&
        Read(queue.shader, 0xau, Graphics::RegisterBank::Shader) == 0x022c0000u,
        "fixed color resolve has unsupported shader register or floating-point configuration");
    Graphics::Require(context(0x203u) == 0x10u && context(0x204u) == 0x00080000u && context(0x205u) == 0x240u && context(0x207u) == 0u,
        "fixed color resolve has unsupported depth, shader coverage, clip or raster state");
    Graphics::Require(context(0x1c2u) == 1u && context(0x1c3u) == 4u && context(0x1c4u) == 0u && context(0x1c5u) == 4u,
        "fixed color resolve has unsupported position, depth, coverage or color export format");
    Graphics::Require(context(0x83u) == 0xffffu && context(0x8cu) == 0xaa99aaaau && context(0x8du) == 0u && context(0x2f9u) == 0x2du,
        "fixed color resolve has unsupported clip rectangles, edge rules or vertex quantization");
    Graphics::Require(context(0x2dcu) == 0xaa00u && context(0x30eu) == 0xffffffffu && context(0x30fu) == 0xffffffffu,
        "fixed color resolve has unsupported alpha-to-coverage or sample masks");
    const auto vertex = RegisteredCode(queue, registry, 0xc8u, 2u, RectangleVertexInstructions.size());
    const auto fragment = RegisteredCode(queue, registry, 0x8u, 1u, ConstantFragmentInstructions.size());
    Graphics::Require(std::equal(vertex.begin(), vertex.end(), RectangleVertexInstructions.begin()),
        "fixed color resolve has unsupported vertex instructions or coverage");
    Graphics::Require(std::equal(fragment.begin(), fragment.end(), ConstantFragmentInstructions.begin()),
        "fixed color resolve has unsupported fragment instructions or effects");
}

}

#endif
