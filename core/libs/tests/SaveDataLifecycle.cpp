#include "SceTypes.hpp"
#include "prx/libc/include/FileStream.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

extern "C" {
int APS5_VABI sceSaveDataInitialize3(const void*);
int APS5_VABI sceSaveDataTerminate();
int APS5_VABI sceSaveDataMount3(const SaveDataMount3*, SaveDataMountResult*);
int APS5_VABI sceSaveDataUmount2(std::uint32_t, const SaveDataMountPoint*);
int APS5_VABI sceSaveDataSetupSaveDataMemory2(const SaveDataMemorySetup2*, SaveDataMemorySetupResult*);
int APS5_VABI sceSaveDataGetSaveDataMemory2(SaveDataMemoryGet2*);
int APS5_VABI sceSaveDataSetSaveDataMemory2(const SaveDataMemorySet2*);
FileStream* APS5_VABI fopen_nid_postfix(const char*, const char*);
std::size_t APS5_VABI fread_nid_postfix(void*, std::size_t, std::size_t, FileStream*);
std::size_t APS5_VABI fwrite_nid_postfix(const void*, std::size_t, std::size_t, FileStream*);
int APS5_VABI fclose_nid_postfix(FileStream*);
}

namespace {

constexpr int NotInitialized = static_cast<int>(0x809f0001u);
constexpr int Busy = static_cast<int>(0x809f0003u);
constexpr std::int32_t UserId = 7531;
const std::filesystem::path SavePath = "_sd/lifecycle/data.bin";
const std::filesystem::path MemoryPath = "_sd_mem/u7531/slot0.bin";
const std::vector<char> OriginalSave{'s', 'a', 'v', 'e', '\0', 'd', 'a', 't', 'a'};
const std::vector<char> OriginalMemory{'m', 'e', 'm', '\0', 'o', 'r', 'y'};

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

std::vector<char> Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    Require(file.is_open(), "saved file is missing");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void Write(const std::filesystem::path& path, const std::vector<char>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    Require(file.is_open(), "cannot create saved file");
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    Require(static_cast<bool>(file), "cannot seed saved file");
}

class TemporarySaveRoot {
public:
    TemporarySaveRoot() : previous(std::filesystem::current_path()),
        root(std::filesystem::temp_directory_path() /
            ("anyps5-savedata-lifecycle-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        Require(std::filesystem::create_directory(root), "cannot create test directory");
        std::filesystem::current_path(root);
        Write(SavePath, OriginalSave);
        Write(MemoryPath, OriginalMemory);
    }

    ~TemporarySaveRoot() {
        std::error_code error;
        std::filesystem::current_path(previous, error);
        std::filesystem::remove_all(root, error);
    }

private:
    std::filesystem::path previous;
    std::filesystem::path root;
};

SaveDataMountResult Mount() {
    SceSaveDataDirName name{};
    std::memcpy(name.data, "lifecycle", sizeof("lifecycle"));
    SaveDataMount3 mount{};
    mount.user_id = UserId;
    mount.dir_name = &name;
    mount.mount_mode = 2;
    SaveDataMountResult result{};
    Require(sceSaveDataMount3(&mount, &result) == 0, "existing save must mount");
    Require(result.mount_point.data[0] != '\0', "mount must expose a guest alias");
    return result;
}

void CheckAlias(const SaveDataMountResult& mount, const std::vector<char>& expected) {
    const auto path = std::string(mount.mount_point.data) + "/data.bin";
    auto* file = fopen_nid_postfix(path.c_str(), "rb");
    Require(file != nullptr, "mounted guest alias is missing");
    std::vector<char> bytes(expected.size() + 1);
    const auto count = fread_nid_postfix(bytes.data(), 1, bytes.size(), file);
    Require(fclose_nid_postfix(file) == 0, "mounted read must close");
    bytes.resize(count);
    Require(bytes == expected, "mounted guest alias no longer reads the saved bytes");
}

void WriteAlias(const SaveDataMountResult& mount, const std::vector<char>& bytes) {
    const auto path = std::string(mount.mount_point.data) + "/data.bin";
    auto* file = fopen_nid_postfix(path.c_str(), "wb");
    Require(file != nullptr, "mounted guest alias is not writable");
    const auto count = fwrite_nid_postfix(bytes.data(), 1, bytes.size(), file);
    Require(fclose_nid_postfix(file) == 0, "mounted write must close");
    Require(count == bytes.size(), "mounted guest alias lost a write");
    Require(Read(SavePath) == bytes, "mounted write did not update the actual save");
}

void CheckUninitialized(const std::vector<char>& expectedSave,
                        const std::vector<char>& expectedMemory) {
    SaveDataMemorySetup2 setup{};
    setup.user_id = UserId;
    setup.memory_size = expectedMemory.size() + 4;
    SaveDataMemorySetupResult result{};
    result.existed_memory_size = 321;
    Require(sceSaveDataSetupSaveDataMemory2(&setup, &result) == NotInitialized,
            "initial cleanup must not initialize memory setup");
    Require(result.existed_memory_size == 321, "failed pre-init setup changed its output");
    std::vector<char> bytes(expectedMemory.size(), 'x');
    SaveDataMemoryData data{bytes.data(), bytes.size(), 0};
    SaveDataMemoryGet2 get{};
    get.user_id = UserId;
    get.data = &data;
    Require(sceSaveDataGetSaveDataMemory2(&get) == NotInitialized,
            "initial cleanup must not initialize memory reads");
    Require(std::all_of(bytes.begin(), bytes.end(), [](char value) { return value == 'x'; }),
            "failed pre-init read changed its output");
    SaveDataMemorySet2 set{};
    set.user_id = UserId;
    set.data = &data;
    set.data_num = 1;
    Require(sceSaveDataSetSaveDataMemory2(&set) == NotInitialized,
            "initial cleanup must not initialize memory writes");
    Require(Read(SavePath) == expectedSave, "pre-init cleanup changed the mounted-save file");
    Require(Read(MemoryPath) == expectedMemory, "pre-init cleanup changed saved memory");
}

void CheckMemory(const std::vector<char>& expected) {
    std::vector<char> bytes(expected.size(), 'x');
    SaveDataMemoryData data{bytes.data(), bytes.size(), 0};
    SaveDataMemoryGet2 get{};
    get.user_id = UserId;
    get.data = &data;
    Require(sceSaveDataGetSaveDataMemory2(&get) == 0, "real initialization must enable memory reads");
    Require(bytes == expected, "real session lost the saved memory bytes");
}

void CheckRealSessions() {
    Require(sceSaveDataInitialize3(nullptr) == 0, "real initialization failed");
    SaveDataMemorySetup2 setup{};
    setup.user_id = UserId;
    setup.memory_size = OriginalMemory.size() + 3;
    SaveDataMemorySetupResult result{};
    Require(sceSaveDataSetupSaveDataMemory2(&setup, &result) == 0,
            "real session must set up saved memory");
    Require(result.existed_memory_size == OriginalMemory.size(),
            "real setup must detect the existing saved memory");
    auto expectedMemory = OriginalMemory;
    expectedMemory.resize(setup.memory_size, 0);
    CheckMemory(expectedMemory);
    char patch = 'z';
    SaveDataMemoryData data{&patch, 1, 1};
    SaveDataMemorySet2 set{};
    set.user_id = UserId;
    set.data = &data;
    set.data_num = 1;
    Require(sceSaveDataSetSaveDataMemory2(&set) == 0, "real session must write saved memory");
    expectedMemory[1] = patch;
    Require(Read(MemoryPath) == expectedMemory, "memory write must persist real bytes");
    CheckMemory(expectedMemory);

    const auto mounted = Mount();
    CheckAlias(mounted, OriginalSave);
    Require(sceSaveDataTerminate() == Busy, "last reference with a live mount must remain BUSY");
    CheckAlias(mounted, OriginalSave);
    CheckMemory(expectedMemory);
    Require(sceSaveDataInitialize3(nullptr) == 0, "second real initialization failed");
    Require(sceSaveDataTerminate() == 0, "releasing one of two references must succeed");
    CheckAlias(mounted, OriginalSave);
    CheckMemory(expectedMemory);
    Require(sceSaveDataTerminate() == Busy, "BUSY must preserve the last reference");
    auto expectedSave = OriginalSave;
    expectedSave[1] = 'Z';
    WriteAlias(mounted, expectedSave);
    CheckAlias(mounted, expectedSave);
    Require(sceSaveDataUmount2(0, &mounted.mount_point) == 0, "real save must unmount");
    Require(sceSaveDataTerminate() == 0, "balanced final termination failed");
    Require(sceSaveDataTerminate() == NotInitialized,
            "initial-cleanup option must not hide double termination after a real session");
    CheckUninitialized(expectedSave, expectedMemory);

    Require(sceSaveDataInitialize3(nullptr) == 0, "a later real session must initialize");
    CheckMemory(expectedMemory);
    const auto remounted = Mount();
    CheckAlias(remounted, expectedSave);
    Require(sceSaveDataUmount2(0, &remounted.mount_point) == 0, "later session must unmount");
    Require(sceSaveDataTerminate() == 0, "later real session must terminate");
    Require(sceSaveDataTerminate() == NotInitialized, "initial allowance must stay closed forever");
    CheckUninitialized(expectedSave, expectedMemory);
}

void CheckFirstInitClosesAllowance() {
    Require(sceSaveDataInitialize3(nullptr) == 0, "first real initialization failed");
    CheckMemory(OriginalMemory);
    Require(sceSaveDataTerminate() == 0, "first real session must terminate");
    Require(sceSaveDataTerminate() == NotInitialized,
            "first real init must close an unused initial-cleanup allowance");
    Require(sceSaveDataTerminate() == NotInitialized, "closed allowance must not reopen");
    CheckUninitialized(OriginalSave, OriginalMemory);
}

}

int main(int argc, char** argv) {
    try {
        Require(argc == 2, "expected one lifecycle mode");
        const std::string mode = argv[1];
        const auto* value = std::getenv("ANYPS5_SAVEDATA_INITIAL_CLEANUP");
        const bool enabled = value != nullptr && std::strcmp(value, "1") == 0;
        Require(mode == "--strict" || mode == "--initial-cleanup" ||
                mode == "--active-initial" || mode == "--init-first", "unknown lifecycle mode");
        Require(enabled == (mode != "--strict"), "lifecycle option does not match the test mode");
        const TemporarySaveRoot directory;
        if (mode == "--init-first") {
            CheckFirstInitClosesAllowance();
        } else {
            if (mode == "--active-initial") {
                const auto mounted = Mount();
                Require(sceSaveDataTerminate() == NotInitialized,
                        "active pre-init mount must prevent initial-cleanup success");
                CheckAlias(mounted, OriginalSave);
                CheckUninitialized(OriginalSave, OriginalMemory);
                Require(sceSaveDataUmount2(0, &mounted.mount_point) == 0,
                        "pre-init mount must still unmount after rejected cleanup");
            }
            Require(sceSaveDataTerminate() == (enabled ? 0 : NotInitialized),
                    "first empty terminate must respect the exact opt-in value");
            Require(sceSaveDataTerminate() == NotInitialized,
                    "only one initial empty terminate may be tolerated");
            CheckUninitialized(OriginalSave, OriginalMemory);
            CheckRealSessions();
        }
        std::printf("Save-data lifecycle checks passed: %s\n", mode.c_str());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Save-data lifecycle check failed: %s\n", error.what());
        return 1;
    }
}
