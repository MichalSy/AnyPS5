#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/ColorResolveGuard.hpp"
#include <array>
#include <bit>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace AgcDriver;
using namespace AgcDriver::DriverDetail;

constexpr std::uint64_t VertexAddress = 0x100000u;
constexpr std::uint64_t FragmentAddress = 0x200000u;
constexpr std::array<std::uint32_t, 27> Vertex{
    0xbfa00001u, 0x93ebff03u, 0x00080008u, 0x8700ff03u, 0x000000ffu,
    0x8f6a8c6bu, 0x887c6a00u, 0xbf900009u, 0x81ea6bc0u, 0x90fe6ac1u,
    0xf8000941u, 0u, 0x81ea00c0u, 0xbf8cff0fu, 0x90fe6ac1u,
    0x34040a81u, 0x36060ac2u, 0x7e000280u, 0x7e0202f2u, 0x36040482u,
    0x4a0606c1u, 0x4a0404c1u, 0x7e060b03u, 0x7e040b02u,
    0xf80008cfu, 0x01000302u, 0xbf810000u
};
constexpr std::array<std::uint32_t, 5> Fragment{0x7e0002ffu, 0x3c003c00u, 0xf8001c0fu, 0u, 0xbf810000u};

struct Case {
    QueueState queue;
    ShaderRegistry registry;
    std::shared_ptr<ShaderSnapshot> vertex;
    std::shared_ptr<ShaderSnapshot> fragment;
    Pm4::DrawParameters draw{0u, 3u, 0u, 1u, 0u, false};

    Case() {
        queue.shader = {{0xc8u, static_cast<std::uint32_t>(VertexAddress >> 8u)}, {0xc9u, 0u}, {0x8bu, 0x30000u}, {0x8au, 0x622c0042u},
            {0x8u, static_cast<std::uint32_t>(FragmentAddress >> 8u)}, {0x9u, 0u}, {0xbu, 0u}, {0xau, 0x022c0000u}};
        queue.userConfig = {{0x242u, 7u}};
        queue.context = {{0x2d5u, 0x02002000u}, {0x2d6u, 0u}, {0x1b6u, 0u}, {0x203u, 0x10u},
            {0x204u, 0x80000u}, {0x205u, 0x240u}, {0x207u, 0u}, {0x1c2u, 1u}, {0x1c3u, 4u}, {0x1c4u, 0u}, {0x1c5u, 4u},
            {0x83u, 0xffffu}, {0x8cu, 0xaa99aaaau}, {0x8du, 0u}, {0x2f9u, 0x2du},
            {0x2dcu, 0xaa00u}, {0x30eu, 0xffffffffu}, {0x30fu, 0xffffffffu}, {0x29bu, 3u}};
        vertex = std::make_shared<ShaderSnapshot>(ShaderSnapshot{VertexAddress, 0u, 2u, {Vertex.begin(), Vertex.end()}, {}});
        fragment = std::make_shared<ShaderSnapshot>(ShaderSnapshot{FragmentAddress, 0u, 1u, {Fragment.begin(), Fragment.end()}, {}});
        registry.emplace(VertexAddress, vertex);
        registry.emplace(FragmentAddress, fragment);
    }

    void Check() const { RequireFixedColorResolvePrograms(queue, registry, draw); }
};

template<typename TAction>
void Reject(TAction action, const char* expected) {
    try { action(); }
    catch (const std::exception& error) {
        Graphics::Require(std::string(error.what()).find(expected) != std::string::npos, error.what());
        return;
    }
    throw std::runtime_error(std::string("accepted unsupported resolve: ") + expected);
}

