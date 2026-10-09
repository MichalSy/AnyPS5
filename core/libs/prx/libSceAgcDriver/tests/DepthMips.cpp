#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace AgcDriver::Graphics;

constexpr std::uint64_t Base = 0x100000;
constexpr VkExtent2D Extent{512, 256};
constexpr std::uint64_t ChainBytes = 0xb0000;

std::uint32_t View(std::uint32_t mip, std::uint32_t slice) {
    return slice | ((slice & 0x7ffu) << 13u) | (mip << 26u) | ((slice >> 11u) << 30u);
}

AgcDriver::QueueState Queue(std::uint32_t mip, std::uint32_t slice = 0) {
    AgcDriver::QueueState queue;
    queue.userConfig[0x242] = 4;
    queue.context = {
        {0x2d5, 0x2000}, {0x1b6, 0}, {0x207, 0}, {0x200, 0x76}, {0x203, 0x800},
        {0x2dc, 0xaa00}, {0x2f8, 0}, {0x292, 2}, {0x293, 0},
        {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa},
        {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff},
        {0x206, 0x43f}, {0x204, 0x80000}, {0x205, 0x240},
        {0x8e, 0}, {0x8f, 0}, {0x202, 0xcc0010},
        {0x1c4, 0}, {0x1c5, 0}, {0x1c3, 4},
        {0x000, 0}, {0x002, View(mip, slice)},
        {0x007, ((Extent.height - 1u) << 16u) | (Extent.width - 1u)},
        {0x00a, 0}, {0x00b, std::bit_cast<std::uint32_t>(1.0f)},
        {0x010, 3u | (24u << 4u) | (2u << 16u)}, {0x011, 0},
        {0x012, static_cast<std::uint32_t>(Base >> 8u)}, {0x014, static_cast<std::uint32_t>(Base >> 8u)},
        {0x01a, 0}, {0x01c, 0},
        {0xc, 0}, {0x81, 0x80000000}, {0x90, 0x80000000}, {0x94, 0x80000000}
    };
    const auto width = std::max(Extent.width >> mip, 1u);
    const auto height = std::max(Extent.height >> mip, 1u);
    for (const auto offset : {0xdu, 0x82u, 0x91u, 0x95u}) queue.context[offset] = (height << 16u) | width;
    queue.context[0x10f] = queue.context[0x110] = std::bit_cast<std::uint32_t>(static_cast<float>(width) / 2.0f);
    queue.context[0x111] = std::bit_cast<std::uint32_t>(-static_cast<float>(height) / 2.0f);
    queue.context[0x112] = std::bit_cast<std::uint32_t>(static_cast<float>(height) / 2.0f);
    queue.context[0x113] = std::bit_cast<std::uint32_t>(1.0f);
    queue.context[0x114] = queue.context[0xb4] = 0;
    queue.context[0xb5] = std::bit_cast<std::uint32_t>(1.0f);
    queue.shader[0x008] = queue.shader[0x009] = 0;
    return queue;
}

template<typename TAction>
void Reject(TAction action, std::string_view reason) {
    try {
        action();
    } catch (const std::runtime_error& error) {
        Require(std::string_view(error.what()).find(reason) != std::string_view::npos, std::string("unexpected rejection: ") + error.what());
        return;
    }
    throw std::runtime_error("missing rejection: " + std::string(reason));
}

void RegisterTests() {
    for (std::uint32_t mip = 0; mip != 3; ++mip) {
        const auto state = DecodeState(Queue(mip));
        Require(state.depth.has_value(), "the mipmapped depth target was dropped");
        const auto& depth = *state.depth;
        Require(depth.address == Base && depth.surfaceExtent.width == Extent.width && depth.surfaceExtent.height == Extent.height,
            "DB_DEPTH_VIEW changed the full-plane address or basis extent");
        Require(depth.mip == mip && depth.mipCount == 3 && depth.extent.width == (Extent.width >> mip) && depth.extent.height == (Extent.height >> mip),
            "DB_DEPTH_VIEW or DB_Z_INFO did not select the correct mip");
        Require(state.renderExtent.width == depth.extent.width && state.renderExtent.height == depth.extent.height,
            "the depth-only framebuffer did not use the selected mip extent");
    }
    for (const auto slice : {1u, 0x801u, 0x1001u}) {
        const auto state = DecodeState(Queue(1, slice));
        Require(state.depth->address == Base + static_cast<std::uint64_t>(slice) * ChainBytes,
            "a depth array slice used a selected-mip stride or lost its high slice bits");
    }
    auto queue = Queue(1);
    queue.context[0x002] |= 0x01000000u;
    const auto readOnly = DecodeState(queue);
    Require(readOnly.depthTest && !readOnly.depthWrite && readOnly.depth->mip == 1,
        "a read-only depth mip changed its view or enabled writes");
    Reject([&] { static_cast<void>(DecodeState(Queue(3))); }, "mip exceeds");
    queue = Queue(1, 1);
    queue.context[0x002] &= ~(0x7ffu << 13u);
    Reject([&] { static_cast<void>(DecodeState(queue)); }, "several array slices");
    queue = Queue(1);
    queue.context[0x010] = 3u | (25u << 4u) | (2u << 16u);
    Reject([&] { static_cast<void>(DecodeState(queue)); }, "SW_64KB_Z_X");
    queue = Queue(1);
    queue.context[0x010] |= 15u << 16u;
    Reject([&] { static_cast<void>(DecodeState(queue)); }, "exceeds its surface dimensions");
}

