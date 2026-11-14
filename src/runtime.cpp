// SPDX-License-Identifier: GPL-3.0-only
#include "runtime.hpp"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace ach {
namespace {
const FH3ModApi* host = nullptr;
FH3Mod* owner = nullptr;
RuntimeActions actions{};
std::mutex controlMutex;
std::thread listener;
std::atomic<bool> quitting = false, enabled = false, active = false;
std::atomic<int> appliedNotice = -1;
std::atomic<unsigned> acceptedWriteEpoch = 0;
const uint8_t* noticePlatform = nullptr;
bool noticeRefresh = false; // only used on the native UI thread
UINT reloadKey = VK_F9;
auto keyState = &GetAsyncKeyState;
auto foreground = &GetForegroundWindow;
auto windowPid = &GetWindowThreadProcessId;
using Notify = void (*)(void*, void*);
using Setter = void (*)(void*, void*, void*);
using Buttons = bool (*)(void*, int, bool);
Setter originalLoadSet = nullptr, originalSelectionSet = nullptr;
Buttons originalButtons = nullptr;
std::array<FH3Hook*, 3> previewHooks{};
std::atomic<bool> previewReady = false;
struct LoadPreview {
    uint8_t* row = nullptr;
    void* page = nullptr;
    int selection = 0, accepted = 0, boot = 0;
} preview;
bool acceptRelease = false;
thread_local bool refreshingBindings = false;
Notify nativeNotify = nullptr;

template<class T> T read(const void* address) {
    T value{};
    SIZE_T bytes{};
    if (!ReadProcessMemory(GetCurrentProcess(), address, &value, sizeof value, &bytes) || bytes != sizeof value)
        return {};
    return value;
}

const uint8_t* hostModule() {
    HMODULE module{};
    if (!host || !host->register_mod || !GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(host->register_mod), &module)) return nullptr;
    return reinterpret_cast<const uint8_t*>(module);
}

bool sharedRuntimeLoader(const uint8_t* base) {
    if (!base) return false;
    const auto dos = read<IMAGE_DOS_HEADER>(base);
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 0x100000) return false;
    const auto nt = read<IMAGE_NT_HEADERS64>(base + dos.e_lfanew);
    if (nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.NumberOfSections > 96) return false;
    const auto* sections = base + dos.e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
    constexpr char marker[] = "Underglow F9 compatibility";
    for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        const auto section = read<IMAGE_SECTION_HEADER>(sections + i * sizeof(IMAGE_SECTION_HEADER));
        if (!(section.Characteristics & IMAGE_SCN_MEM_READ) || (section.Characteristics & IMAGE_SCN_MEM_DISCARDABLE) ||
            section.VirtualAddress >= nt.OptionalHeader.SizeOfImage) continue;
        const auto size = (std::min)(section.Misc.VirtualSize, nt.OptionalHeader.SizeOfImage - section.VirtualAddress);
        const auto* first = base + section.VirtualAddress;
        if (std::search(first, first + size, reinterpret_cast<const uint8_t*>(marker),
                        reinterpret_cast<const uint8_t*>(marker) + sizeof(marker) - 1) != first + size) return true;
    }
    return false;
}

bool stockNoticeLayout(const uint8_t* base) {
    if (!base) return false;
    const auto dos = read<IMAGE_DOS_HEADER>(base);
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 0x100000) return false;
    const auto nt = read<IMAGE_NT_HEADERS64>(base + dos.e_lfanew);
    // Checked official 1.4.2/API 17 layout, shared with Underglow's tested
    // notice adapter. Unknown platform builds retain their own notice behavior.
    return nt.Signature == IMAGE_NT_SIGNATURE && nt.FileHeader.TimeDateStamp == 0x6af85a9e &&
        nt.OptionalHeader.SizeOfImage == 0x26b000;
}

