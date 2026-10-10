#include "GraphicsTests.hpp"
#include "AlignedByteArray.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderInputState.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include <algorithm>
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

const auto colorStorage = MakeAlignedByteArray<262144, 65536>();
const auto depthStorage = MakeAlignedByteArray<262144, 65536>();
const auto stencilStorage = MakeAlignedByteArray<262144, 65536>();
auto& colorMemory = *colorStorage;
auto& depthMemory = *depthStorage;
auto& stencilMemory = *stencilStorage;
alignas(256) std::array<std::uint32_t, 4> pixelProgram{0xbf810000u};

class RegisteredMemory {
public:
    RegisteredMemory() {
        GuestAllocations::Mutation mutation;
        mutation.Add(colorMemory.data(), colorMemory.size(), true, true, true);
        mutation.Add(depthMemory.data(), depthMemory.size(), true, true, true);
        mutation.Add(stencilMemory.data(), stencilMemory.size(), true, true, true);
        mutation.Add(pixelProgram.data(), sizeof(pixelProgram), true, false, true);
    }
    ~RegisteredMemory() {
        GuestAllocations::Mutation mutation;
        mutation.Remove(colorMemory.data());
        mutation.Remove(depthMemory.data());
        mutation.Remove(stencilMemory.data());
        mutation.Remove(pixelProgram.data());
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
    queue.userConfig[0x242] = 4u;
    queue.userConfig[0x24b] = 0u;
    queue.context = {
        {0x000,0x22u}, {0x002,0u}, {0x007,(68u << 16u) | 36u},
        {0x00a,0u}, {0x00b,0x3f800000u}, {0x010,0xa000018fu}, {0x011,0x20000181u},
        {0x080,0u}, {0x081,0x80000000u}, {0x082,(69u << 16u) | 37u}, {0x083,0xffffu},
        {0x08c,0xaa99aaaau}, {0x08d,0u}, {0x08e,0xfu}, {0x08f,0xfu},
        {0x090,0x80000000u}, {0x091,(69u << 16u) | 37u},
        {0x094,0x80000000u}, {0x095,(69u << 16u) | 37u},
        {0x0b4,0u}, {0x0b5,0x3f800000u}, {0x00c,0u}, {0x00d,(69u << 16u) | 37u},
        {0x10b,0x333u}, {0x10c,0xffffff00u}, {0x10d,0xffffff00u},
        {0x10f,std::bit_cast<std::uint32_t>(18.5f)}, {0x110,std::bit_cast<std::uint32_t>(18.5f)},
        {0x111,std::bit_cast<std::uint32_t>(-34.5f)}, {0x112,std::bit_cast<std::uint32_t>(34.5f)},
        {0x113,0x3f800000u}, {0x114,0u}, {0x1b3,2u}, {0x1b4,2u}, {0x1b6,0u},
        {0x1c3,4u}, {0x1c4,0u}, {0x1c5,4u}, {0x1e0,0x10001u},
        {0x200,0x771u}, {0x201,0x103033u}, {0x202,0xcc0010u}, {0x203,0x10u},
        {0x204,0x80000u}, {0x205,0x240u}, {0x206,0x43fu}, {0x207,0u},
        {0x292,3u}, {0x293,0x06020000u}, {0x2d5,0x02002000u}, {0x2dc,0xaa00u},
        {0x2f8,0x30e003u}, {0x2f9,0x2du}, {0x30e,0xffffffffu}, {0x30f,0xffffffffu},
        {0x313,0x6000u}, {0x31b,0u}, {0x31c,0x8e28u}, {0x31d,0x1b000u},
        {0x3b0,(36u << 14u) | 68u}, {0x3b8,0x09c6c000u}
    };
    for (std::uint32_t pixel = 0; pixel < 4u; ++pixel) {
        queue.context[0x2feu + pixel * 4u] = 0x5bb137d9u;
        queue.context[0x2ffu + pixel * 4u] = 0x1ff5739du;
        queue.context[0x300u + pixel * 4u] = 0u;
        queue.context[0x301u + pixel * 4u] = 0u;
    }
    address(queue.context, 0x318u, 0x390u, colorMemory.data());
    address(queue.context, 0x012u, 0x01au, depthMemory.data());
    address(queue.context, 0x014u, 0x01cu, depthMemory.data());
    address(queue.context, 0x013u, 0x01bu, stencilMemory.data());
    address(queue.context, 0x015u, 0x01du, stencilMemory.data());
    address(queue.shader, 0x008u, 0x009u, pixelProgram.data());
    return queue;
}

void locations(const State& state) {
    constexpr std::array<std::array<std::uint32_t, 2>, 8> expected{{
        {1u,5u}, {15u,11u}, {9u,3u}, {3u,13u}, {5u,1u}, {11u,15u}, {13u,7u}, {7u,9u}
    }};
    Require(state.sampleLocations.size() == expected.size(), "eight sample locations were not decoded");
    Require(state.sampleLocationsGrid.width == 1u && state.sampleLocationsGrid.height == 1u, "identical guest quad was not a one-pixel sample grid");
    Require(state.depth && state.depth->sampleLocations.size() == expected.size(), "depth sample locations are missing");
    Require(state.depth->sampleLocationsGrid.width == 1u && state.depth->sampleLocationsGrid.height == 1u, "depth sample grid differs from rasterization");
    for (std::size_t sample = 0; sample < expected.size(); ++sample) {
        const auto x = static_cast<float>(expected[sample][0]) / 16.0f;
        const auto y = static_cast<float>(expected[sample][1]) / 16.0f;
        Require(state.sampleLocations[sample].x == x && state.sampleLocations[sample].y == y, "guest sample location or framebuffer Y convention changed");
        Require(state.depth->sampleLocations[sample].x == x && state.depth->sampleLocations[sample].y == y, "depth and raster sample locations disagree");
    }
}

std::string decodedError(const AgcDriver::QueueState& queue) {
    try { static_cast<void>(DecodeState(queue)); }
    catch (const std::runtime_error& error) { return error.what(); }
    throw std::runtime_error("invalid multisample state was accepted");
}

void rejects(const AgcDriver::QueueState& queue, std::string_view expected, bool shared = true, std::string_view precheckExpected = {}) {
    std::vector<RegisterRead> reads;
    std::string decoded;
    std::string precheck;
    {
        ReadLog log(reads);
        decoded = decodedError(queue);
        precheck = DrawRejection(queue, false);
    }
    Require(decoded.find(expected) != std::string::npos, "wrong multisample decoder rejection: " + decoded);
    if (shared) Require(!precheck.empty(), "multisample precheck accepted a covered invalid feature");
    if (!precheck.empty()) {
        const auto compatible = precheckExpected.empty() ? expected : precheckExpected;
        Require(precheck.find(compatible) != std::string::npos, "multisample precheck disagrees with decoder: " + precheck);
    }
    for (const auto read : reads) Require(DrawKeyCovers(read), "MSAA rejection reads an unkeyed register");
}

void sharedRejections() {
    auto queue = reference();
    queue.context[0x010] = 0xa0000183u;
    rejects(queue, "depth and rasterization sample counts differ");
    queue = reference();
    queue.context[0x31d] = 0u;
    rejects(queue, "color and rasterization sample counts differ");
    queue = reference();
    queue.context[0x302] ^= 1u;
    rejects(queue, "locations differing across a pixel quad");
    queue = reference();
    for (const auto offset : {0x300u,0x304u,0x308u,0x30cu}) queue.context[offset] = 1u;
    rejects(queue, "locations beyond the eight active samples");
    for (const auto offset : {0x30eu,0x30fu}) {
        queue = reference();
        queue.context[offset] &= ~1u;
        rejects(queue, "sample masks are unsupported");
    }
    queue = reference();
    queue.context[0x2f8] ^= 0x2000u;
    rejects(queue, "maximum sample distance does not match");
    queue = reference();
    queue.context[0x292] &= ~1u;
    rejects(queue, "eight-sample coverage requires MSAA scan conversion");
    queue = reference();
    queue.context[0x2f8] = 0u;
    rejects(queue, "MSAA scan conversion without a multisample coverage count");
    for (const auto count : {1u,2u}) {
        queue = reference();
        queue.context[0x2f8] = (count << 20u) | (7u << 13u) | count;
        rejects(queue, "matching eight coverage and exposed samples");
    }
    queue = reference();
    queue.context[0x2f8] = 0x20e003u;
    rejects(queue, "matching eight coverage and exposed samples");
    for (const auto invalid : {0x103032u,0x003033u,0x113033u,0x103043u,0x103433u,0x104033u}) {
        queue = reference();
        queue.context[0x201] = invalid;
        rejects(queue, "mixed anchor counts, overrasterization or unsupported EQAA state");
    }
    for (const auto control : {0u,0x22u}) {
        for (const auto coverage : {0x80u,0x100u}) {
            queue = reference();
            queue.context[0x000] = control;
            queue.context[0x203] |= coverage;
            rejects(queue, "multisampled shader coverage and sample-mask exports", true, "shader coverage");
        }
    }
    for (const auto centroid : {0x4u,0x40u}) {
        queue = reference();
        queue.context[0x1b3] |= centroid;
        rejects(queue, "centroid interpolation with guest centroid priorities");
    }
    queue = reference();
    queue.context[0x293] |= 0x10000u;
    rejects(queue, "sample iteration");
    for (const auto control : {0u,0x22u}) {
        for (const auto view : {1u,0x01000000u,0x02000000u,0x04000000u}) {
            queue = reference();
            queue.context[0x000] = control;
            queue.context[0x002] = view;
            rejects(queue, control == 0u ? "multisampled read-only, mipmapped or array depth views" : "read-only, mipmapped or array views");
        }
    }
}

void colorRejections() {
    auto queue = reference();
    queue.context[0x31d] = 0x13000u;
    rejects(queue, "mixed color samples/fragments", false);
    queue = reference();
    queue.context[0x3b0] |= 1u << 28u;
    rejects(queue, "multisampled mipmapped, array or volume color views", false);
    queue = reference();
    queue.context[0x31b] = (1u << 13u) | 1u;
    rejects(queue, "multisampled mipmapped, array or volume color views", false);
    queue = reference();
    queue.context[0x3b0] |= 1u << 28u;
    queue.context[0x31b] = 1u << 26u;
    rejects(queue, "multisampled mipmapped, array or volume color views", false);
    queue = reference();
    queue.context[0x3b8] = 0x0ac6c000u;
    rejects(queue, "multisampled mipmapped, array or volume color views", false);
    queue = reference();
    queue.context[0x3b8] = 0x09000000u;
    rejects(queue, "multisampled color requires SW_64KB_R_X and four-byte elements", false);
    queue = reference();
    queue.context[0x31c] = 0x872cu;
    rejects(queue, "multisampled color requires SW_64KB_R_X and four-byte elements", false);
    for (const auto metadata : {0x10000000u,0x2000u}) {
        queue = reference();
        queue.context[0x31c] |= metadata;
        rejects(queue, "multisampled DCC or CMASK color metadata", false);
    }
    queue = reference();
    queue.context.erase(0x304u);
    rejects(queue, "missing register", false);
}

void keyCoverage(const AgcDriver::QueueState& queue) {
    std::vector<RegisterRead> reads;
    {
        ReadLog log(reads);
        const auto state = DecodeState(queue);
        Require(DrawRejection(queue, false).empty(), "precheck rejected the actual eight-sample reference");
        static_cast<void>(DecodePixelStageInfo(queue.context, ExportMappings(state)));
    }
    Require(!reads.empty(), "MSAA register facade recorded no reads");
    for (const auto read : reads) Require(DrawKeyCovers(read), "MSAA decoder reads an unkeyed register");
    for (const auto offset : {0x010u,0x011u,0x201u,0x203u,0x292u,0x293u,0x2f8u,0x30eu,0x30fu,0x31du})
        Require(DrawKeyCovers({RegisterBank::Context,offset}), "MSAA sample state is absent from the draw key");
    for (std::uint32_t offset = 0x2feu; offset <= 0x30du; ++offset) {
        Require(DrawKeyCovers({RegisterBank::Context,offset}), "programmed sample location is absent from the draw key");
        Require(std::any_of(reads.begin(),reads.end(),[&](const auto read) { return read.bank == RegisterBank::Context && read.offset == offset; }), "programmed sample location was not read");
    }
    auto changed = queue;
    for (const auto offset : {0x2feu,0x302u,0x306u,0x30au}) changed.context[offset] ^= 3u;
    Require(DrawRejection(changed, false).empty(), "a different valid one-pixel sample pattern was rejected");
    const auto pattern = DecodeState(changed);
    Require(pattern.sampleLocations[0].x == 2.0f / 16.0f && pattern.sampleLocations[0].y == 5.0f / 16.0f, "sample-location update reused stale decoded state");
}

}