void InertDepthTests() {
    alignas(256) static std::array<std::byte, 1024> color{};
    auto queue = Queue(1);
    queue.context[0x010] = 0xa0000181;
    queue.context[0x011] = 0x20000180;
    queue.context[0x007] = 0x00ff00ff;
    queue.context[0x200] = 0x72;
    queue.context[0x012] = queue.context[0x014] = 0x02573900;
    queue.context[0x8e] = queue.context[0x8f] = 0xf;
    queue.context[0x1c5] = 9;
    queue.context[0x31c] = 0x28028;
    queue.context[0x31b] = queue.context[0x31d] = queue.context[0x1e0] = 0;
    queue.context[0x3b0] = (63u << 14u) | 3u;
    queue.context[0x3b8] = 0x09000000;
    const auto address = reinterpret_cast<std::uintptr_t>(color.data());
    queue.context[0x318] = static_cast<std::uint32_t>(address >> 8u);
    queue.context[0x390] = static_cast<std::uint32_t>(address >> 40u);
    queue.shader[0x008] = 0x100;
    std::vector<RegisterRead> reads;
    const auto previous = RegisterReadLog();
    RegisterReadLog() = &reads;
    State state{};
    try {
        state = DecodeState(queue);
    } catch (...) {
        RegisterReadLog() = previous;
        throw;
    }
    RegisterReadLog() = previous;
    Require(!state.depth && !state.depthTest && !state.depthWrite && !state.depthBoundsTest && !state.stencilTest,
        "an always-pass nonwriting depth state retained an unused depth attachment");
    Require(state.hasColorTarget && state.renderExtent.width == 64 && state.renderExtent.height == 4 && state.color.address == address,
        "the unused depth view changed the real color target or its framebuffer extent");
    for (const auto& read : reads) {
        if (read.bank != RegisterBank::Context) continue;
        Require(read.offset != 0x002 && read.offset != 0x007 && read.offset != 0x012 && read.offset != 0x014 && read.offset != 0x01a && read.offset != 0x01c,
            "the always-pass nonwriting depth state read stale depth geometry or plane addresses");
    }
    for (const auto control : {0x76u, 0x62u, 0x7au, 0x73u}) {
        queue.context[0x200] = control;
        Reject([&] { static_cast<void>(DecodeState(queue)); }, "mip exceeds");
    }
    queue.context[0x200] = 0x72;
    for (const auto bias : {0x800u, 0x1000u}) {
        queue.context[0x205] = 0x240u | bias;
        Reject([&] { static_cast<void>(DecodeState(queue)); }, "mip exceeds");
    }
    queue.context[0x205] = 0x240;
    for (const auto control : {1u, 2u, 4u}) {
        queue.context[0x000] = control;
        Reject([&] { static_cast<void>(DecodeState(queue)); }, "DB_RENDER_CONTROL");
    }
}

