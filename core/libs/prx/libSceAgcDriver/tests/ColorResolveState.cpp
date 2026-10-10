#include "GraphicsTests.hpp"
#include "AlignedByteArray.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace AgcDriver::Graphics;
constexpr std::uint32_t Width = 37u;
constexpr std::uint32_t Height = 69u;
const auto sourceStorage = MakeAlignedByteArray<262144, 65536>();
const auto destinationStorage = MakeAlignedByteArray<65536, 65536>();
auto& sourceMemory = *sourceStorage;
auto& destinationMemory = *destinationStorage;

class Memory {
public:
    Memory() {
        sourceMemory.fill(std::byte{0x39});
        destinationMemory.fill(std::byte{0xa7});
        GuestAllocations::Mutation mutation;
        mutation.Add(sourceMemory.data(), sourceMemory.size(), true, true, true);
        mutation.Add(destinationMemory.data(), destinationMemory.size(), true, true, true);
    }
    ~Memory() {
        GuestAllocations::Mutation mutation;
        mutation.Remove(sourceMemory.data());
        mutation.Remove(destinationMemory.data());
    }
};

class ReadLog {
public:
    explicit ReadLog(std::vector<RegisterRead>& reads) : previous(RegisterReadLog()) { RegisterReadLog() = &reads; }
    ~ReadLog() { RegisterReadLog() = previous; }
private:
    std::vector<RegisterRead>* previous;
};

void address(AgcDriver::Registers& registers, std::uint32_t low, std::uint32_t high, const void* pointer) {
    const auto value = reinterpret_cast<std::uintptr_t>(pointer);
    registers[low] = static_cast<std::uint32_t>(value >> 8u);
    registers[high] = static_cast<std::uint32_t>(value >> 40u);
}

AgcDriver::QueueState reference() {
    AgcDriver::QueueState queue;
    for (const auto range : DrawKeyRegisters) {
        if (range.bank != RegisterBank::Context) continue;
        for (std::uint32_t offset = range.first; offset < range.first + range.count; ++offset) queue.context[offset] = 0u;
    }
    queue.userConfig[0x242] = 7u;
    queue.context[0x000] = 0x20u;
    queue.context[0x010] = 0x80000180u;
    queue.context[0x011] = 0x20000180u;
    queue.context[0x00d] = (Height << 16u) | Width;
    queue.context[0x081] = 0x80000000u;
    queue.context[0x082] = (Height << 16u) | Width;
    queue.context[0x08e] = queue.context[0x08f] = 0xfu;
    queue.context[0x091] = queue.context[0x095] = (Height << 16u) | Width;
    queue.context[0x10f] = queue.context[0x110] = std::bit_cast<std::uint32_t>(Width / 2.0f);
    queue.context[0x111] = std::bit_cast<std::uint32_t>(-static_cast<float>(Height) / 2.0f);
    queue.context[0x112] = std::bit_cast<std::uint32_t>(Height / 2.0f);
    queue.context[0x201] = 0x103033u;
    queue.context[0x202] = 0xcc0030u;
    queue.context[0x203] = 0x10u;
    queue.context[0x206] = 0x43fu;
    queue.context[0x293] = 0x06020000u;
    queue.context[0x2f8] = 0x30e003u;
    queue.context[0x30e] = queue.context[0x30f] = 0xffffffffu;
    queue.context[0x313] = 0x6000u;
    for (std::uint32_t pixel = 0; pixel < 4u; ++pixel) {
        queue.context[0x2feu + pixel * 4u] = 0x5bb137d9u;
        queue.context[0x2ffu + pixel * 4u] = 0x1ff5739du;
    }
    queue.context[0x31c] = queue.context[0x32b] = 0x8828u;
    queue.context[0x31d] = 0x1b000u;
    queue.context[0x3b0] = queue.context[0x3b1] = ((Width - 1u) << 14u) | (Height - 1u);
    queue.context[0x3b8] = 0x4dc6c000u;
    queue.context[0x3b9] = 0x09c6c000u;
    address(queue.context, 0x318u, 0x390u, sourceMemory.data());
    address(queue.context, 0x327u, 0x391u, destinationMemory.data());
    return queue;
}

void rejects(const AgcDriver::QueueState& queue, std::string_view reason) {
    std::vector<RegisterRead> reads;
    std::string error;
    {
        ReadLog log(reads);
        try { static_cast<void>(DecodeColorResolvePass(queue)); }
        catch (const std::runtime_error& exception) { error = exception.what(); }
    }
    Require(!error.empty() && error.find(reason) != std::string::npos, "wrong color-resolve rejection: " + error);
    for (const auto read : reads) Require(DrawKeyCovers(read), "color-resolve rejection reads an unkeyed register");
}