uint8_t* loadRow(const uint8_t* base, FH3Mod* mod) {
    constexpr size_t entrySize = 0xb0, entryMod = 0xa0, rowSize = 0xd0, blockSize = 4096 / rowSize;
    const auto begin = read<uintptr_t>(base + 0x241da0), end = read<uintptr_t>(base + 0x241da8);
    if (!begin || end < begin || (end - begin) % entrySize || end - begin > entrySize * 4096) return nullptr;
    size_t entry = 0;
    for (; entry < (end - begin) / entrySize; ++entry)
        if (read<FH3Mod*>(reinterpret_cast<const void*>(begin + entry * entrySize + entryMod)) == mod) break;
    if (entry == (end - begin) / entrySize) return nullptr;
    const auto key = "Mod_" + std::to_string(entry);
    const auto* deque = base + 0x241248;
    const auto blocks = read<uintptr_t>(deque + 8), blocksEnd = read<uintptr_t>(deque + 16);
    const auto start = read<size_t>(deque + 32), count = read<size_t>(deque + 40);
    if (!blocks || blocksEnd < blocks || count > 4096 || start > 4096) return nullptr;
    for (size_t i = 0; i < count; ++i) {
        const auto slot = start + i, block = slot / blockSize;
        if ((block + 1) * 8 > blocksEnd - blocks) return nullptr;
        auto* row = read<uint8_t*>(reinterpret_cast<const void*>(blocks + block * 8));
        if (!row) return nullptr;
        row += (slot % blockSize) * rowSize;
        if (key.size() >= 23 || read<uint8_t>(row + 8) != key.size() * 2 ||
            read<uintptr_t>(row + 0x80) != entry || !read<void*>(row + 0x78) ||
            read<FH3Mod*>(row) == mod || read<uint8_t>(row + 0x90)) continue;
        char text[24]{};
        SIZE_T bytes{};
        if (ReadProcessMemory(GetCurrentProcess(), row + 9, text, key.size() + 1, &bytes) &&
            bytes == key.size() + 1 && std::strcmp(text, key.c_str()) == 0) return row;
    }
    return nullptr;
}

void* ownSettingsPage(const uint8_t* base, FH3Mod* mod) {
    if (!base || !read<uint8_t>(base + 0x2418a0)) return nullptr;
    const int view = read<int>(base + 0x2412a8);
    const auto begin = read<uintptr_t>(base + 0x241da0), end = read<uintptr_t>(base + 0x241da8);
    if (view <= 0 || !begin || end < begin || end - begin > 0xb0 * 4096 ||
        static_cast<size_t>(view) > (end - begin) / 0xb0) return nullptr;
    if (read<FH3Mod*>(reinterpret_cast<void*>(begin + (view - 1) * 0xb0 + 0xa0)) != mod) return nullptr;
    return read<void*>(base + 0x2362c0);
}