void CoverageAndInputs() {
    Case example;
    example.Check();
    const std::array<std::array<float, 4>, 3> corners{{{-1.0f, -1.0f, 0.0f, 1.0f}, {1.0f, -1.0f, 0.0f, 1.0f}, {-1.0f, 1.0f, 0.0f, 1.0f}}};
    for (std::uint32_t id = 0u; id < 3u; ++id) {
        const auto x = static_cast<float>(std::bit_cast<std::int32_t>(((id << 1u) & 2u) - 1u));
        const auto y = static_cast<float>(std::bit_cast<std::int32_t>((id & 0xfffffffeu) - 1u));
        Graphics::Require(x == corners[id][0] && y == corners[id][1], "canonical rectangle vertex does not match the independent corner oracle");
    }
    example.queue.shader[0xcu] = 0xdeadbeefu;
    example.queue.shader[0x8cu] = 0xa5a5a5a5u;
    example.queue.shader[0x240u] = 0x12345678u;
    example.vertex->code.push_back(0xffffffffu);
    example.fragment->code.push_back(0xe0700000u);
    example.Check();
    std::vector<Graphics::RegisterRead> reads;
    Graphics::RegisterReadLog() = &reads;
    example.Check();
    Graphics::RegisterReadLog() = nullptr;
    Graphics::Require(!reads.empty(), "resolve guard did not record register reads");
    for (const auto& read : reads) Graphics::Require(Graphics::DrawKeyCovers(read), "resolve guard register is outside the draw key");
}

void DrawAndRouting() {
    for (const auto count : {0u, 1u, 2u, 4u, 6u}) {
        Case example; example.draw.indexCount = count; Reject([&] { example.Check(); }, "count=");
    }
    for (const auto count : {0u, 2u}) {
        Case example; example.draw.instanceCount = count; Reject([&] { example.Check(); }, "instances=");
    }
    { Case example; example.draw.indexed = true; Reject([&] { example.Check(); }, "indexed=1"); }
    { Case example; example.draw.indirect.emplace(); Reject([&] { example.Check(); }, "indirect=1"); }
    { Case example; example.draw.firstVertex = 1u; Reject([&] { example.Check(); }, "firstVertex=1"); }
    { Case example; example.draw.firstInstance = 1u; Reject([&] { example.Check(); }, "firstInstance=1"); }
    for (const auto primitive : {4u, 17u, 19u}) {
        Case example; example.queue.userConfig[0x242u] = primitive; Reject([&] { example.Check(); }, "2D rectangle");
    }
    for (const auto output : {0u, 1u, 2u, 4u, 7u, 11u, 19u, 67u}) {
        Case example; example.queue.context[0x29bu] = output; Reject([&] { example.Check(); }, "rectangle primitive output");
    }
    for (const auto offset : {0x2d5u, 0x2d6u, 0x1b6u}) {
        Case example; example.queue.context[offset] ^= offset == 0x1b6u ? 0x8000u : 0x20u;
        Reject([&] { example.Check(); }, "routing");
    }
    for (const auto offset : {0x8bu, 0xbu}) {
        for (const auto extra : {1u, 2u, 0x8000000u}) {
            Case example; example.queue.shader[offset] |= extra; Reject([&] { example.Check(); }, "user SGPRs");
        }
    }
    for (const auto offset : {0x203u, 0x204u, 0x205u, 0x207u}) {
        Case example; example.queue.context[offset] ^= 1u; Reject([&] { example.Check(); }, "clip or raster");
    }
    for (const auto offset : {0x8au, 0xau}) {
        Case example; example.queue.shader[offset] ^= 1u; Reject([&] { example.Check(); }, "floating-point configuration");
    }
    for (const auto offset : {0x83u, 0x8cu, 0x8du, 0x2f9u}) {
        Case example; example.queue.context[offset] ^= 1u; Reject([&] { example.Check(); }, "vertex quantization");
    }
    for (const auto offset : {0x2dcu, 0x30eu, 0x30fu}) {
        Case example; example.queue.context[offset] ^= 1u; Reject([&] { example.Check(); }, "sample masks");
    }
    for (const auto offset : {0x1c2u, 0x1c3u, 0x1c4u, 0x1c5u}) {
        Case example; example.queue.context[offset] ^= 1u; Reject([&] { example.Check(); }, "export format");
    }
    { Case example; example.queue.shader.erase(0xbu); Reject([&] { example.Check(); }, "missing register"); }
}

