// SPDX-License-Identifier: GPL-3.0-only
#include "journal.hpp"
#include "runtime.hpp"

#include <windows.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>

namespace ach {
namespace {
using TileFn = void (*)(void*);
using BuildSceneFn = uint8_t* (*)(uint8_t*);
using FindPropertyFn = uint8_t* (*)(uint8_t*, uint8_t*);
using PageButtonFn = bool (*)(void*, int, bool);

constexpr uintptr_t PauseTile = 0x1579ab0, FireTransition = 0x1689f60, PartyExit = 0x4e31b18;
constexpr uintptr_t HudScene = 0x1182cb0, HudVtable = 0x4566590, FindProperty = 0x200c1d0;
constexpr uintptr_t ObjectCtor = 0x1f6e7c0, ModelProperty = 0x14fd740;
constexpr uintptr_t BoxInt = 0x5592b0, BoxBool = 0x5592d0, BoolType = 0x1f5f000;
constexpr int Accept = 1000000, Back = 999999, Reset = 68, Prev = 19, Next = 20;
constexpr int SectionPane = JournalSections, Index = JournalSections + 1;

const FH3ModApi* api = nullptr;
FH3Mod* mod = nullptr;
const uint8_t* game = nullptr;
TileFn originalTile = nullptr;
BuildSceneFn originalScene = nullptr;
FindPropertyFn originalProperty = nullptr;
TileFn originalFocus = nullptr;
TileFn originalUpdate = nullptr;
PageButtonFn originalButton = nullptr;
std::array<FH3Hook*, 6> hooks{};
std::mutex snapshotMutex;
ProgressList latestProgress{};
Unlocks latestUnlocks{};

std::atomic<ULONGLONG> requestedAt = 0;
std::atomic<bool> sceneRedirected = false;
std::atomic<void*> journalPage = nullptr;
std::atomic<void*> journalModel = nullptr;
std::atomic<int> group = 0;
std::atomic<bool> cardsFocused = false;
bool returningFromCards = false;
bool enteringCards = false;
std::mutex descriptorsMutex;
std::array<uint8_t*, JournalSections + 2> descriptors{};
constexpr std::array<const char*, JournalSections + 2> DescriptorNames = {
    "FH3AchGroup0", "FH3AchGroup1", "FH3AchGroup2", "FH3AchGroup3", "FH3AchGroup4",
    "FH3AchGroup5", "FH3AchGroup6", "FH3AchGroup7", "FH3AchGroup8", "FH3AchGroup9",
    "FH3AchGroup10", "FH3AchSections", "FH3AchIndex"
};

template<class Function> Function function(uintptr_t rva) {
    return reinterpret_cast<Function>(const_cast<uint8_t*>(game) + rva);
}
void report(const char* message) { api->log(mod, message); }

bool hasHudRoute(const wchar_t* path) {
    if (!path || !*path) return false;
    // Standalone Mod Platform mounts its generated pause route in
    // mediaplatform; ExtraOptions setups can still use mediaoverride.
    // Look for the actual route in either location instead of making the
    // entire Progress tile depend on the legacy overlay directory existing.
    for (const auto* overlay : {L"mediaplatform", L"mediaoverride"}) {
        try {
            const auto data = readFile(fs::path(path) / overlay / L"stateflow" / L"pause.xml", 2 << 20);
            const std::string_view contents(reinterpret_cast<const char*>(data.data()), data.size());
            if (contents.find("<node id=\"online_party_list\" state=\"ui_screens.show_hud_options\">") !=
                std::string_view::npos) return true;
        } catch (...) {
            // A missing unused overlay is normal for the other loader mode.
        }
    }
    return false;
}

void notify(void* obj, uint8_t* desc) {
    if (!obj || !desc) return;
    // Same PropertyChanged dispatch as the FH3 platform's HUD properties.
    __try {
        auto* dispatcher = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(obj) + 0x28);
        if (!dispatcher || *reinterpret_cast<int*>(dispatcher + 0x64) <= 0) {
            function<void (*)(void*, void*)>(0x56d4f0)(obj, desc);
            return;
        }
        void* eventId = **reinterpret_cast<void***>(dispatcher + 0x58);
        alignas(16) uint8_t args[0x80]{};
        function<void (*)(void*)>(ObjectCtor)(args);
        *reinterpret_cast<const void**>(args) = game + 0x42f9a28;
        *reinterpret_cast<uint8_t**>(args + 0x28) = desc;
        ++*reinterpret_cast<int*>(desc + 0x10);
        function<void (*)(void*, void*, void*, void*)>(0x20f40b0)(obj, eventId, obj, args);
        --*reinterpret_cast<int*>(desc + 0x10);
        function<void (*)(void*)>(0x1f71d70)(args);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        report("Achievement page binding update failed.");
    }
}

int descriptorIndex(const void* desc) {
    for (int i = 0; i < static_cast<int>(descriptors.size()); ++i)
        if (descriptors[i] == desc) return i;
    return -1;
}

void** getValue(void** out, void* desc, void* obj) {
    *out = nullptr;
    if (obj) journalModel = obj;
    const int key = descriptorIndex(desc);
    if (key == Index) {
        const int value = group;
        return function<void** (*)(void**, const int*)>(BoxInt)(out, &value);
    }
    const bool value = key == SectionPane ? !cardsFocused.load() : key == group.load();
    return function<void** (*)(void**, const bool*)>(BoxBool)(out, &value);
}

bool setGroup(void* obj, int next) {
    const int previous = group.exchange(next);
    if (next == previous) return false;
    notify(obj, descriptors[previous]);
    notify(obj, descriptors[next]);
    return true;
}

void setValue(void* desc, void* obj, const uint8_t* value) {
    if (desc != descriptors[Index] || !value || cardsFocused) return;
    const int next = *reinterpret_cast<const int*>(value + 0x28);
    if (next < 0 || next >= JournalSections) return;
    if (obj) journalModel = obj;
    setGroup(obj, next);
}

// Descriptor layout is the one used by the FH3 platform's AVUI properties.
uint8_t* makeDescriptor(int slot, uint8_t* owner) {
    auto* model = function<uint8_t* (*)()>(ModelProperty)();
    if (!model) return nullptr;
    auto* desc = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x60, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!desc) return nullptr;
    function<void (*)(void*)>(ObjectCtor)(desc);
    std::memcpy(desc, model, 8);
    *reinterpret_cast<int*>(desc + 0x10) += 1 << 20;
    *reinterpret_cast<void**>(desc + 0x28) = reinterpret_cast<void*>(getValue);
    *reinterpret_cast<void**>(desc + 0x30) = reinterpret_cast<void*>(setValue);
    std::memcpy(desc + 0x38, model + 0x38, 8);
    *reinterpret_cast<const char**>(desc + 0x40) = DescriptorNames[slot];
    *reinterpret_cast<void**>(desc + 0x48) = slot == Index ? *reinterpret_cast<void**>(model + 0x48) :
                                             function<void* (*)(int)>(BoolType)(0);
    *reinterpret_cast<uint8_t**>(desc + 0x50) = owner;
    desc[0x58] = model[0x58];
    return desc;
}

