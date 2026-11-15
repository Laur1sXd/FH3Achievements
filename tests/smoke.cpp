// SPDX-License-Identifier: GPL-3.0-only
#include "events.hpp"
#include "journal.hpp"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <set>
#include <mutex>
#include <stdexcept>

namespace {
using namespace ach;
std::wstring installPath;
std::vector<uint8_t> image;
std::atomic<void*> installedDetour = nullptr;
std::atomic<void*> installedTile = nullptr;
std::atomic<void*> installedFocus = nullptr, installedScene = nullptr, installedUpdate = nullptr;
unsigned inheritedUpdates = 0, nativeHudUpdates = 0;
std::string capturedPage;
std::mutex hookMutex;
std::set<FH3Hook*> activeHooks;
std::map<FH3Hook*, void*> hookCallbacks;
unsigned hookCreations = 0;
std::atomic<bool> codeAvailable = true;
void stockFocus(void*) {}
void stockUpdate(void*) { ++nativeHudUpdates; }
void inheritedUpdate(void*) { ++inheritedUpdates; }
uint8_t* stockScene(uint8_t* record) { return record; }
void transition(void*, const void*, int) {}
void stub(uintptr_t rva, const void* target) {
    uint8_t bytes[]{0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xe0};
    std::memcpy(bytes+2,&target,sizeof(target));
    DWORD old{};
    if (!VirtualProtect(image.data()+rva,sizeof(bytes),PAGE_EXECUTE_READWRITE,&old)) throw std::runtime_error("fixture code protection failed");
    std::memcpy(image.data()+rva,bytes,sizeof(bytes));
    FlushInstructionCache(GetCurrentProcess(),image.data()+rva,sizeof(bytes));
}

FH3Mod* registerMod(const FH3ModInfo* info) {
    if (!info || !info->check_updates || !info->github_repo ||
        std::strcmp(info->github_repo, "Laur1sXd/FH3Achievements") != 0) return nullptr;
    return reinterpret_cast<FH3Mod*>(1);
}
void logLine(FH3Mod*, const char* line) { std::cout << line << std::endl; }
void* gameBase() { return image.data(); }
void* waitForCode(uint32_t timeout) {
    if (codeAvailable) return gameBase();
    Sleep(timeout);
    return nullptr;
}
int addSetting(FH3Mod*, const FH3Setting*) { return 1; }
uint32_t original(const void*, const void*, uint64_t, uint32_t, const void*) { return 42; }
FH3Hook* remember(uintptr_t id, void* callback) {
    std::lock_guard lock(hookMutex);
    auto* hook = reinterpret_cast<FH3Hook*>(id);
    hookCallbacks[hook] = callback;
    ++hookCreations;
    return hook;
}
FH3Hook* hookPointer(FH3Mod*, const char*, void** slot, void* callback, void** next) {
    auto* ptr = reinterpret_cast<uint8_t*>(slot);
    if (ptr == image.data() + 0x4566590 + 148 * sizeof(void*)) { *next=reinterpret_cast<void*>(stockFocus);installedFocus=callback;return remember(3, callback); }
    if (ptr == image.data() + 0x4566590 + 179 * sizeof(void*)) return remember(4, callback);
    if (ptr == image.data() + 0x4566590 + 175 * sizeof(void*)) { *next=reinterpret_cast<void*>(stockUpdate);installedUpdate=callback;return remember(10, callback); }
    if (ptr != image.data() + 0x2cf01a0) return nullptr;
    *next = reinterpret_cast<void*>(original);
    installedDetour = callback;
    return remember(1, callback);
}
int enable(FH3Mod*, FH3Hook* hook) {
    std::lock_guard lock(hookMutex);
    activeHooks.insert(hook);
    if (hook == reinterpret_cast<FH3Hook*>(1)) installedDetour = hookCallbacks[hook];
    if (hook == reinterpret_cast<FH3Hook*>(2)) installedTile = hookCallbacks[hook];
    if (hook == reinterpret_cast<FH3Hook*>(3)) installedFocus = hookCallbacks[hook];
    if (hook == reinterpret_cast<FH3Hook*>(6)) installedScene = hookCallbacks[hook];
    if (hook == reinterpret_cast<FH3Hook*>(10)) installedUpdate = hookCallbacks[hook];
    return 1;
}
void stockTile(void*) {}
FH3Hook* hookFunction(FH3Mod*, const char*, void* target, void* callback, void** next) {
    if (target == image.data() + 0xb26690) return remember(7, callback);
    if (target == image.data() + 0xadb350) return remember(8, callback);
    if (target == image.data() + 0xadc940) return remember(9, callback);
    if (target == image.data() + 0x200c1d0) return remember(5, callback);
    if (target == image.data() + 0x1182cb0) { *next=reinterpret_cast<void*>(stockScene);installedScene=callback;return remember(6, callback); }
    if (target != image.data() + 0x1579ab0) return nullptr;
    *next = reinterpret_cast<void*>(stockTile);
    installedTile = callback;
    return remember(2, callback);
}
int disable(FH3Mod*, FH3Hook* hook) {
    std::lock_guard lock(hookMutex);
    activeHooks.erase(hook);
    if (hook == reinterpret_cast<FH3Hook*>(2)) installedTile = nullptr;
    if (hook == reinterpret_cast<FH3Hook*>(1)) installedDetour = nullptr;
    if (hook == reinterpret_cast<FH3Hook*>(3)) installedFocus = nullptr;
    if (hook == reinterpret_cast<FH3Hook*>(6)) installedScene = nullptr;
    if (hook == reinterpret_cast<FH3Hook*>(10)) installedUpdate = nullptr;
    return 1;
}
int setFile(FH3Mod*, const char* path, const void* content, size_t length) {
    if (std::string_view(path) == JournalFile) capturedPage.assign(static_cast<const char*>(content), length);
    // Capture the scene; the transition is stubbed in this private code image.
    return 1;
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string contents(const fs::path& file) {
    if (!fs::exists(file)) return {};
    const auto data = readFile(file, 32 * 1024);
    return {data.begin(), data.end()};
}

bool waitFor(auto condition, unsigned ms = 10000) {
    const auto end = GetTickCount64() + ms;
    while (GetTickCount64() < end) {
        if (condition()) return true;
        Sleep(50);
    }
    return false;
}

struct LoadedMod {
    using Init = int (*)(const FH3ModApi*);
    using Shutdown = void (*)();
    HMODULE dll;
    Shutdown shutdown;
    using Capability = int (*)();
    Capability canShutdown;
    Capability apply, state;
    bool active = true;
    explicit LoadedMod(const fs::path& path, const FH3ModApi& api) {
        // The real platform retains old modules and loads a fresh copy per F9.
        static unsigned generation = 0;
        const auto copy = fs::path(installPath) / L"fh3-mods" / L".reload" /
            (L"FH3Achievements." + std::to_wstring(++generation) + L".dll");
        fs::create_directories(copy.parent_path());
        fs::copy_file(path, copy);
        dll = LoadLibraryW(copy.c_str());
        require(dll != nullptr, "DLL failed to load");
        auto init = reinterpret_cast<Init>(GetProcAddress(dll, "fh3mod_init"));
        shutdown = reinterpret_cast<Shutdown>(GetProcAddress(dll, "fh3mod_shutdown"));
        auto runtime = reinterpret_cast<Capability>(GetProcAddress(dll, "fh3mod_runtime_toggle"));
        canShutdown = reinterpret_cast<Capability>(GetProcAddress(dll, "fh3mod_can_shutdown"));
        apply = reinterpret_cast<Capability>(GetProcAddress(dll, "fh3mod_apply_runtime_load"));
        state = reinterpret_cast<Capability>(GetProcAddress(dll, "fh3mod_runtime_load_state"));
        require(apply && state, "stock-platform runtime controller exports missing");
        require(runtime && runtime() == 1 && canShutdown, "runtime Load/F9 exports missing");
        require(init && shutdown && init(&api) == 0, "platform initialization failed");
    }
    void stop() {
        if (!active) return;
        require(canShutdown() == 1, "journal must be closed before runtime disable");
        shutdown();
        active = false;
        std::lock_guard lock(hookMutex);
        require(activeHooks.empty(), "runtime disable left active achievement hooks");
        const auto file = fs::path(installPath) / L"fh3-mods" / L"FH3Achievements" / L"session.lock";
        const auto lease = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, 0, nullptr);
        require(lease != INVALID_HANDLE_VALUE, "runtime disable did not release the worker's journal lock");
        CloseHandle(lease);
    }
    ~LoadedMod() {
        if (active) shutdown();
        // Intentionally keep code mapped, just like the platform's F9 loader.
    }
};
}