void RunMsaaStateTests() {
    RegisteredMemory memory;
    const auto queue = reference();
    const auto state = DecodeState(queue);
    Require(DrawRejection(queue,false).empty(), "precheck rejected the actual MSAA reference");
    Require(state.rasterizationSamples == VK_SAMPLE_COUNT_8_BIT && state.sampleMask == 0xffffffffu, "actual coverage count or full sample mask changed");
    Require(state.depth && state.depth->samples == VK_SAMPLE_COUNT_8_BIT && state.depth->format == VK_FORMAT_D32_SFLOAT_S8_UINT, "actual eight-sample depth/stencil attachment changed");
    Require(!state.depthTest && !state.depthWrite && state.stencilTest && state.stencilFront.passOp == VK_STENCIL_OP_REPLACE, "actual stencil clear did not preserve depth");
    Require(state.colors.size() == 1u && state.color.samples == VK_SAMPLE_COUNT_8_BIT && state.color.format == VK_FORMAT_B8G8R8A8_SRGB, "actual eight-sample color format changed");
    Require(state.color.address == reinterpret_cast<std::uintptr_t>(colorMemory.data()) && state.color.bytes == 262144u && state.color.extent.width == 37u && state.color.extent.height == 69u, "actual MSAA guest color allocation was multiplied or misaligned");
    Require(state.renderExtent.width == 37u && state.renderExtent.height == 69u, "actual attachment extents disagree");
    locations(state);
    keyCoverage(queue);
    sharedRejections();
    colorRejections();
    auto single = queue;
    single.context[0x010] = 0xa0000183u;
    single.context[0x31d] = 0u;
    single.context[0x201] = 0u;
    single.context[0x2f8] = 0u;
    single.context[0x292] = 2u;
    for (std::uint32_t offset = 0x2feu; offset <= 0x30du; ++offset) single.context[offset] = 0u;
    const auto one = DecodeState(single);
    Require(DrawRejection(single,false).empty() && one.rasterizationSamples == VK_SAMPLE_COUNT_1_BIT && one.depth->samples == VK_SAMPLE_COUNT_1_BIT && one.color.samples == VK_SAMPLE_COUNT_1_BIT, "single-sample state regressed beside MSAA");
    Require(one.sampleLocations.empty() && one.depth->sampleLocations.empty() && one.color.bytes == 65536u, "single-sample state inherited multisample layout or locations");
}
