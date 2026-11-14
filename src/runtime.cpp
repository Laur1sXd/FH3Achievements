// SPDX-License-Identifier: GPL-3.0-only
#include "runtime.hpp"
#include <windows.h>
#include <algorithm>
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
const uint8_t* noticePlatform = nullptr;
bool noticeRefresh = false; // only used on the native UI thread
UINT reloadKey = VK_F9;
auto keyState = &GetAsyncKeyState;
auto foreground = &GetForegroundWindow;
auto windowPid = &GetWindowThreadProcessId;
using Notify = void (*)(void*, void*);

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

void updateNotice(const uint8_t* base, FH3Mod* mod, void* page, int applied, Notify notify) {
    if (!base || applied < 0 || !read<uint8_t>(base + 0x2418a0) ||
        read<void*>(base + 0x2362c0) != page) return;
    auto* row = loadRow(base, mod);
    if (!row) return;
    if (read<int>(row + 0x6c) != applied) {
        std::memcpy(row + 0x6c, &applied, sizeof applied);
        noticeRefresh = true;
    }
    if (!noticeRefresh) return;
    // Resolve these anew every frame; reopening MODS replaces its binding model.
    auto* model = read<void*>(base + 0x234e00);
    auto* descriptor = read<void*>(base + 0x235b10);
    if (!model || !descriptor) return;
    notify(model, descriptor);
    noticeRefresh = false;
}

void monitor() {
    bool down = (keyState(static_cast<int>(reloadKey)) & 0x8000) != 0;
    while (!quitting) {
        DWORD pid{};
        const auto window = foreground();
        const bool front = window && windowPid(window, &pid) && pid == GetCurrentProcessId();
        const bool pressed = front && (keyState(static_cast<int>(reloadKey)) & 0x8000) != 0;
        if (pressed && !down) applyRuntimeLoad();
        down = pressed;
        Sleep(25);
    }
}
}

void startRuntimeControl(const FH3ModApi* api, FH3Mod* mod, bool initial, RuntimeActions callbacks) {
    host = api; owner = mod; actions = callbacks; active = initial;
    const auto config = std::filesystem::path(api->azimuth_dir) / L"fh3-mods/platform.ini";
    const auto* module = hostModule();
    if (!GetPrivateProfileIntW(L"dev", L"HotReload", 0, config.c_str()) || sharedRuntimeLoader(module)) return;
    wchar_t key[16]{};
    GetPrivateProfileStringW(L"dev", L"ReloadKey", L"F9", key, 16, config.c_str());
    if ((key[0] == L'F' || key[0] == L'f') && key[1]) {
        const int n = _wtoi(key + 1);
        if (n >= 1 && n <= 12) reloadKey = VK_F1 + n - 1;
    }
    noticePlatform = stockNoticeLayout(module) ? module : nullptr;
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
        const bool wanted = actions.requested();
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
