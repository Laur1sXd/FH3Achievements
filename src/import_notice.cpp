// SPDX-License-Identifier: GPL-3.0-only
#include "import_notice.hpp"

#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace ach {
namespace {
constexpr uintptr_t ComponentCtor = 0xadb350, ComponentDtor = 0xadc940;
constexpr uintptr_t DrivingHudUpdate = 0xb26690;
constexpr uintptr_t UiManager = 0x1eed710, AcquireHud = 0x662ef0;
constexpr uintptr_t MakeDisplayString = 0x1ee5ee0, ShowNotification = 0xaf18a0;
constexpr int DrivingHudScene = 0x79;
constexpr char RoadDiscovery[] = "RoadDiscovery";

using Construct = uint8_t* (*)(uint8_t*);
using Method = void (*)(void*);

const FH3ModApi* platform = nullptr;
FH3Mod* owner = nullptr;
const uint8_t* game = nullptr;
std::array<FH3Hook*, 3> hooks{};
Construct originalCtor = nullptr;
Method originalDtor = nullptr, originalUpdate = nullptr;
std::atomic<bool> enabled = false;
std::atomic<bool> waiting = false;
std::recursive_mutex mutex;
std::vector<uint8_t*> components;
ImportSummary pending;
struct UnlockNotice { std::string title, body; };
std::deque<UnlockNotice> unlockNotices;
bool shown = false;
bool watchingLifetime = false;
ULONGLONG hudSince = 0, nextCheck = 0;

template<class Function> Function native(uintptr_t rva) {
    return reinterpret_cast<Function>(const_cast<uint8_t*>(game) + rva);
}

uint8_t* onConstruct(uint8_t* component) {
    auto* result = originalCtor(component);
    try {
        std::lock_guard lock(mutex);
        components.push_back(result);
    } catch (...) {
        platform->log(owner, "Could not track the achievement notification HUD.");
    }
    return result;
}

void onDestruct(void* component) {
    {
        std::lock_guard lock(mutex);
        std::erase(components, component);
    }
    originalDtor(component);
}

bool drivingHudVisible() {
    __try {
        auto* ui = native<void* (*)()>(UiManager)();
        if (!ui) return false;
        auto** methods = *static_cast<void***>(ui);
        return reinterpret_cast<int (*)(void*)>(methods[0x1e0 / sizeof(void*)])(ui) == DrivingHudScene;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const char* stringData(const uint8_t* text) {
    return *reinterpret_cast<const size_t*>(text + 0x18) < 16 ? reinterpret_cast<const char*>(text)
                                                             : *reinterpret_cast<const char* const*>(text);
}

bool roadDiscoveryReady(const uint8_t* component) {
    // FH3's road notification is identified by IconName. TemplateName is a
    // separate value, and +0x1f0 is occupied while the component is in use.
    return component && *reinterpret_cast<const size_t*>(component + 0x1a0) == sizeof RoadDiscovery - 1 &&
           std::memcmp(stringData(component + 0x190), RoadDiscovery, sizeof RoadDiscovery) == 0 &&
           !*reinterpret_cast<void* const*>(component + 0x1f0);
}

void releaseShared(void* control) {
    if (!control) return;
    auto** methods = *static_cast<void***>(control);
    auto* counts = static_cast<uint8_t*>(control);
    if (InterlockedDecrement(reinterpret_cast<volatile LONG*>(counts + 8)) == 0) {
        reinterpret_cast<Method>(methods[0])(control);
        if (InterlockedDecrement(reinterpret_cast<volatile LONG*>(counts + 12)) == 0)
            reinterpret_cast<Method>(methods[1])(control);
    }
}

bool showNotice(uint8_t* component, const char* heading, const char* detail) {
    __try {
        void* hud[2]{};
        native<void** (*)(void*, void**)>(AcquireHud)(component + 0x1f8, hud);
        if (!hud[0]) {
            releaseShared(hud[1]);
            return false;
        }
        void *title = nullptr, *body = nullptr, *icon = nullptr, *style = nullptr;
        const auto make = native<void** (*)(void**, const char*)>(MakeDisplayString);
        make(&title, heading);
        make(&body, detail);
        make(&icon, "Achievement");
        make(&style, stringData(component + 0x1d0));
        // ShowNotification takes ownership of these four native display strings.
        native<void (*)(void*, void**, void**, void**, void**)>(ShowNotification)(component, &title, &body, &icon, &style);
        releaseShared(hud[1]);
        return *reinterpret_cast<void* const*>(component + 0x1f0) != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        enabled = false;
        platform->log(owner, "Native achievement HUD notification failed and was left pending.");
        return false;
    }
}

// Called on FH3's HUD thread. Queueing from the save worker never calls native UI.
void pumpNotices(ULONGLONG now, bool visible) {
    if (!enabled || !waiting) return;
    if (now < nextCheck) return;
    nextCheck = now + 250;
    if (!visible) {
        hudSince = 0;
        return;
    }
    if (!hudSince) {
        hudSince = now;
    }
    if (now - hudSince < 1500) return;
    auto it = std::ranges::find_if(components, roadDiscoveryReady);
    if (it == components.end()) return;
    bool delivered = false;
    if (pending.count && !shown) {
        char text[96];
        std::snprintf(text, sizeof text, "%d unlocked from your save - %dG", pending.count, pending.score);
        delivered = showNotice(*it, "ACHIEVEMENTS UNLOCKED", text);
        if (delivered) {
            shown = true;
            platform->log(owner, "First-run achievement summary shown on the driving HUD.");
        }
    } else if (!unlockNotices.empty()) {
        const auto& notice = unlockNotices.front();
        delivered = showNotice(*it, notice.title.c_str(), notice.body.c_str());
        if (delivered) {
            unlockNotices.pop_front();
            platform->log(owner, "Achievement unlock shown on the native driving HUD.");
        }
    }
    waiting = (pending.count && !shown) || !unlockNotices.empty();
    if (delivered) nextCheck = now + 1500;
}

void onUpdate(void* controller) {
    originalUpdate(controller);
    if (!enabled || !waiting) return;
    std::lock_guard lock(mutex);
    DWORD foreground = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
    pumpNotices(GetTickCount64(), foreground == GetCurrentProcessId() && drivingHudVisible());
}
}

bool installImportNotice(const FH3ModApi* api, FH3Mod* mod, const uint8_t* base) {
    if (!api->hook_function || !api->enable_hooks || !api->disable_hooks) return false;
    platform = api;
    owner = mod;
    game = base;
    constexpr uint8_t MakeBytes[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x50, 0x48, 0xc7, 0x44, 0x24, 0x28};
    constexpr uint8_t ShowBytes[] = {0x48, 0x8b, 0xc4, 0x4c, 0x89, 0x48, 0x20, 0x4c, 0x89, 0x40, 0x18};
    if (std::memcmp(base + MakeDisplayString, MakeBytes, sizeof MakeBytes) != 0 ||
        std::memcmp(base + ShowNotification, ShowBytes, sizeof ShowBytes) != 0) return false;

    auto* writable = const_cast<uint8_t*>(base);
    {
        std::lock_guard lock(mutex);
        // Resident Off keeps constructor/destructor watches so re-enabling
        // can use an existing HUD without keeping stale component pointers.
        if (!watchingLifetime) components.clear();
    }
    if (!hooks[0]) hooks[0] = api->hook_function(mod, "Achievement HUD notification destructor", writable + ComponentDtor,
                                  reinterpret_cast<void*>(onDestruct), reinterpret_cast<void**>(&originalDtor));
    if (!hooks[1]) hooks[1] = api->hook_function(mod, "Achievement HUD notification constructor", writable + ComponentCtor,
                                  reinterpret_cast<void*>(onConstruct), reinterpret_cast<void**>(&originalCtor));
    if (!hooks[2]) hooks[2] = api->hook_function(mod, "Achievement driving HUD update", writable + DrivingHudUpdate,
                                  reinterpret_cast<void*>(onUpdate), reinterpret_cast<void**>(&originalUpdate));
    for (auto* hook : hooks) {
        if (!hook || !api->enable_hooks(mod, hook)) {
            stopImportNotice();
            return false;
        }
    }
    watchingLifetime = true;
    return true;
}

void stopImportNotice(bool keepLifetimeWatch) {
    enabled = false;
    if (!platform || !owner) return;
    for (size_t i = hooks.size(); i-- > 0;)
        if (hooks[i] && !(keepLifetimeWatch && watchingLifetime && i < 2))
            platform->disable_hooks(owner, hooks[i]);
    std::lock_guard lock(mutex);
    unlockNotices.clear();
    waiting = pending.count && !shown;
    hudSince = nextCheck = 0;
    if (!keepLifetimeWatch) {
        watchingLifetime = false;
        components.clear();
    }
}

void enableImportNotice(bool value) {
    std::lock_guard lock(mutex);
    enabled = value;
    if (!value) unlockNotices.clear();
    waiting = (pending.count && !shown) || !unlockNotices.empty();
    hudSince = nextCheck = 0;
}

void queueImportNotice(ImportSummary summary) {
    std::lock_guard lock(mutex);
    pending = summary;
    unlockNotices.clear(); // Profile changes cannot show the previous career's unlocks.
    shown = false;
    hudSince = nextCheck = 0;
    waiting = summary.count != 0;
}

void queueAchievementNotices(std::span<const Achievement* const> achievements) {
    if (achievements.empty()) return;
    std::deque<UnlockNotice> batch;
    if (achievements.size() >= 2) {
        int score = 0;
        for (const auto* achievement : achievements) score += achievement->gamerscore;
        batch.push_back({"ACHIEVEMENTS UNLOCKED", std::to_string(achievements.size()) +
            " achievements - " + std::to_string(score) + "G"});
    } else {
        for (const auto* achievement : achievements) {
            const auto length = WideCharToMultiByte(CP_UTF8, 0, achievement->title, -1, nullptr, 0, nullptr, nullptr);
            if (length <= 1) continue;
            std::string name(length, '\0');
            WideCharToMultiByte(CP_UTF8, 0, achievement->title, -1, name.data(), length, nullptr, nullptr);
            name.pop_back();
            batch.push_back({"ACHIEVEMENT UNLOCKED", name + " - " + std::to_string(achievement->gamerscore) + "G"});
        }
    }
    std::lock_guard lock(mutex);
    if (!enabled) return;
    for (auto& notice : batch) unlockNotices.push_back(std::move(notice));
    if (!waiting && !unlockNotices.empty()) hudSince = nextCheck = 0;
    waiting = (pending.count && !shown) || !unlockNotices.empty();
}

bool importNoticeShown() {
    std::lock_guard lock(mutex);
    return shown;
}
}