void HtileTests() {
    Require(HtileSliceBytes({1920, 1080}) == 196608u, "HTILE misses hardware padding for 1920x1080");
    Require(HtileSliceBytes({1804, 732}) == 131072u, "HTILE misses hardware padding for 1804x732");
    Require(HtileSliceBytes({1024, 512}) == 32768u && HtileSliceBytes({1025, 513}) == 131072u,
        "HTILE meta-block boundary rounding changed");
    Require(HtileSliceBytes({1, 1}) == 32768u && HtileSliceBytes({0, 512}) == 0, "HTILE invalid extent or minimum block changed");
    constexpr std::uint64_t metadata = 0x100000;
    Require(HtileFillCovers(metadata, {1920, 1080}, metadata, 196608), "whole padded HTILE fill rejected");
    Require(!HtileFillCovers(metadata, {1920, 1080}, metadata, 129600), "the unpadded ceil8 estimate cleared the surface");
    Require(!HtileFillCovers(metadata, {1920, 1080}, metadata, 196604) && !HtileFillCovers(metadata, {1920, 1080}, metadata + 4, 196608),
        "partial HTILE prefix or suffix cleared the surface");
    Require(!HtileFillCovers(metadata, {64, 64}, metadata, static_cast<std::size_t>(-1)), "overflowing fill range accepted");
    auto queue = Queue(0);
    queue.context[0x010] = 3u | (24u << 4u) | (1u << 29u);
    queue.context[0x005] = metadata >> 8u;
    queue.context[0x01e] = 1u;
    queue.context[0x2af] = 1u << 18u;
    std::vector<RegisterRead> reads;
    RegisterReadLog() = &reads;
    State state{};
    try { state = DecodeState(queue); } catch (...) { RegisterReadLog() = nullptr; throw; }
    RegisterReadLog() = nullptr;
    Require(state.depth && state.depth->htileAddress == (1ull << 40u) + metadata && !state.depth->htileStencil,
        "single-sample pipe-aligned depth HTILE base decode changed");
    for (const auto read : reads) Require(DrawKeyCovers(read), "draw key misses an HTILE register");
    for (const auto surface : {0u, 1u << 19u, (1u << 18u) | (1u << 19u), (1u << 18u) | (3u << 19u)}) {
        auto unsupported = queue;
        unsupported.context[0x2af] = surface;
        Require(DecodeState(unsupported).depth->htileAddress == 0, "non pipe-aligned or VRS HTILE enabled clears");
    }
    auto unsupported = queue;
    unsupported.context.erase(0x2af);
    Require(DecodeState(unsupported).depth->htileAddress == 0, "missing DB_HTILE_SURFACE assumed pipe alignment");
    unsupported = queue;
    unsupported.context[0x010] &= ~(1u << 29u);
    Require(DecodeState(unsupported).depth->htileAddress == 0, "disabled HTILE enabled clears");
    unsupported = queue;
    unsupported.context[0x010] |= 1u << 16u;
    Require(DecodeState(unsupported).depth->htileAddress == 0, "mipmapped HTILE enabled clears");
    unsupported = queue;
    unsupported.context[0x002] = View(0, 1);
    Require(DecodeState(unsupported).depth->htileAddress == 0, "array-slice HTILE enabled clears");
    unsupported = queue;
    unsupported.context[0x011] = 1u | (24u << 4u);
    unsupported.context[0x013] = unsupported.context[0x015] = 0x2000;
    Require(DecodeState(unsupported).depth->htileAddress == 0 && DecodeState(unsupported).depth->htileStencil,
        "stencil-in-HTILE was silently accepted by the depth-only path");
    unsupported.context[0x011] |= 1u << 29u;
    Require(DecodeState(unsupported).depth->htileAddress != 0 && !DecodeState(unsupported).depth->htileStencil,
        "TILE_STENCIL_DISABLE prevented depth-only HTILE clears");
}

void LayoutTests() {
    const auto macro = ComputeElementMipLayout(TextureTileMode::kZ64KBX, 4, 512, 256, 3);
    constexpr std::array<std::uint64_t, 3> sizes{0x80000, 0x20000, 0x10000};
    constexpr std::array<std::uint64_t, 3> offsets{0x30000, 0x10000, 0};
    Require(macro.size() == 3, "the macro depth chain lost a mip");
    for (std::size_t mip = 0; mip != macro.size(); ++mip) {
        Require(!macro[mip].tail && macro[mip].tiledSize == sizes[mip] && macro[mip].tiledOffset == offsets[mip],
            "the D32 depth chain disagrees with its padded mip allocation at level " + std::to_string(mip));
    }
    Require(DepthMipChainBytes(Extent, 4, 3) == ChainBytes, "the whole D32 depth chain size disagrees with its padded mip allocation");
    const auto tail = ComputeElementMipLayout(TextureTileMode::kZ64KBX, 4, 64, 64, 3);
    constexpr std::array<std::array<std::uint32_t, 2>, 3> origins{{{64, 0}, {0, 64}, {32, 0}}};
    Require(tail.size() == 3, "the shared depth tail lost a mip");
    for (std::size_t mip = 0; mip != tail.size(); ++mip) {
        Require(tail[mip].tail && tail[mip].tiledOffset == 0 && tail[mip].tailX == origins[mip][0] && tail[mip].tailY == origins[mip][1],
            "the D32 depth tail disagrees with its shared-block mip placement at level " + std::to_string(mip));
    }
    Require(DepthMipChainBytes({64, 64}, 4, 3) == 0x10000, "the shared D32 depth tail allocated more than one block");
    Require(DepthMipChainBytes(Extent, 4, 1) == 0x80000 && DepthMipChainBytes(Extent, 4, 1) == DepthSliceBytes(Extent, 4),
        "the unmipped depth plane changed its legacy footprint");
    Reject([&] { static_cast<void>(DepthMipChainBytes({64, 64}, 4, 8)); }, "invalid depth plane mip chain");
}

}

int main() {
    try {
        RegisterTests();
        InertDepthTests();
        LayoutTests();
        HtileTests();
        std::puts("depth mip register decoding, slice strides and padded chain layout tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