int matchProperty(uint8_t* name) {
    if (!name) return -1;
    __try {
        const auto* wide = *reinterpret_cast<const wchar_t**>(name + 0x28);
        if (!wide || std::wcsncmp(wide, L"FH3Ach", 6) != 0) return -1;
        char narrow[24]{};
        for (size_t i = 0; i + 1 < sizeof narrow && wide[i] && wide[i] <= 127; ++i)
            narrow[i] = static_cast<char>(wide[i]);
        for (int i = 0; i < static_cast<int>(descriptors.size()); ++i)
            if (std::strcmp(narrow, DescriptorNames[i]) == 0) return i;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return -1;
}

uint8_t* onProperty(uint8_t* owner, uint8_t* name) {
    if (auto* found = originalProperty(owner, name)) return found;
    const int slot = matchProperty(name);
    if (slot < 0 || !owner) return nullptr;
    std::lock_guard lock(descriptorsMutex);
    if (!descriptors[slot]) descriptors[slot] = makeDescriptor(slot, owner);
    return descriptors[slot];
}

bool focusNamed(void* page, const char* name) {
    __try {
        alignas(16) uint8_t string[0x80]{};
        function<void (*)(void*, const char*, int)>(0x2094100)(string, name, -1);
        void* scratch[2]{};
        auto* element = function<uint8_t* (*)(void*, void*, void*)>(0x20ea170)(page, string, scratch);
        function<void (*)(void*)>(0x2095e20)(string);
        if (!element) return false;
        void** input = reinterpret_cast<void**>(element + 0x88);
        auto** vtable = *reinterpret_cast<void***>(input);
        reinterpret_cast<void (*)(void*)>(vtable[3])(input);
        void* out[4]{};
        function<void (*)(void**, void*)>(0x203bf60)(out, input);
        auto* object = static_cast<uint8_t*>(out[1]);
        if (object && InterlockedDecrement(reinterpret_cast<volatile LONG*>(object + 0x10)) == 0)
            function<void (*)(void*)>(0x31eb20)(object);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool focusGroup(void* page, bool cards) {
    char name[32];
    const int current = group;
    if (cards) std::snprintf(name, sizeof name, "AchCard_%d_0", current);
    else if (current) std::snprintf(name, sizeof name, "AchSection_%d", current);
    else std::snprintf(name, sizeof name, "FirstItem");
    return focusNamed(page, name);
}

bool onButton(void* page, int button, bool pressed) {
    if (journalPage.load() != page) return originalButton(page, button, pressed);
    if (button == Reset) return true;
    if (button == Prev || button == Next) {
        if (pressed) {
            const int next = (group + JournalSections + (button == Next ? 1 : -1)) % JournalSections;
            auto* obj = journalModel.load();
            if (setGroup(obj, next)) notify(obj, descriptors[Index]);
        }
        focusGroup(page, cardsFocused);
        return true;
    }
    if (button == Accept) {
        if (pressed && !cardsFocused) {
            cardsFocused = true;
            enteringCards = true;
            notify(journalModel, descriptors[SectionPane]);
            focusGroup(page, true);
        } else if (!pressed && enteringCards) {
            enteringCards = false;
            if (!focusGroup(page, true)) {
                cardsFocused = false;
                notify(journalModel, descriptors[SectionPane]);
                focusGroup(page, false);
            }
        }
        return true;
    }
    if (button != Back) return originalButton(page, button, pressed);
    if (pressed && cardsFocused) {
        cardsFocused = false;
        returningFromCards = true;
        notify(journalModel, descriptors[SectionPane]);
        focusGroup(page, false);
        return true;
    }
    if (returningFromCards) {
        if (!pressed) {
            returningFromCards = false;
            focusGroup(page, false);
        }
        return true;
    }
    const bool handled = originalButton(page, button, pressed);
    if (!pressed) {
        journalPage = nullptr;
        journalModel = nullptr;
    }
    return handled;
}

void onFocus(void* page) {
    if (sceneRedirected.exchange(false)) {
        journalPage = page;
        requestedAt = 0;
        report("Native Achievements page opened.");
    } else if (journalPage != page) {
        journalPage = nullptr;
        journalModel = nullptr;
        requestedAt = 0;
    }
    originalFocus(page);
}

void onUpdate(void* page) {
    if (journalPage.load() == page) {
        // HudOptions' frame update reads HUD-safe-frame resources and updates
        // its HUD editor. This journal uses the inherited menu page instead.
        // Keep that inherited update, without running the HUD editor against
        // a custom journal scene (native null-resource crash at 0x14db340).
        function<void (*)(void*)>(0x14a1b70)(page);
    } else {
        originalUpdate(page);
    }
    refreshRuntimeNotice(page);
}

uint8_t* onBuildScene(uint8_t* out) {
    auto* record = originalScene(out);
    const auto asked = requestedAt.load();
    if (!record || !asked || GetTickCount64() - asked > 2500) return record;
    const auto size = *reinterpret_cast<const uint64_t*>(record + 0x38);
    const auto capacity = *reinterpret_cast<const uint64_t*>(record + 0x40);
    if (size != 19 || capacity < size) return record;
    auto* path = *reinterpret_cast<wchar_t**>(record + 0x28);
    if (!path || std::wcsncmp(path, L"Anthem\\A_HudOptions", 19) != 0) return record;
    std::wmemcpy(path + 7, L"A_AchJournal", 12);
    sceneRedirected = true;
    return record;
}

void onTile(void* page) {
    try {
        ProgressList progress;
        Unlocks unlocks;
        {
            std::lock_guard lock(snapshotMutex);
            progress = latestProgress;
            unlocks = latestUnlocks;
        }
        const auto xaml = journalXaml(progress, unlocks);
        if (api->set_file(mod, JournalFile, xaml.data(), xaml.size())) {
            group = 0;
            cardsFocused = false;
            returningFromCards = false;
            enteringCards = false;
            journalModel = nullptr;
            sceneRedirected = false;
            requestedAt = GetTickCount64();
            function<void (*)(void*, const void*, int)>(FireTransition)(page, game + PartyExit, 0);
            return;
        }
    } catch (...) {
        report("Could not prepare the native Achievements page.");
    }
    originalTile(page);
}
}

bool platformJournalRouteReady(const uint8_t* base) {
    if (!base) return false;
    // Mod Platform validates stock UI code and vtable slots after building the
    // Mods pages. Leave those unchanged until its shared page hooks are wired.
    // wait_for_code alone can return before a slow database/UI build finishes.
    __try {
        auto* const* vtable = reinterpret_cast<void* const*>(base + HudVtable);
        return vtable[148] && vtable[179] &&
            vtable[148] != base + 0x13a19a0 && vtable[179] != base + 0x14981a0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool installJournal(const FH3ModApi* platform, FH3Mod* handle, const uint8_t* base) {
    if (!platform->set_file || !platform->hook_function || !platform->hook_pointer ||
        !platform->enable_hooks || !hasHudRoute(platform->game_dir)) return false;
    api = platform;
    mod = handle;
    game = base;
    auto* writable = const_cast<uint8_t*>(base);
    auto** vtable = reinterpret_cast<void**>(writable + HudVtable);
    // The platform uses the same scene and property hooks for MODS. The hook API
    // chains those shared entry points so normal HUD and MODS navigation stays intact.
    if (!hooks[0]) hooks[0] = api->hook_function(mod, "Achievement property bindings", writable + FindProperty,
                                  reinterpret_cast<void*>(onProperty), reinterpret_cast<void**>(&originalProperty));
    if (!hooks[1]) hooks[1] = api->hook_function(mod, "Achievement scene path", writable + HudScene,
                                  reinterpret_cast<void*>(onBuildScene), reinterpret_cast<void**>(&originalScene));
    if (!hooks[2]) hooks[2] = api->hook_pointer(mod, "Achievement page focus", &vtable[148],
                                 reinterpret_cast<void*>(onFocus), reinterpret_cast<void**>(&originalFocus));
    if (!hooks[3]) hooks[3] = api->hook_pointer(mod, "Achievement page buttons", &vtable[179],
                                 reinterpret_cast<void*>(onButton), reinterpret_cast<void**>(&originalButton));
    if (!hooks[4]) hooks[4] = api->hook_function(mod, "Pause / Progress / Achievements", writable + PauseTile,
                                  reinterpret_cast<void*>(onTile), reinterpret_cast<void**>(&originalTile));
    if (!hooks[5]) hooks[5] = api->hook_pointer(mod, "Achievement page update", &vtable[175],
                                 reinterpret_cast<void*>(onUpdate), reinterpret_cast<void**>(&originalUpdate));
    for (auto* hook : hooks) {
        if (!hook || !api->enable_hooks(mod, hook)) {
            stopJournal();
            return false;
        }
    }
    return true;
}

void publishJournal(const ProgressList& progress, const Unlocks& unlocks) {
    std::lock_guard lock(snapshotMutex);
    latestProgress = progress;
    latestUnlocks = unlocks;
}

void stopJournal(bool keepNoticeRefresh) {
    if (!api || !mod) return;
    for (size_t i = hooks.size(); i-- > 0;)
        if (hooks[i] && !(keepNoticeRefresh && i == 5)) api->disable_hooks(mod, hooks[i]);
    // Keep tokens for the resident controller: stock platform hooks are toggled
    // in place, so repeated On never adds a second hook on the same target.
    // FH3 mounts this scene once. Retain its file across Off/On so the next
    // DLL can reopen it without requiring a game restart to remount assets.
}

bool journalCanShutdown() {
    const auto asked = requestedAt.load();
    return !journalPage.load() && !sceneRedirected.load() &&
        (!asked || GetTickCount64() - asked > 2500);
}
}