void fields(const ColorResolvePass& pass) {
    Require(pass.source.samples == VK_SAMPLE_COUNT_8_BIT && pass.destination.samples == VK_SAMPLE_COUNT_1_BIT, "resolve sample counts changed");
    Require(pass.source.format == VK_FORMAT_B8G8R8A8_UNORM && pass.destination.format == pass.source.format, "captured resolve BGRA format changed");
    Require(pass.source.address == reinterpret_cast<std::uintptr_t>(sourceMemory.data()) && pass.destination.address == reinterpret_cast<std::uintptr_t>(destinationMemory.data()), "resolve source and destination addresses changed");
    Require(pass.source.bytes == sourceMemory.size() && pass.destination.bytes == destinationMemory.size(), "resolve guest layouts were multiplied or interchanged");
    Require(pass.source.componentMapping == 0xe4u && pass.destination.componentMapping == 0xe4u, "resolve component mappings changed");
    Require(pass.source.extent.width == Width && pass.source.extent.height == Height && pass.destination.extent.width == Width && pass.destination.extent.height == Height, "resolve surface extents changed");
}

}

void RunColorResolveStateTests() {
    Memory memory;
    const auto captured = reference();
    std::vector<RegisterRead> reads;
    std::optional<ColorResolvePass> full;
    {
        ReadLog log(reads);
        full = DecodeColorResolvePass(captured);
    }
    Require(full.has_value(), "captured fixed-function resolve was not recognized");
    fields(*full);
    Require(full->region.offset.x == 0 && full->region.offset.y == 0 && full->region.extent.width == Width && full->region.extent.height == Height, "full resolve region changed");
    for (const auto read : reads) Require(DrawKeyCovers(read), "color-resolve decoder reads an unkeyed register");
    for (const auto offset : {0x398u,0x399u,0x3a0u,0x3a1u}) Require(DrawKeyCovers({RegisterBank::Context,offset}), "resolve metadata extension is absent from the draw key");
    for (const auto byte : sourceMemory) Require(byte == std::byte{0x39}, "resolve state decoding modified source storage");
    for (const auto byte : destinationMemory) Require(byte == std::byte{0xa7}, "resolve state decoding modified destination storage");
    auto queue = captured;
    queue.context[0x2f8] = 0u;
    reads.clear();
    std::optional<ColorResolvePass> disabledRasterizer;
    {
        ReadLog log(reads);
        disabledRasterizer = DecodeColorResolvePass(queue);
    }
    Require(disabledRasterizer.has_value(), "an eight-sample source resolve was rejected with single-sample rasterizer state");
    fields(*disabledRasterizer);
    Require(disabledRasterizer->region.extent.width == Width && disabledRasterizer->region.extent.height == Height, "rasterizer sample count changed the resolve region");
    for (const auto read : reads) Require(DrawKeyCovers(read), "single-sample rasterizer resolve reads an unkeyed register");
    queue.context[0x31d] = 0u;
    rejects(queue, "eight source samples/fragments");
    queue = captured;
    queue.context[0x2f8] = 0x01000000u;
    rejects(queue, "matching eight coverage");
    queue = captured;
    queue.context[0x090] = (9u << 16u) | 5u;
    queue.context[0x091] = (21u << 16u) | 19u;
    auto partial = DecodeColorResolvePass(queue);
    Require(partial && partial->region.offset.x == 5 && partial->region.offset.y == 9 && partial->region.extent.width == 14u && partial->region.extent.height == 12u, "resolve scissors lost the shared source and destination offset");
    queue.context[0x292] = 2u;
    queue.context[0x094] = (11u << 16u) | 7u;
    queue.context[0x095] = (17u << 16u) | 13u;
    partial = DecodeColorResolvePass(queue);
    Require(partial && partial->region.offset.x == 7 && partial->region.offset.y == 11 && partial->region.extent.width == 6u && partial->region.extent.height == 6u, "enabled viewport scissors were not intersected");
    queue = captured;
    queue.context[0x3b8] &= ~0x40000000u;
    Require(DecodeColorResolvePass(queue).has_value(), "disabled DCC alignment flag affected raw resolve storage");
    queue = captured;
    queue.context[0x31c] = queue.context[0x32b] = 0x8028u;
    const auto rgba = DecodeColorResolvePass(queue);
    Require(rgba && rgba->source.format == VK_FORMAT_R8G8B8A8_UNORM, "matching RGBA8 resolve was rejected");
    queue = captured;
    queue.context[0x202] = 0xcc0010u;
    Require(!DecodeColorResolvePass(queue) && !DrawRejection(queue,false).empty(), "ordinary unsupported scan state was bypassed as a resolve");
    for (const auto mode : {0u,2u,4u,5u,6u,7u}) {
        queue = captured;
        queue.context[0x202] = 0xcc0000u | (mode << 4u);
        Require(!DecodeColorResolvePass(queue), "a non-resolve color operation was captured as a resolve");
    }
    queue = captured; queue.context.erase(0x202u);
    Require(!DecodeColorResolvePass(queue), "missing color control was treated as a resolve");
    queue = captured; queue.context[0x202] ^= 0x10000u; rejects(queue,"copy ROP");
    queue = captured; queue.userConfig[0x242] = 4u; rejects(queue,"2D rectangle");
    queue = captured; queue.context[0x200] = 2u; rejects(queue,"depth or stencil");
    queue = captured; queue.context[0x000] |= 2u; rejects(queue,"depth or stencil");
    queue = captured; queue.context[0x292] = 1u; rejects(queue,"disabled MSAA");
    queue = captured; queue.context[0x293] |= 0x10000u; rejects(queue,"sample iteration");
    queue = captured; queue.context[0x2f8] = 0x20e003u; rejects(queue,"matching eight coverage");
    for (const auto eqaa : {0x103032u,0x113033u,0x003033u,0x104033u}) {
        queue = captured; queue.context[0x201] = eqaa; rejects(queue,"EQAA");
    }
    queue = captured; queue.context[0x31d] = 0u; rejects(queue,"eight source samples/fragments");
    queue = captured; queue.context[0x31d] = 0x13000u; rejects(queue,"eight source samples/fragments");
    queue = captured; queue.context[0x32c] = 0x1b000u; rejects(queue,"one destination sample/fragment");
    for (const auto bit : {0x10000000u,0x4000u,0x2000u}) {
        for (const auto offset : {0x31cu,0x32bu}) {
            queue = captured; queue.context[offset] |= bit; rejects(queue,"compressed, DCC or CMASK");
        }
    }
    for (const auto offset : {0x31fu,0x321u,0x325u,0x32eu,0x330u,0x334u,0x398u,0x399u,0x3a0u,0x3a1u,0x3a8u,0x3a9u}) {
        queue = captured; queue.context[offset] = 1u; rejects(queue,"CMASK, FMASK or DCC addresses");
    }
    queue = captured; queue.context[0x31c] |= 0x40000u; rejects(queue,"nonstandard rounding");
    queue = captured; queue.context[0x32b] = 0x8028u; rejects(queue,"matching RGBA8 or BGRA8");
    queue = captured; queue.context[0x31c] = queue.context[0x32b] = 0x8e28u; rejects(queue,"matching RGBA8 or BGRA8");
    queue = captured; queue.context[0x31b] = 1u; rejects(queue,"mipmapped, array or volume");
    queue = captured; queue.context[0x3b1] |= 1u << 28u; rejects(queue,"mipmapped, array or volume");
    queue = captured; queue.context[0x3b9] = 0x0ac6c000u; rejects(queue,"mipmapped, array or volume");
    queue = captured; queue.context[0x3b9] = 0x09000000u; rejects(queue,"SW_64KB_R_X");
    queue = captured; queue.context[0x3b1] ^= 1u; rejects(queue,"matching source and destination extents");
    queue = captured; address(queue.context,0x327u,0x391u,sourceMemory.data()); rejects(queue,"storage overlaps");
    queue = captured; queue.context[0x390] = 0x100u; rejects(queue,"invalid color address extension");
    queue = captured; queue.context[0x08e] = 0xffu; rejects(queue,"all source components in MRT0");
    queue = captured; queue.context[0x080] = 1u; rejects(queue,"window offset");
    queue = captured; queue.context[0x110] = 0u; rejects(queue,"full-surface viewport");
    queue = captured; queue.context[0x111] = 0x7fc00000u; rejects(queue,"non-finite register");
    queue = captured; queue.context[0x090] = (5u << 16u) | 10u; queue.context[0x091] = (5u << 16u) | 9u; rejects(queue,"inverted scissor");
    queue = captured; queue.context.erase(0x32bu); rejects(queue,"missing register");
}