int wmain(int argc, wchar_t** argv) {
    try {
        require(argc == 5 || argc == 6, "usage: smoke <mod dll> <game exe> <existing player-1 save root> <new test folder> [mediaplatform|mediaoverride]");
        const auto dll = fs::absolute(argv[1]);
        const auto fixture = fs::absolute(argv[4]);
        require(!fs::exists(fixture), "test folder must be new");
        installPath = fixture.wstring();
        image = readGameImage(argv[2]);
        stub(0x1689f60,reinterpret_cast<void*>(transition));
        stub(0x14a1b70,reinterpret_cast<void*>(inheritedUpdate));
        const auto input = latestRevision(argv[3]);
        require(input.has_value(), "progressed save fixture missing");
        const auto empty = initialProgress();
        require(std::ranges::none_of(empty, [](const auto& p) { return p.current || p.complete(); }),
                "fresh career has earned progress");
        const auto sample = readSave(image.data(), *input);
        const auto expected = evaluate(sample);
        int importedCount = 0, importedScore = 0;
        for (const auto& item : Catalog) {
            if (item.online || !expected[item.id].complete()) continue;
            ++importedCount;
            importedScore += item.gamerscore;
        }
        require(importedCount > 0, "fixture must be a progressed save");
        std::ostringstream expectedHeader;
        expectedHeader << "FH3ACH3 " << std::hex << input->number << std::dec << ' '
                       << importedCount << ' ' << importedScore << '\n';
        const auto copyRoot = fixture / L"data" / L"titles" / L"4CE1DFAB" / L"saves" / L"player-1";
        const auto copied = copyRoot / input->profile / L"revisions" / input->commit.parent_path().filename();
        const auto stateDir = fixture / L"fh3-mods" / L"FH3Achievements";
        const auto record = stateDir / (input->profile + L".txt");
        const std::wstring overlay = argc == 6 ? argv[5] : L"mediaoverride";
        require(overlay == L"mediaoverride" || overlay == L"mediaplatform", "unknown overlay mode");
        const auto stateflow = fixture / overlay / L"stateflow";
        fs::create_directories(stateflow);
        std::ofstream(stateflow / L"pause.xml") << "<node id=\"online_party_list\" state=\"ui_screens.show_hud_options\">";

        FH3ModApi api{};
        api.api_version = 16;
        api.azimuth_dir = installPath.c_str();
        api.game_dir = installPath.c_str();
        api.register_mod = registerMod;
        api.log = logLine;
        api.game_base = gameBase;
        api.wait_for_code = waitForCode;
        api.add_setting = addSetting;
        api.set_file = setFile;
        api.hook_pointer = hookPointer;
        api.hook_function = hookFunction;
        api.enable_hooks = enable;
        api.disable_hooks = disable;
        const auto disabled = fixture / L"fh3-mods" / L"platform-disabled.txt";
        fs::create_directories(disabled.parent_path());
        std::ofstream(disabled, std::ios::binary) << "\xef\xbb\xbf# Cold-start disabled fixture\r\n  FH3aCHIEVEMENTS.DLL\t\r\nOtherMod.dll\r\n";
        {
            LoadedMod off(dll, api);
            Sleep(400);
            require(!installedDetour && !installedTile, "cold OFF started tracking or journal hooks");
            require(!fs::exists(record), "cold OFF wrote achievement progress");
            require(!capturedPage.empty(), "cold OFF must register its startup scene for later F9 activation");
            off.stop();
        }
        std::ofstream(disabled) << "# Unrelated disabled mod must not affect Achievements\nOtherMod.dll\n";
        std::string firstRun;
        auto** initialHudSlots = reinterpret_cast<void**>(image.data() + 0x4566590);
        initialHudSlots[148] = image.data() + 0x13a19a0;
        initialHudSlots[179] = image.data() + 0x14981a0;
        {
            LoadedMod running(dll, api);
            require(!capturedPage.empty(), "journal scene must register during initialization, before FH3 mounts files");
            require(waitFor([] { return installedDetour.load() != nullptr; }), "event hook setup timed out");
            Sleep(400);
            require(installedTile.load() == nullptr, "journal must wait while native MODS route is uninitialized");
            // Model a slow Mod Platform database/UI build completing later.
            auto** hudSlots = reinterpret_cast<void**>(image.data() + 0x4566590);
            hudSlots[148] = reinterpret_cast<void*>(stockFocus);
            hudSlots[179] = reinterpret_cast<void*>(stockTile);
            require(waitFor([] { return installedTile.load() != nullptr; }), "original tile binding timed out");
            auto openTile = reinterpret_cast<void (*)(void*)>(installedTile.load());
            openTile(nullptr);
            // Exercise the real scene redirect and page update detours. The
            // journal must use the inherited menu update; a normal HUD page
            // must still reach its original native HUD editor update.
            wchar_t scenePath[20]=L"Anthem\\A_HudOptions";
            std::array<uint8_t,0x48> sceneRecord{};
            *reinterpret_cast<wchar_t**>(sceneRecord.data()+0x28)=scenePath;
            *reinterpret_cast<uint64_t*>(sceneRecord.data()+0x38)=19;
            *reinterpret_cast<uint64_t*>(sceneRecord.data()+0x40)=19;
            require(installedScene&&installedFocus&&installedUpdate,"journal lifecycle hooks missing");
            reinterpret_cast<uint8_t* (*)(uint8_t*)>(installedScene.load())(sceneRecord.data());
            require(std::wstring(scenePath)==L"Anthem\\A_AchJournal","journal scene redirect failed");
            void* journal=reinterpret_cast<void*>(0x1234);
            reinterpret_cast<void (*)(void*)>(installedFocus.load())(journal);
            require(running.canShutdown() == 0, "runtime disable must be refused while the journal is open");
            reinterpret_cast<void (*)(void*)>(installedUpdate.load())(journal);
            reinterpret_cast<void (*)(void*)>(installedUpdate.load())(reinterpret_cast<void*>(0x5678));
            require(inheritedUpdates==1&&nativeHudUpdates==1,"journal/HUD frame updates were not isolated");
            reinterpret_cast<void (*)(void*)>(installedFocus.load())(reinterpret_cast<void*>(0x5678));
            require(running.canShutdown() == 1, "journal close must allow runtime disable");
            require(capturedPage.find("0 / 135     0 / 3000G") != std::string::npos,
                    "fresh journal did not start at zero");
            require(capturedPage.find("A_Double_HUDSafeFrameHorizontalMax") != std::string::npos &&
                    capturedPage.find("A_Double_HUDSafeFrameVerticalMax") != std::string::npos,
                    "the HudOptions page requires both safe frame limits");
            std::ofstream(fixture / L"fresh.xaml", std::ios::binary) << capturedPage;
            Sleep(2200);
            require(!fs::exists(record), "fresh slot got an achievement ledger");
            fs::create_directories(copied.parent_path());
            fs::copy(input->commit.parent_path(), copied, fs::copy_options::recursive);
            require(waitFor([&] { return fs::exists(record); }), "backfill did not write the local record");
            firstRun = contents(record);
            require(firstRun.starts_with(expectedHeader.str()), "the save import summary was not queued");
            for (const auto& item : Catalog)
                if (expected[item.id].complete())
                    require(firstRun.find("\n" + std::to_string(item.id) + " ") != std::string::npos,
                            "a save-backed unlock is missing");
            require(waitFor([&] {
                openTile(nullptr);
                return capturedPage.find("UNLOCKED 20") != std::string::npos;
            }), "the native page did not pick up the save-backed unlocks");
            std::ofstream(fixture / L"backfill.xaml", std::ios::binary) << capturedPage;

            using EventWrite = uint32_t (*)(const void*, const void*, uint64_t, uint32_t, const void*);
            const auto detour = reinterpret_cast<EventWrite>(installedDetour.load());
            for (int n = 0; n < 5; ++n)
                require(detour(image.data() + 0x4463fc8, nullptr, 0, 5, nullptr) == 42,
                        "ETW detour did not return the original result");
            require(waitFor([&] { return contents(record).find("\n32 ") != std::string::npos; }),
                    "Skill Song events did not persist");
            firstRun = contents(record);
            require(firstRun.find("\n32 0 ") == std::string::npos, "five Skill Songs did not unlock");
            // Queue one live event immediately before Off/F9, without waiting
            // for the next save scan. Shutdown must persist it exactly once.
            detour(image.data() + 0x4462668, nullptr, 0, 10, nullptr);
            reinterpret_cast<void (*)(void*)>(installedFocus.load())(reinterpret_cast<void*>(0x5678));
            std::ofstream(disabled) << "FH3Achievements.dll\n";
            running.stop();
            firstRun = contents(record);
            require(firstRun.find("\n26 0 1\n") != std::string::npos,
                    "runtime shutdown lost the last live convoy event");
            detour(image.data() + 0x4462668, nullptr, 0, 10, nullptr);
            Sleep(100);
            require(contents(record) == firstRun, "an old callback changed achievements while OFF");
        }
        // On/F9 must work without a DLL file change, with the old image still
        // mapped. Repeated cycles cannot duplicate counters or create hooks.
        for (int cycle = 0; cycle < 3; ++cycle) {
            std::ofstream(disabled) << "OtherMod.dll\n";
            LoadedMod running(dll, api);
            require(waitFor([] { return installedDetour && installedTile; }), "On/F9 did not restore hooks");
            Sleep(150);
            std::ofstream(disabled) << "FH3Achievements.dll\n";
            running.stop();
            require(contents(record) == firstRun, "Off/On duplicated or lost achievement progress");
        }
        {
            LoadedMod off(dll, api);
            Sleep(300);
            off.stop();
            require(contents(record) == firstRun, "disabled initialization changed an existing career");
        }
        fs::remove(disabled);
        // F9 may arrive while the game's code is still becoming available.
        // Shutdown must interrupt that wait rather than exceed the loader's
        // eight-second shutdown deadline.
        codeAvailable = false;
        {
            LoadedMod waiting(dll, api);
            Sleep(100);
            const auto before = GetTickCount64();
            waiting.stop();
            require(GetTickCount64() - before < 1500, "shutdown blocked on game-code readiness");
        }
        codeAvailable = true;
        {
            LoadedMod running(dll, api);
            require(waitFor([] { return installedDetour.load() != nullptr; }), "reload hook timed out");
            Sleep(2300);
            require(contents(record) == firstRun, "reloading duplicated or lost an unlock");
        }
        // An existing 1.0.0 ledger gets the new notice once. Its live-only
        // Skill Song unlock stays in the journal and out of the save-import count.
        std::ofstream(record, std::ios::binary) << "FH3ACH2 " << std::hex << input->number
                                               << firstRun.substr(firstRun.find('\n'));
        {
            LoadedMod running(dll, api);
            require(waitFor([&] { return contents(record) == firstRun; }),
                    "migrating the previous ledger changed unlocks or the import summary");
        }
        require(readFile(input->commit, 65536) == readFile(copied / L"commit", 65536),
                "the game's commit changed");
        require(readFile(input->commit.parent_path() / L"data" / L"ProfileData", 16 << 20) ==
                readFile(copied / L"data" / L"ProfileData", 16 << 20), "the game's save changed");
        const auto config = fixture / L"fh3-mods" / L"platform.ini";
        std::ofstream(config) << "[dev]\nHotReload=1\nReloadKey=F9\n";
        {
            LoadedMod resident(dll, api);
            require(waitFor([] { return installedDetour && installedTile; }), "resident startup hooks missing");
            const auto definitions = hookCreations;
            const auto ledger = contents(record);
            for (int cycle = 0; cycle < 3; ++cycle) {
                std::ofstream(disabled) << "FH3Achievements.dll\n";
                require(resident.apply() == 1 && resident.state() == 0, "stock-platform Off/F9 failed");
                {
                    std::lock_guard lock(hookMutex);
                    require(activeHooks == std::set<FH3Hook*>{reinterpret_cast<FH3Hook*>(8),
                                reinterpret_cast<FH3Hook*>(9), reinterpret_cast<FH3Hook*>(10)},
                            "Off must retain only notice refresh and safe HUD lifetime watches");
                }
                require(!installedDetour && !installedTile, "Off left tracking or the journal tile active");
                require(contents(record) == ledger, "Off changed earned achievements");
                std::ofstream(disabled) << "OtherMod.dll\n";
                require(resident.apply() == 1 && resident.state() == 1, "stock-platform On/F9 failed");
                require(waitFor([] { return installedDetour && installedTile; }), "On did not restore cached hooks");
                require(hookCreations == definitions, "re-enabling created duplicate hooks");
            }
            resident.stop();
            require(contents(record) == ledger, "same-DLL Off/On changed achievement records");
        }
        std::ofstream(disabled) << "FH3Achievements.dll\n";
        {
            LoadedMod residentOff(dll, api);
            require(residentOff.state() == 0 && !installedDetour && !installedTile,
                    "refreshing an Off DLL started tracking");
            std::ofstream(disabled) << "OtherMod.dll\n";
            require(residentOff.apply() == 1, "an Off refreshed DLL could not activate");
            require(waitFor([] { return installedDetour && installedTile; }),
                    "refreshed Off DLL did not restore the journal/tracking hooks");
            residentOff.stop();
        }
        std::ofstream(config) << "[dev]\nHotReload=0\n";
        {
            LoadedMod noHotReload(dll, api);
            require(waitFor([] { return installedDetour && installedTile; }), "HotReload=0 startup failed");
            std::ofstream(disabled) << "FH3Achievements.dll\n";
            require(noHotReload.apply() == 0 && noHotReload.state() == 1,
                    "HotReload=0 must retain restart-based Load behavior");
            noHotReload.stop();
        }
        fs::remove(disabled);
        std::cout << "PASS: save integrity/backfill, ledger migration, live events, shutdown flush, journal guard, retained-DLL reload, stock-platform same-DLL Off/On with no duplicate hooks and HotReload=0.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
