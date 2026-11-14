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
int rowNotifications{}, persisted{}, forwardedSets{}, forwardedButtons{};
bool saveFails{};
bool f9DuringAcceptRejected = true;
const uint8_t* fixtureBase{};
void notify(void* model, void*) {
    require(model == expectedModel, "notice used a discarded binding model");
    ++notifications;
}
void notifyPreview(void* model, void* descriptor) {
    require(model == expectedModel, "preview refresh used a stale native UI model");
    if (descriptor == reinterpret_cast<void*>(43)) ++rowNotifications;
    else ++notifications;
}
void persistLoad(void*, const char*, int value) {
    ++persisted;
    f9DuringAcceptRejected = f9DuringAcceptRejected && !applyRuntimeLoad();
    if (!saveFails) requested = value != 0;
}
void passthroughSet(void*, void*, void*) { ++forwardedSets; }
void selectionSet(void*, void*, void* value) {
    int selected{}; std::memcpy(&selected, static_cast<uint8_t*>(value) + 0x28, sizeof selected);
    std::memcpy(const_cast<uint8_t*>(fixtureBase) + 0x2362a8, &selected, sizeof selected);
}
bool passthroughButtons(void*, int, bool) { ++forwardedButtons; return false; }
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
            front = true; Sleep(70);
            require(runtimeLoadState() == 1, "a held background key toggled the mod on focus return");
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

        // Exercise the production row/button detours: no persistence callback
        // until pressed A, cancellation on B/row/page exit, and F9 must read the
        // accepted value rather than a currently displayed preview.
        startRuntimeControl(&api, mod, true, {requestedState, start, stop, guard});
        noticePlatform = base; nativeNotify = notifyPreview; fixtureBase = base;
        previewReady = true;
        originalLoadSet = passthroughSet; originalSelectionSet = selectionSet; originalButtons = passthroughButtons;
        put(base, 0x2412a8, 3); put(base, 0x2362a8, 0);
        put(row, 0x78, &persistLoad); put(row, 0x88, reinterpret_cast<void*>(43));
        put(row, 0x68, 1); put(row, 0x6c, 1);
        expectedModel = reinterpret_cast<void*>(500); put(base, 0x234e00, expectedModel);
        requested = true; active = true; appliedNotice = 1;
        std::array<uint8_t, 64> off{}, on{}, next{};
        put(on.data(), 0x28, 1); put(next.data(), 0x28, 1);
        auto* descriptor = reinterpret_cast<void*>(43);
        const auto pending = [&] { return read<int>(row + 0x68) != read<int>(row + 0x6c); };
        onLoadSet(descriptor, nullptr, off.data());
        require(read<int>(row + 0x68) == 0 && requested && persisted == 0 && pending(), "selector preview must show its notice without persisting before A");
        require(applyRuntimeLoad() && runtimeLoadState() == 1, "F9 applied an unaccepted Off preview");
        onLoadButtons(page, 999999, true);
        require(!preview.row && read<int>(row + 0x68) == 1 && requested && !pending(), "B did not restore the accepted value");
        onLoadSet(descriptor, nullptr, off.data());
        onLoadSelection(nullptr, nullptr, next.data());
        require(!preview.row && read<int>(row + 0x68) == 1 && !pending(), "leaving the row did not discard its preview");
        put(base, 0x2362a8, 0);
        onLoadSet(descriptor, nullptr, off.data());
        put(base, 0x2418a0, uint8_t{0});
        updateNotice(base, mod, page, appliedNotice, nativeNotify);
        require(!preview.row && read<int>(row + 0x68) == 1 && requested, "leaving the page did not discard its preview");
        put(base, 0x2418a0, uint8_t{1});
        onLoadSet(descriptor, nullptr, off.data());
        onLoadButtons(page, 1000000, false);
        require(requested && persisted == 0, "A release accepted a preview");
        const int forwardsBeforeAccept = forwardedButtons;
        require(onLoadButtons(page, 1000000, true), "pressed A was not consumed for Load acceptance");
        onLoadButtons(page, 1000000, false);
        require(forwardedButtons == forwardsBeforeAccept, "accepted Load button events reached an unrelated native action");
        require(!requested && persisted == 1 && pending() && !preview.row && runtimeLoadState() == 1,
                "A did not accept exactly one pending value without applying it");
        // An accepted Off is pending; previewing On must not hide that pending
        // state. F9 applies the accepted Off and leaves the On preview unsaved.
        onLoadSet(descriptor, nullptr, on.data());
        require(pending() && persisted == 1, "opposite preview erased the accepted pending request");
        require(applyRuntimeLoad() && runtimeLoadState() == 0, "F9 did not apply the accepted Off value");
        updateNotice(base, mod, page, appliedNotice, nativeNotify);
        require(pending() && read<int>(row + 0x68) == 1 && !requested, "F9 must preserve an unsaved opposite preview notice without persisting it");
        onLoadButtons(page, 999999, true);
        require(read<int>(row + 0x68) == 0 && !pending(), "B did not restore accepted Off after F9");
        onLoadSet(descriptor, nullptr, on.data());
        require(pending(), "unaccepted On preview did not show its notice");
        onLoadButtons(page, 1000000, true); onLoadButtons(page, 1000000, false);
        require(requested && pending() && persisted == 2, "On acceptance did not become pending");
        require(applyRuntimeLoad() && runtimeLoadState() == 1, "F9 did not apply accepted On");
        updateNotice(base, mod, page, appliedNotice, nativeNotify);
        require(!pending(), "On application did not clear the pending notice");
        saveFails = true;
        onLoadSet(descriptor, nullptr, off.data());
        onLoadButtons(page, 1000000, true); onLoadButtons(page, 1000000, false);
        require(preview.row && requested && pending(), "failed acceptance lost the preview notice or replaced the saved request");
        onLoadButtons(page, 999999, true); saveFails = false;
        const auto oldForwards = forwardedSets;
        onLoadSet(reinterpret_cast<void*>(99), nullptr, off.data());
        require(forwardedSets == oldForwards + 1 && read<int>(first.data() + 0x6c) == 77, "preview gate intercepted another row");
        require(rowNotifications > 0, "cancellation did not refresh the selector");
        require(f9DuringAcceptRejected, "F9 read a partially accepted request");
        stopRuntimeControl();
        appliedNotice = -1; put(row, 0x68, 1); put(row, 0x6c, 1); requested = true;
        onLoadSet(descriptor, nullptr, off.data());
        require(pending(), "HotReload=0 preview did not show its notice");
        onLoadButtons(page, 1000000, true); onLoadButtons(page, 1000000, false);
        require(!requested && pending() && !applyRuntimeLoad(), "HotReload=0 lost acceptance or restart semantics");
        stopRuntimePreview();

        // Exact user flow in both directions, changing the native binding
        // model on every mod/list reopen. F9 is applied only after free roam.
        requested = true;
        startRuntimeControl(&api, mod, true, {requestedState, start, stop, guard});
        noticePlatform = base; nativeNotify = notifyPreview; previewReady = true;
        put(row, 0x68, 1); put(row, 0x6c, 1);
        uintptr_t modelId = 700;
        const auto enter = [&](int view) {
            put(base, 0x2418a0, uint8_t{1}); put(base, 0x2412a8, view); put(base, 0x2362a8, 0);
            expectedModel = reinterpret_cast<void*>(++modelId); put(base, 0x234e00, expectedModel);
            updateNotice(base, mod, page, appliedNotice, nativeNotify);
        };
        for (const int desired : {0, 1}) {
            const int acceptedBefore = requested ? 1 : 0;
            std::array<uint8_t, 64> value{}; put(value.data(), 0x28, desired);
            enter(3); onLoadSet(descriptor, nullptr, value.data());
            require(pending(), "preview did not show the notice on the mod page");
            onLoadButtons(page, 999999, true); enter(0); enter(3);
            require(read<int>(row + 0x68) == acceptedBefore && !pending() && requested == (acceptedBefore != 0),
                    "Back/list/mod round trip without A did not restore the accepted value and clear its preview notice");
            onLoadSet(descriptor, nullptr, value.data());
            onLoadButtons(page, 1000000, true); onLoadButtons(page, 1000000, false);
            require(pending(), "A did not retain the accepted pending notice on the mod page");
            onLoadButtons(page, 999999, true); enter(0);
            require(pending(), "accepted pending notice disappeared in the MODS list");
            enter(3); require(pending(), "accepted pending notice disappeared when reopening the mod");
            onLoadButtons(page, 999999, true); enter(0);
            put(base, 0x2418a0, uint8_t{0});
            require(applyRuntimeLoad() && runtimeLoadState() == desired, "F9 in free roam did not apply the accepted value");
            enter(0); require(!pending(), "notice remained in the MODS list after successful F9");
            enter(3); require(!pending() && read<int>(row + 0x68) == desired, "reopened mod did not show the applied value without a notice");
        }
        stopRuntimeControl(); stopRuntimePreview();
        std::cout << "PASS: F9 listener, A-only persistence, preview rollback, accepted-only apply, and exact On/Off mod/list/free-roam round trips with fresh UI bindings.\n";
    } catch (const std::exception& error) {
        stopRuntimeControl();
        std::cerr << error.what() << '\n'; return 1;
    }
}
