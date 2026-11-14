// SPDX-License-Identifier: GPL-3.0-only
#include "../src/runtime.cpp"
#include <iostream>
#include <stdexcept>
#include <vector>
#include <fstream>

using namespace ach;
namespace {
std::atomic<bool> requested = true, canStop = true, startSucceeds = true, pressed = false, front = true;
std::atomic<int> starts = 0, stops = 0;
bool requestedState() { return requested; }
bool start() { ++starts; return startSucceeds; }
void stop() { ++stops; }
bool guard() { return canStop; }
void logLine(FH3Mod*, const char*) {}
SHORT WINAPI simulatedKey(int key) { return key == VK_F9 && pressed ? static_cast<SHORT>(0x8000) : 0; }
HWND WINAPI simulatedForeground() { return reinterpret_cast<HWND>(1); }
DWORD WINAPI simulatedPid(HWND, LPDWORD pid) { *pid = front ? GetCurrentProcessId() : GetCurrentProcessId() + 1; return 1; }
void require(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
template<class T> void put(uint8_t* base, size_t offset, T value) { std::memcpy(base + offset, &value, sizeof value); }
void* expectedModel{};
int notifications{};
void notify(void* model, void*) {
    require(model == expectedModel, "notice used a discarded binding model");
    ++notifications;
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: runtime-test <new test directory>");
        const auto root = std::filesystem::absolute(argv[1]);
        require(!std::filesystem::exists(root), "test directory must be new");
        std::filesystem::create_directories(root / "fh3-mods");
        std::ofstream(root / "fh3-mods/platform.ini") << "[dev]\nHotReload=1\nReloadKey=F9\n";
        const auto directory = root.wstring();
        FH3ModApi api{};
        api.azimuth_dir = directory.c_str();
        api.log = logLine;
        keyState = simulatedKey; foreground = simulatedForeground; windowPid = simulatedPid;
        auto* mod = reinterpret_cast<FH3Mod*>(0x1234);
        startRuntimeControl(&api, mod, true, {requestedState, start, stop, guard});
        Sleep(60);
        for (int cycle = 0; cycle < 3; ++cycle) {
            requested = false; front = false; pressed = true; Sleep(70);
            require(runtimeLoadState() == 1, "background F9 toggled the mod");
            pressed = false; Sleep(60); front = true; pressed = true;
            const auto until = GetTickCount64() + 1500;
            while (runtimeLoadState() && GetTickCount64() < until) Sleep(5);
            require(runtimeLoadState() == 0 && appliedNotice == 0, "foreground F9 did not stop the mod");
            const auto calls = stops.load(); Sleep(70);
            require(stops == calls, "held F9 repeated shutdown");
            pressed = false; Sleep(60);
            requested = true; require(applyRuntimeLoad() && runtimeLoadState() == 1, "runtime On failed");
        }
        requested = false; canStop = false;
        require(!applyRuntimeLoad() && runtimeLoadState() == 1 && appliedNotice == 1, "journal guard cleared a pending change");
        canStop = true; require(applyRuntimeLoad(), "guard recovery failed");
        requested = true; startSucceeds = false;
        require(!applyRuntimeLoad() && runtimeLoadState() == 0 && appliedNotice == 0, "failed start advanced the notice baseline");
        startSucceeds = true; require(applyRuntimeLoad(), "start retry failed");
        stopRuntimeControl();
        require(!listener.joinable() && !applyRuntimeLoad(), "listener did not stop for a DLL reload");

        // Validate the stock 1.4.2 row lookup and native-notice update against
        // multiple deque blocks, non-first mod identity and changing UI models.
        std::vector<uint8_t> memory(0x26b000), entries(0xb0 * 3), first(0xd0 * 19), second(0xd0 * 19);
        uint8_t* blocks[]{first.data(), second.data()};
        auto* base = memory.data();
        put(entries.data(), 0xb0 * 2 + 0xa0, mod);
        put(base, 0x241da0, entries.data()); put(base, 0x241da8, entries.data() + entries.size());
        put(base, 0x241248 + 8, blocks); put(base, 0x241248 + 16, blocks + 2);
        put(base, 0x241248 + 32, size_t{2}); put(base, 0x241248 + 40, size_t{20});
        auto* row = second.data(); const char key[] = "Mod_2";
        row[8] = 10; std::memcpy(row + 9, key, sizeof key);
        put(row, 0x78, reinterpret_cast<void*>(9)); put(row, 0x80, uintptr_t{2});
        put(row, 0x68, 1); put(row, 0x6c, 1); put(first.data(), 0x6c, 77);
        auto* page = reinterpret_cast<void*>(10);
        put(base, 0x2362c0, page); put(base, 0x2418a0, uint8_t{1});
        put(base, 0x235b10, reinterpret_cast<void*>(42));
        require(loadRow(base, mod) == row, "notice did not resolve Achievements by its own mod handle");
        for (const int desired : {0, 1, 0, 1, 0, 1}) {
            put(row, 0x68, desired);
            expectedModel = reinterpret_cast<void*>(uintptr_t{100} + notifications);
            put(base, 0x234e00, expectedModel);
            updateNotice(base, mod, page, 1 - desired, notify);
            require(read<int>(row + 0x6c) != desired, "unapplied change lost its notice");
            updateNotice(base, mod, page, desired, notify);
            require(read<int>(row + 0x6c) == desired, "successful F9 did not clear the notice");
            require(read<int>(first.data() + 0x6c) == 77, "notice changed another mod's baseline");
        }
        require(notifications == 6, "notice did not refresh after every successful toggle");
        put(row, 0x68, 0); updateNotice(base, mod, page, -1, notify);
        require(read<int>(row + 0x6c) == 1, "HotReload=0 changed the restart baseline");
        std::cout << "PASS: stock-platform F9 listener, held/background keys, repeated toggles, failure/journal guards, own-row notice lifecycle and fresh UI bindings.\n";
    } catch (const std::exception& error) {
        stopRuntimeControl();
        std::cerr << error.what() << '\n'; return 1;
    }
}