bool safeNotify(Notify notify, void* model, void* descriptor) {
    if (!notify || !model || !descriptor) return false;
    __try { notify(model, descriptor); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void refreshLoadValue(uint8_t* row) {
    const bool previous = refreshingBindings;
    refreshingBindings = true;
    safeNotify(nativeNotify, read<void*>(noticePlatform + 0x234e00), read<void*>(row + 0x88));
    refreshingBindings = previous;
    noticeRefresh = true;
}

void discardPreview(bool notify = true) {
    if (!preview.row) return;
    auto* row = preview.row;
    const int baseline = appliedNotice < 0 ? preview.boot : appliedNotice.load();
    InterlockedExchange(reinterpret_cast<volatile LONG*>(row + 0x68), preview.accepted);
    std::memcpy(row + 0x6c, &baseline, sizeof baseline);
    preview = {};
    noticeRefresh = true;
    if (notify) refreshLoadValue(row);
}

bool callAcceptedChange(uint8_t* row, int value) {
    using Changed = void (*)(void*, const char*, int);
    const auto changed = read<Changed>(row + 0x78);
    if (!changed) return false;
    __try {
        changed(read<void*>(row + 0x80), reinterpret_cast<const char*>(row + 9), value);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool acceptPreview() {
    if (!preview.row) return false;
    const int wanted = read<int>(preview.row + 0x68);
    try {
        if (wanted != preview.accepted) {
            struct AcceptedWrite {
                AcceptedWrite() { ++acceptedWriteEpoch; }
                ~AcceptedWrite() { ++acceptedWriteEpoch; }
            } writing;
            if (!callAcceptedChange(preview.row, wanted) || actions.requested() != (wanted != 0)) {
                host->log(owner, "Could not accept Load Achievements. Press A to retry or B to discard.");
                return false;
            }
        }
        preview.accepted = wanted;
        discardPreview();
        host->log(owner, "Accepted Load Achievements; F9 applies this saved value.");
        return true;
    } catch (...) {
        host->log(owner, "Could not save the accepted Load setting; the previous value remains active.");
        return false;
    }
}

void updateNotice(const uint8_t* base, FH3Mod* mod, void* page, int applied, Notify notify) {
    if (!base) return;
    if (preview.row && (preview.page != ownSettingsPage(base, mod) ||
        preview.selection != read<int>(base + 0x2362a8))) discardPreview();
    if (!read<uint8_t>(base + 0x2418a0) || read<void*>(base + 0x2362c0) != page) return;
    auto* row = loadRow(base, mod);
    if (!row) return;
    int baseline = applied;
    if (preview.row == row) {
        const int actual = applied < 0 ? preview.boot : applied;
        const int shown = read<int>(row + 0x68);
        // Previewing a different value shows the notice, but cannot erase an
        // earlier accepted request which is still waiting for F9.
        const bool pending = shown != actual || preview.accepted != actual;
        baseline = pending ? 1 - shown : shown;
    }
    if (baseline >= 0 && read<int>(row + 0x6c) != baseline) {
        std::memcpy(row + 0x6c, &baseline, sizeof baseline);
        noticeRefresh = true;
    }
    if (!noticeRefresh) return;
    // Resolve these anew every frame; reopening MODS replaces its binding model.
    auto* model = read<void*>(base + 0x234e00);
    auto* descriptor = read<void*>(base + 0x235b10);
    if (!model || !descriptor) return;
    noticeRefresh = !safeNotify(notify, model, descriptor);
}

void onLoadSet(void* descriptor, void* model, void* value) {
    auto* row = noticePlatform ? loadRow(noticePlatform, owner) : nullptr;
    if (row && descriptor == read<void*>(row + 0x88)) {
        if (refreshingBindings) return;
        auto* page = ownSettingsPage(noticePlatform, owner);
        int wanted{}; SIZE_T bytes{};
        if (previewReady && page && value && ReadProcessMemory(GetCurrentProcess(),
            static_cast<uint8_t*>(value) + 0x28, &wanted, sizeof wanted, &bytes) && bytes == sizeof wanted &&
            (wanted == 0 || wanted == 1)) {
            const int selected = read<int>(noticePlatform + 0x2362a8);
            if (preview.row && (preview.page != page || preview.selection != selected)) discardPreview();
            if (!preview.row) preview = {row, page, selected, read<int>(row + 0x68), read<int>(row + 0x6c)};
            InterlockedExchange(reinterpret_cast<volatile LONG*>(row + 0x68), wanted);
            updateNotice(noticePlatform, owner, page, appliedNotice, nativeNotify);
            return; // Preview only: never persist a selector movement.
        }
    }
    originalLoadSet(descriptor, model, value);
}

void onLoadSelection(void* descriptor, void* model, void* value) {
    originalSelectionSet(descriptor, model, value);
    if (preview.row && preview.selection != read<int>(noticePlatform + 0x2362a8)) discardPreview();
}

bool onLoadButtons(void* page, int button, bool pressed) {
    constexpr int Accept = 1000000, Back = 999999;
    if (button == Accept && !pressed && acceptRelease) { acceptRelease = false; return true; }
    if (preview.row) {
        if (preview.page != ownSettingsPage(noticePlatform, owner) ||
            preview.selection != read<int>(noticePlatform + 0x2362a8) ||
            (page == preview.page && button == Back && pressed)) discardPreview();
        else if (page == preview.page && button == Accept && pressed) {
            acceptPreview();
            acceptRelease = true;
            updateNotice(noticePlatform, owner, page, appliedNotice, nativeNotify);
            return true;
        }
    }
    const bool result = originalButtons(page, button, pressed);
    updateNotice(noticePlatform, owner, page, appliedNotice, nativeNotify);
    return result;
}

bool matches(const uint8_t* code, const char* expected, size_t size) {
    std::array<uint8_t, 16> actual{}; SIZE_T bytes{};
    if (size > actual.size() || !ReadProcessMemory(GetCurrentProcess(), code, actual.data(), size, &bytes) || bytes != size) return false;
    if (std::memcmp(actual.data(), expected, size) == 0) return true;
    if (size < 6 || actual[0] != 0xe9 || std::memcmp(actual.data() + 5, expected + 5, size - 5)) return false;
    int32_t offset{}; std::memcpy(&offset, actual.data() + 1, sizeof offset);
    const auto relay = read<std::array<uint8_t, 6>>(code + 5 + offset);
    return relay == std::array<uint8_t, 6>{0xff, 0x25, 0, 0, 0, 0};
}

void monitor() {
    bool down = (keyState(static_cast<int>(reloadKey)) & 0x8000) != 0;
    while (!quitting) {
        DWORD pid{};
        const auto window = foreground();
        const bool front = window && windowPid(window, &pid) && pid == GetCurrentProcessId();
        const bool pressed = (keyState(static_cast<int>(reloadKey)) & 0x8000) != 0;
        if (pressed && !down && front) {
            // Focus can change between the first sample and the key sample.
            // Recheck it at the edge; held background keys are still tracked.
            DWORD currentPid{};
            const auto current = foreground();
            if (current == window && windowPid(current, &currentPid) && currentPid == GetCurrentProcessId())
                applyRuntimeLoad();
        }
        down = pressed;
        Sleep(25);
    }
}
}

void startRuntimeControl(const FH3ModApi* api, FH3Mod* mod, bool initial, RuntimeActions callbacks) {
    host = api; owner = mod; actions = callbacks; active = initial;
    quitting = false;
    const auto config = std::filesystem::path(api->azimuth_dir) / L"fh3-mods/platform.ini";
    const auto* module = hostModule();
    noticePlatform = stockNoticeLayout(module) ? module : nullptr;
    nativeNotify = noticePlatform ? reinterpret_cast<Notify>(const_cast<uint8_t*>(noticePlatform) + 0xbc420) : nullptr;
    if (!GetPrivateProfileIntW(L"dev", L"HotReload", 0, config.c_str()) || sharedRuntimeLoader(module)) return;
    wchar_t key[16]{};
    GetPrivateProfileStringW(L"dev", L"ReloadKey", L"F9", key, 16, config.c_str());
    if ((key[0] == L'F' || key[0] == L'f') && key[1]) {
        const int n = _wtoi(key + 1);
        if (n >= 1 && n <= 12) reloadKey = VK_F1 + n - 1;
    }
    appliedNotice = initial ? 1 : 0;
    quitting = false;
    enabled = true;
    try { listener = std::thread(monitor); }
    catch (...) { enabled = false; appliedNotice = -1; throw; }
    host->log(owner, "Built-in F9 Load controller ready; stock Mod Platform is unchanged.");
}

bool applyRuntimeLoad() {
    std::lock_guard lock(controlMutex);
    if (!enabled || quitting) return false;
    try {
        const unsigned epoch = acceptedWriteEpoch;
        if (epoch & 1) return false;
        const bool wanted = actions.requested();
        if (acceptedWriteEpoch != epoch) return false;
        if (wanted == active) return true;
        if (!actions.canStop()) return false;
        if (wanted) {
            if (!actions.start()) return false;
        } else actions.stop();
        active = wanted;
        appliedNotice = wanted ? 1 : 0;
        host->log(owner, wanted ? "Stock-platform F9: Load ON applied by Achievements." :
                  "Stock-platform F9: Load OFF applied; tracking and pop-ups stopped. Only the F9 listener and notice refresh remain.");
        return true;
    } catch (...) {
        host->log(owner, "F9 Load change remains pending; close Achievements and retry.");
        return false;
    }
}

void stopRuntimeControl() {
    quitting = true;
    if (listener.joinable()) listener.join();
    enabled = false;
    appliedNotice = -1;
}

bool installRuntimePreview() {
    if (previewReady) return true;
    if (quitting || !noticePlatform || !host->hook_function || !host->enable_hooks || !host->disable_hooks) return false;
    if (!matches(noticePlatform + 0xda1a0, "\x41\x57\x41\x56\x41\x54\x56\x57\x53\x48", 10) ||
        !matches(noticePlatform + 0xdfe70, "\x41\x57\x41\x56\x41\x55\x41\x54\x56\x57", 10) ||
        !matches(noticePlatform + 0xdd320, "\x56\x57\x48\x83\xec\x58\x4d\x85\xc0\x74", 10)) return false;
    auto* base = const_cast<uint8_t*>(noticePlatform);
    if (!previewHooks[0]) previewHooks[0] = host->hook_function(owner, "Achievements Load preview binding", base + 0xdfe70,
        reinterpret_cast<void*>(onLoadSet), reinterpret_cast<void**>(&originalLoadSet));
    if (!previewHooks[1]) previewHooks[1] = host->hook_function(owner, "Achievements Load Accept and Back", base + 0xda1a0,
        reinterpret_cast<void*>(onLoadButtons), reinterpret_cast<void**>(&originalButtons));
    if (!previewHooks[2]) previewHooks[2] = host->hook_function(owner, "Achievements Load row selection", base + 0xdd320,
        reinterpret_cast<void*>(onLoadSelection), reinterpret_cast<void**>(&originalSelectionSet));
    for (auto* hook : previewHooks) {
        if (!hook || !host->enable_hooks(owner, hook)) { stopRuntimePreview(); return false; }
    }
    previewReady = true;
    host->log(owner, "Load preview ready: press A to accept; leaving the row or page discards it.");
    return true;
}

void stopRuntimePreview() {
    previewReady = false;
    discardPreview(false);
    for (auto* hook : previewHooks) if (hook) host->disable_hooks(owner, hook);
}
bool runtimeControlEnabled() { return enabled && !quitting; }
int runtimeLoadState() { return active ? 1 : 0; }
void refreshRuntimeNotice(void* page) {
    // Native UI thread only. A mismatched layout or stale native model must not
    // turn a notice refresh into a game crash.
    if (!noticePlatform) return;
    __try {
        updateNotice(noticePlatform, owner, page, appliedNotice,
                     reinterpret_cast<Notify>(const_cast<uint8_t*>(noticePlatform) + 0xbc420));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        noticeRefresh = true;
    }
}
}