void RegisteredBounds() {
    for (const bool front : {false, true}) {
        const auto address = front ? VertexAddress : FragmentAddress;
        const auto base = front ? 0xc8u : 0x8u;
        { Case example; example.registry.erase(address); Reject([&] { example.Check(); }, front ? "registered shader" : "outside registered"); }
        { Case example; (front ? example.vertex : example.fragment)->type = 0u; Reject([&] { example.Check(); }, "incompatible shader"); }
        { Case example; (front ? example.vertex : example.fragment)->code.pop_back(); Reject([&] { example.Check(); }, "truncated"); }
        { Case example; example.queue.shader[base + 1u] = 0x100u; Reject([&] { example.Check(); }, "reserved program"); }
        { Case example; example.queue.shader[base] = 0u; Reject([&] { example.Check(); }, "registered shader"); }
        { Case example; example.registry[address].reset(); Reject([&] { example.Check(); }, "null registered"); }
        { Case example; (front ? example.vertex : example.fragment)->codeAddress -= 4u;
          Reject([&] { example.Check(); }, "inconsistent registered"); }
        { Case example; auto snapshot = front ? example.vertex : example.fragment; example.registry.erase(address);
          snapshot->codeAddress = address - 1u; example.registry.emplace(snapshot->codeAddress, snapshot);
          Reject([&] { example.Check(); }, "not dword aligned"); }
        { Case example; auto snapshot = front ? example.vertex : example.fragment; example.registry.erase(address);
          snapshot->codeAddress = address - 0x100u; snapshot->code.insert(snapshot->code.begin(), 64u, 0xffffffffu);
          example.registry.emplace(snapshot->codeAddress, snapshot); example.Check(); }
        { Case example; auto snapshot = front ? example.vertex : example.fragment; example.registry.erase(address);
          snapshot->codeAddress = address - 0x100u; example.registry.emplace(snapshot->codeAddress, snapshot);
          Reject([&] { example.Check(); }, "outside registered"); }
    }
}

void InstructionEffects() {
    const std::array<std::pair<std::size_t, std::uint32_t>, 13> changedVertex{{
        {3u, 0x8700ff08u}, {7u, 0xbf900001u}, {9u, 0x90fe6ac0u}, {10u, 0xf8000940u}, {11u, 1u},
        {14u, 0x90fe6ac0u}, {15u, 0x34041081u}, {16u, 0x36060ac1u}, {20u, 0x4a060681u},
        {24u, 0xf80008dfu}, {25u, 0x00000302u}, {26u, 0xbf82ffffu}, {17u, 0xe0700000u}
    }};
    for (const auto& [word, instruction] : changedVertex) {
        Case example; example.vertex->code[word] = instruction;
        Reject([&] { example.Check(); }, "vertex instructions or coverage");
    }
    const std::array<std::pair<std::size_t, std::uint32_t>, 5> changedFragment{{
        {0u, 0x7e000208u}, {1u, 0u}, {2u, 0xf8001d4fu}, {3u, 0x01000000u}, {4u, 0xbf82ffffu}
    }};
    for (const auto& [word, instruction] : changedFragment) {
        Case example; example.fragment->code[word] = instruction;
        Reject([&] { example.Check(); }, "fragment instructions or effects");
    }
    { Case example; example.vertex->code.insert(example.vertex->code.end() - 1, 0xbf8a0000u);
      Reject([&] { example.Check(); }, "vertex instructions or coverage"); }
    { Case example; example.fragment->code.insert(example.fragment->code.end() - 1, 0xe0700000u);
      Reject([&] { example.Check(); }, "fragment instructions or effects"); }
}

}

int main() {
    try {
        CoverageAndInputs();
        DrawAndRouting();
        RegisteredBounds();
        InstructionEffects();
        std::cout << "Fixed color resolve shader guard tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
