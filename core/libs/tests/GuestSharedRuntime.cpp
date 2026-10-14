#include "prx/libc/include/General.hpp"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>

extern "C" {
int APS5_VABI chdir_nid_postfix(const char*);
char* APS5_VABI getcwd_nid_postfix(char*, std::size_t);
}

static void Require(bool value) { if (!value) std::abort(); }

int main() {
    const auto previous = std::filesystem::current_path();
    const auto root = previous / ("shared-runtime-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto game = root / "original";
    const auto code = root / "code";
    const auto work = root / "saves";
    std::filesystem::create_directories(game / "Modules");
    std::filesystem::create_directories(code / "app0" / "Modules");
    std::filesystem::create_directories(work);
    { std::ofstream file(game / "Modules" / "sample.prx"); file << "original"; }
    const auto native = code / "app0" / "Modules" / "sample.prx.guest.prx";
    { std::ofstream file(native); file << "generated"; }
#ifdef _WIN32
    Require(_putenv_s("ANYPS5_GAME_ROOT", game.string().c_str()) == 0);
    Require(_putenv_s("ANYPS5_CODE_ROOT", code.string().c_str()) == 0);
#else
    Require(setenv("ANYPS5_GAME_ROOT", game.string().c_str(), 1) == 0);
    Require(setenv("ANYPS5_CODE_ROOT", code.string().c_str(), 1) == 0);
#endif
    std::filesystem::current_path(work);
    char path[256];
    Require(getcwd_nid_postfix(path, sizeof(path)) == path && std::strcmp(path, "/app0") == 0);
    Require(ResolvePath_nid_no_patch("Modules/sample.prx") == game / "Modules" / "sample.prx");
    Require(RelinkedModulePath_nid_no_patch(ResolveModulePath_nid_no_patch("Modules/sample.prx")) == native);
    Require(chdir_nid_postfix("Modules") == 0);
    Require(getcwd_nid_postfix(path, sizeof(path)) == path && std::strcmp(path, "/app0/Modules") == 0);
    Require(RelinkedModulePath_nid_no_patch(ResolveModulePath_nid_no_patch("sample.prx")) == native);
    Require(ResolvePath_nid_no_patch("/savedata0/save.bin") == work / "savedata0" / "save.bin");
    Require(std::filesystem::current_path() == work);
    std::filesystem::current_path(previous);
    std::filesystem::remove_all(root);
}
