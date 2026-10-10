#include "prx/libc/include/ApplicationHeap.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libc/include/GuestHeap.hpp"
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
extern "C" {
int APS5_VABI chdir_nid_postfix(const char*);
char* APS5_VABI getcwd_nid_postfix(char*, std::size_t);
int* APS5_VABI __error_nid_postfix();
}
static void Require(bool value) { if (!value) std::abort(); }
static void ConfiguredPaths() {
    const auto previous = std::filesystem::current_path();
    const auto directory = std::filesystem::temp_directory_path() /
        ("anyps5-roots-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto game = directory / "Original Game";
    const auto code = directory / "Code Cache";
    const auto work = directory / "Writable Cache";
    Require(std::filesystem::create_directories(game / "Media"));
    Require(std::filesystem::create_directories(code / "app0" / "sce_module"));
    Require(std::filesystem::create_directory(work));
    { std::ofstream file(game / "Media" / "Sample.txt"); file << "original"; }
    { std::ofstream file(code / "app0" / "sce_module" / "Plugin.prx.guest.prx"); file << "generated"; }
#ifdef _WIN32
    Require(_putenv_s("ANYPS5_GAME_ROOT", game.string().c_str()) == 0);
    Require(_putenv_s("ANYPS5_CODE_ROOT", code.string().c_str()) == 0);
#else
    Require(setenv("ANYPS5_GAME_ROOT", game.string().c_str(), 1) == 0);
    Require(setenv("ANYPS5_CODE_ROOT", code.string().c_str(), 1) == 0);
#endif
    std::filesystem::current_path(work);
    char path[1024];
    Require(getcwd_nid_postfix(path, sizeof(path)) == path && std::strcmp(path, "/app0") == 0);
    Require(std::filesystem::equivalent(ResolvePath_nid_no_patch("media/sample.TXT"), game / "Media" / "Sample.txt"));
    Require(chdir_nid_postfix("/app0/Media") == 0);
    Require(getcwd_nid_postfix(path, sizeof(path)) == path && std::strcmp(path, "/app0/Media") == 0);
    Require(std::filesystem::equivalent(ResolvePath_nid_no_patch("sample.txt"), game / "Media" / "Sample.txt"));
    Require(chdir_nid_postfix("..") == 0);
    Require(std::filesystem::equivalent(ResolveModulePath_nid_no_patch("SCE_MODULE/PLUGIN.PRX.guest.prx"),
        code / "app0" / "sce_module" / "Plugin.prx.guest.prx"));
    Require(ResolvePath_nid_no_patch("/download0") == work / "download0");
    Require(ResolvePath_nid_no_patch("/_sd") == work / "_sd");
    Require(std::filesystem::current_path() == work);
    Require(!std::filesystem::exists(work / "app0"));
    Require(!std::filesystem::exists(game / "sce_module"));
    std::filesystem::current_path(previous);
    std::filesystem::remove_all(directory);
}
static int allocations = 0;
static int releases = 0;
static void* APS5_VABI Allocate(std::size_t bytes) { ++allocations; return GuestHeap::GuestHeapAllocate_nid_postfix(bytes); }
static void APS5_VABI Release(void* pointer) { ++releases; GuestHeap::GuestHeapFree_nid_postfix(pointer); }
static void* APS5_VABI Unused() { std::abort(); }
int main(int argc, char** argv) {
    const std::array<void*, 10> api{reinterpret_cast<void*>(&Allocate), reinterpret_cast<void*>(&Release), reinterpret_cast<void*>(&Unused),
        reinterpret_cast<void*>(&Unused), reinterpret_cast<void*>(&Unused), reinterpret_cast<void*>(&Unused), reinterpret_cast<void*>(&Unused)};
    ApplicationHeapRegister_nid_no_patch(api.data());
    if (argc == 2 && std::strcmp(argv[1], "--configured") == 0) {
        ConfiguredPaths();
        return 0;
    }
    Require(argc == 1);
    const auto host = std::filesystem::canonical(std::filesystem::current_path());
    char path[1024];
    Require(getcwd_nid_postfix(path, sizeof(path)) == path && std::strcmp(path, "/") == 0);
    const auto name = "anyps5-cwd-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto directory = host / name;
    Require(std::filesystem::create_directory(directory));
    { std::ofstream file(directory / "sample.txt"); file << "sample"; }
    Require(chdir_nid_postfix(name.c_str()) == 0);
    Require(std::filesystem::current_path() == host);
    Require(getcwd_nid_postfix(path, sizeof(path)) == path && path == "/" + name);
    Require(ResolvePath_nid_no_patch("sample.txt") == directory / "sample.txt");
    Require(ResolvePath_nid_no_patch(("/" + name + "/sample.txt").c_str()) == directory / "sample.txt");
    char tiny[] = "xyz";
    Require(getcwd_nid_postfix(tiny, 2) == nullptr && *__error_nid_postfix() == 34);
    Require(std::strcmp(tiny, "xyz") == 0);
    Require(getcwd_nid_postfix(path, 0) == nullptr && *__error_nid_postfix() == 22);
    char* allocated = getcwd_nid_postfix(nullptr, 0);
    Require(allocated && std::strcmp(allocated, path) == 0 && allocations == 1);
    ApplicationHeapFree_nid_no_patch(allocated);
    Require(releases == 1);
    Require(chdir_nid_postfix("sample.txt") == -1 && *__error_nid_postfix() == 20);
    Require(chdir_nid_postfix("missing") == -1 && *__error_nid_postfix() == 2);
    Require(chdir_nid_postfix("..") == 0);
    Require(getcwd_nid_postfix(path, sizeof(path)) == path && std::strcmp(path, "/") == 0);
    Require(chdir_nid_postfix("../..") == 0);
    Require(getcwd_nid_postfix(path, sizeof(path)) == path && std::strcmp(path, "/") == 0);
    const auto external = std::filesystem::temp_directory_path() / name / "Data";
    Require(std::filesystem::create_directories(external));
    { std::ofstream file(external / "Sample.txt"); file << "external"; }
    AddPathAlias_nid_no_patch("/mounted", external.string().c_str());
    Require(chdir_nid_postfix("/mounted") == 0);
    Require(getcwd_nid_postfix(path, sizeof(path)) == path && std::strcmp(path, "/mounted") == 0);
    Require(std::filesystem::equivalent(ResolvePath_nid_no_patch("sample.TXT"), external / "Sample.txt"));
    Require(std::filesystem::current_path() == host);
    Require(chdir_nid_postfix("..") == 0);
    Require(getcwd_nid_postfix(path, sizeof(path)) == path && std::strcmp(path, "/") == 0);
    RemovePathAlias_nid_no_patch("/mounted");
    std::filesystem::remove_all(external.parent_path());
    std::filesystem::remove(directory / "sample.txt");
    std::filesystem::remove(directory);
}
