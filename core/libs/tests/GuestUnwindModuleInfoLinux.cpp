#include "SceTypes.hpp"
#include "prx/libkernel/KernelErrors.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <link.h>
#include <initializer_list>
#include <sys/mman.h>

extern "C" {
void* APS5_VABI dlopen_nid_postfix(const char*, int);
void* APS5_VABI dlsym_nid_postfix(void*, const char*);
int APS5_VABI dlclose_nid_postfix(void*);
int APS5_VABI sceKernelGetModuleInfoForUnwind(std::uint64_t, int, ModuleInfoForUnwind*);
int APS5_VABI sceKernelGetModuleInfoFromAddr(std::uint64_t, int, ModuleInfoEx*);
}

static void Require(bool value) { if (!value) std::abort(); }
static_assert(sizeof(ModuleInfoForUnwind) == 0x130);
static_assert(offsetof(ModuleInfoForUnwind, eh_frame_hdr_addr) == 0x108);
static_assert(offsetof(ModuleInfoForUnwind, eh_frame_addr) == 0x110);
static_assert(offsetof(ModuleInfoForUnwind, eh_frame_size) == 0x118);
static_assert(offsetof(ModuleInfoForUnwind, seg0_addr) == 0x120);

static bool Contains(const dl_phdr_info& image, std::uintptr_t address, std::uint64_t size) {
    for (std::uint16_t index = 0; index < image.dlpi_phnum; ++index) {
        const auto& header = image.dlpi_phdr[index];
        const auto begin = image.dlpi_addr + header.p_vaddr;
        if (header.p_type == PT_LOAD && address >= begin && address - begin <= header.p_memsz &&
            size <= header.p_memsz - (address - begin)) return true;
    }
    return false;
}

struct Expected {
    std::uintptr_t address;
    ModuleInfoForUnwind info{};
    bool found = false;
};

static int Find(dl_phdr_info* image, std::size_t, void* data) {
    auto& expected = *static_cast<Expected*>(data);
    if (!Contains(*image, expected.address, 1)) return 0;
    expected.found = true;
    bool first = true;
    for (std::uint16_t index = 0; index < image->dlpi_phnum; ++index) {
        const auto& header = image->dlpi_phdr[index];
        const auto address = image->dlpi_addr + header.p_vaddr;
        if (header.p_type == PT_LOAD && first) {
            expected.info.seg0_addr = address;
            expected.info.seg0_size = header.p_memsz;
            first = false;
        }
        if (header.p_type != PT_GNU_EH_FRAME) continue;
        Require(Contains(*image, address, 8));
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(address);
        Require(bytes[0] == 1 && bytes[1] == 0x1b);
        std::int32_t offset;
        std::memcpy(&offset, bytes + 4, sizeof(offset));
        expected.info.eh_frame_hdr_addr = address;
        expected.info.eh_frame_addr = address + 4 + static_cast<std::intptr_t>(offset);
        auto cursor = expected.info.eh_frame_addr;
        for (;;) {
            Require(Contains(*image, cursor, 4));
            std::uint32_t length;
            std::memcpy(&length, reinterpret_cast<const void*>(cursor), sizeof(length));
            cursor += 4;
            if (length == 0) break;
            std::uint64_t size = length;
            if (length == UINT32_MAX) {
                Require(Contains(*image, cursor, 8));
                std::memcpy(&size, reinterpret_cast<const void*>(cursor), sizeof(size));
                cursor += 8;
            }
            Require(Contains(*image, cursor, size));
            cursor += size;
        }
        expected.info.eh_frame_size = cursor - expected.info.eh_frame_addr;
    }
    return 1;
}

static ModuleInfoForUnwind Query(const void* address, int flags = 1, int status = 0) {
    struct Output {
        std::uint64_t before;
        ModuleInfoForUnwind info;
        std::uint64_t after;
    } output{};
    std::memset(&output.info, 0xa5, sizeof(output.info));
    output.before = 0x0123456789abcdef;
    output.after = 0xfedcba9876543210;
    output.info.st_size = sizeof(output.info);
    Require(sceKernelGetModuleInfoForUnwind(reinterpret_cast<std::uintptr_t>(address), flags, &output.info) == status);
    Require(output.before == 0x0123456789abcdef && output.after == 0xfedcba9876543210);
    if (status == 0) {
        Require(output.info.st_size == sizeof(output.info));
        Require(std::memchr(output.info.name, 0, sizeof(output.info.name)) != nullptr);
    }
    return output.info;
}

static void CheckImage(const void* address) {
    Expected expected{reinterpret_cast<std::uintptr_t>(address)};
    dl_iterate_phdr(Find, &expected);
    Require(expected.found && expected.info.eh_frame_hdr_addr != 0 && expected.info.eh_frame_addr != 0);
    Require(expected.info.eh_frame_size > 4);
    for (int flags : {0, 1}) {
        const auto info = Query(address, flags);
        Require(info.eh_frame_hdr_addr == expected.info.eh_frame_hdr_addr);
        Require(info.eh_frame_addr == expected.info.eh_frame_addr && info.eh_frame_size == expected.info.eh_frame_size);
        Require(info.seg0_addr == expected.info.seg0_addr && info.seg0_size == expected.info.seg0_size);
    }
    ModuleInfoEx extended{};
    extended.st_size = sizeof(extended);
    Require(sceKernelGetModuleInfoFromAddr(reinterpret_cast<std::uintptr_t>(address), 2, &extended) == 0);
    const auto info = Query(address);
    Require(extended.eh_frame_hdr_addr == info.eh_frame_hdr_addr);
    Require(extended.eh_frame_addr == info.eh_frame_addr && extended.eh_frame_size == info.eh_frame_size);
}

int main(int argc, char** argv) {
    Require(argc == 2);
    CheckImage(reinterpret_cast<const void*>(&Query));
    CheckImage(reinterpret_cast<const void*>(&sceKernelGetModuleInfoForUnwind));
    void* module = dlopen_nid_postfix(argv[1], 2);
    Require(module != nullptr);
    const auto* code = dlsym_nid_postfix(module, "GuestModuleAdd");
    Require(code != nullptr);
    CheckImage(code);
    const auto info = Query(code);
    CheckImage(reinterpret_cast<const void*>(info.eh_frame_hdr_addr));
    Require(dlclose_nid_postfix(module) == 0);
    Query(code, 1, SCE_KERNEL_ERROR_ESRCH);
    void* anonymous = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Require(anonymous != MAP_FAILED);
    const auto anon = Query(anonymous);
    Require(anon.eh_frame_hdr_addr == 0 && anon.eh_frame_addr == 0 && anon.eh_frame_size == 0);
    Require(anon.seg0_addr <= reinterpret_cast<std::uintptr_t>(anonymous));
    Require(reinterpret_cast<std::uintptr_t>(anonymous) - anon.seg0_addr < anon.seg0_size);
    Require(munmap(anonymous, 4096) == 0);
    int stack = 0;
    const auto stackInfo = Query(&stack);
    Require(stackInfo.eh_frame_hdr_addr == 0 && stackInfo.eh_frame_addr == 0 && stackInfo.eh_frame_size == 0);
    Query(nullptr, 1, SCE_KERNEL_ERROR_ESRCH);
    Query(reinterpret_cast<const void*>(1), 1, SCE_KERNEL_ERROR_ESRCH);
    Require(sceKernelGetModuleInfoForUnwind(reinterpret_cast<std::uintptr_t>(code), 1, nullptr) == SCE_KERNEL_ERROR_EFAULT);
    std::puts("guest Linux unwind module metadata tests passed");
}
