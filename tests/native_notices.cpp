// SPDX-License-Identifier: GPL-3.0-only
// Exercise the production native HUD route using a private mapped-code fixture.
#include "../src/import_notice.cpp"
#include <iostream>
#include <set>
#include <thread>
#include <stdexcept>

namespace {
using namespace ach;
std::set<FH3Hook*> activeHooks;
unsigned definitions = 0, inheritedFrames = 0;
DWORD uiThread = 0;
bool acquired = true, accepted = true;
std::vector<UnlockNotice> displayed;
std::array<uint8_t, 0x240> firstComponent{}, secondComponent{};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void logLine(FH3Mod*, const char*) {}
uint8_t* construct(uint8_t* value) { return value; }
void destruct(void*) {}
void update(void*) { ++inheritedFrames; }
FH3Hook* hook(FH3Mod*, const char*, void* target, void*, void** original) {
    uintptr_t id = 0;
    if (target == game + ComponentDtor) { id = 1; *original = reinterpret_cast<void*>(destruct); }
    if (target == game + ComponentCtor) { id = 2; *original = reinterpret_cast<void*>(construct); }
    if (target == game + DrivingHudUpdate) { id = 3; *original = reinterpret_cast<void*>(update); }
    if (!id) return nullptr;
    ++definitions;
    return reinterpret_cast<FH3Hook*>(id);
}
int enable(FH3Mod*, FH3Hook* handle) { activeHooks.insert(handle); return 1; }
int disable(FH3Mod*, FH3Hook* handle) { activeHooks.erase(handle); return 1; }

void** acquire(void*, void** result) {
    result[0] = acquired ? firstComponent.data() : nullptr;
    result[1] = nullptr;
    return result;
}
void** make(void** result, const char* text) {
    *result = new std::string(text);
    return result;
}
void show(void* component, void** title, void** body, void** icon, void** style) {
    require(GetCurrentThreadId() == uiThread, "native UI called from the worker thread");
    auto* heading = static_cast<std::string*>(*title);
    auto* detail = static_cast<std::string*>(*body);
    auto* graphic = static_cast<std::string*>(*icon);
    auto* theme = static_cast<std::string*>(*style);
    require(*graphic == "Achievement", "native achievement icon token missing");
    require(*theme == "RoadStyle", "native component template was replaced");
    if (accepted) {
        displayed.push_back({*heading, *detail});
        *reinterpret_cast<void**>(static_cast<uint8_t*>(component) + 0x1f0) = component;
    }
    delete heading; delete detail; delete graphic; delete theme;
}

void writeCode(uintptr_t rva, const void* data, size_t size) {
    DWORD old{};
    auto* address = const_cast<uint8_t*>(game) + rva;
    require(VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &old), "fixture code protection failed");
    std::memcpy(address, data, size);
    FlushInstructionCache(GetCurrentProcess(), address, size);
}
void stub(uintptr_t rva, const void* target) {
    uint8_t code[]{0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xe0};
    std::memcpy(code + 2, &target, sizeof target);
    writeCode(rva, code, sizeof code);
}
void signatures() {
    const uint8_t makeCode[]{0x40,0x53,0x48,0x83,0xec,0x50,0x48,0xc7,0x44,0x24,0x28};
    const uint8_t showCode[]{0x48,0x8b,0xc4,0x4c,0x89,0x48,0x20,0x4c,0x89,0x40,0x18};
    writeCode(MakeDisplayString, makeCode, sizeof makeCode);
    writeCode(ShowNotification, showCode, sizeof showCode);
}
void bindNative() {
    stub(AcquireHud, reinterpret_cast<void*>(acquire));
    stub(MakeDisplayString, reinterpret_cast<void*>(make));
    stub(ShowNotification, reinterpret_cast<void*>(show));
}
void componentString(uint8_t* output, const char* text) {
    const size_t length = std::strlen(text), capacity = 15;
    require(length <= capacity, "fixture string is too long");
    std::memcpy(output, text, length + 1);
    std::memcpy(output + 16, &length, sizeof length);
    std::memcpy(output + 24, &capacity, sizeof capacity);
}
void initComponent(auto& value) {
    value.fill(0);
    componentString(value.data() + 0x190, "RoadDiscovery");
    componentString(value.data() + 0x1d0, "RoadStyle");
}
void finishNative() {
    *reinterpret_cast<void**>(firstComponent.data() + 0x1f0) = nullptr;
    *reinterpret_cast<void**>(secondComponent.data() + 0x1f0) = nullptr;
}
ULONGLONG tick = 1000;
void pump(ULONGLONG after, bool visible = true) {
    tick += after;
    std::lock_guard lock(mutex);
    pumpNotices(tick, visible);
}
void queue(std::initializer_list<const Achievement*> achievements) {
    queueAchievementNotices(std::span(achievements.begin(), achievements.size()));
}
}

int main() {
    using namespace ach;
    try {
        uiThread = GetCurrentThreadId();
        game = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x2000000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        require(game, "fixture allocation failed");
        FH3ModApi api{};
        api.log = logLine; api.hook_function = hook; api.enable_hooks = enable; api.disable_hooks = disable;
        auto* mod = reinterpret_cast<FH3Mod*>(1);
        require(!installImportNotice(&api, mod, game), "unchecked native code was accepted");
        signatures();
        require(installImportNotice(&api, mod, game), "native HUD hooks failed");
        bindNative();
        initComponent(firstComponent); initComponent(secondComponent);
        onConstruct(firstComponent.data());
        enableImportNotice(true);

        // Enqueueing a new unlock cannot call any native UI on the save worker.
        std::thread worker([] { queue({&Catalog[0]}); });
        worker.join();
        require(displayed.empty() && unlockNotices.size() == 1, "worker called native UI or lost its unlock");
        pump(0, false); pump(2000, false);
        require(displayed.empty(), "native notice escaped the free-roam/foreground gate");
        pump(250); pump(1499);
        require(displayed.empty(), "HUD readiness delay was skipped");
        pump(250);
        require(displayed.size() == 1 && displayed.back().title == "ACHIEVEMENT UNLOCKED" &&
                displayed.back().body == "Welcome to Your Horizon - 10G", "single native notice wording is wrong");
        require(!waiting && unlockNotices.empty(), "successful single unlock was not consumed");

        // Two or more in the same detection batch retain the original grouping.
        queue({&Catalog[0], &Catalog[1]});
        pump(2000); pump(2000);
        require(displayed.size() == 1, "an active game notification was overwritten");
        finishNative(); pump(250);
        require(displayed.size() == 2 && displayed.back().title == "ACHIEVEMENTS UNLOCKED" &&
                displayed.back().body == "2 achievements - 20G", "two simultaneous unlocks were not grouped");
        finishNative();
        queue({&Catalog[0], &Catalog[1], &Catalog[2], &Catalog[3]});
        pump(250); pump(1500);
        require(displayed.size() == 3 && displayed.back().body == "4 achievements - 40G", "group score is wrong");
        finishNative();

        // Distinct detection batches queue in order instead of replacing each other.
        const Achievement unicode{999, Group::base, 15, L"D\u00e9j\u00e0 Vu", L"", false};
        queue({&unicode}); queue({&Catalog[0]});
        pump(250); pump(1500);
        require(displayed.back().body == "D\xc3\xa9j\xc3\xa0 Vu - 15G" && unlockNotices.size() == 1,
                "UTF-8 title or independent batch ordering failed");
        pump(1500);
        require(unlockNotices.size() == 1, "busy native notice discarded a queued unlock");
        finishNative(); pump(250);
        require(displayed.back().body == "Welcome to Your Horizon - 10G" && unlockNotices.empty(), "queued unlock order changed");
        finishNative();

        // Imports keep their once-only acknowledgement and run before live unlocks.
        queueImportNotice({7, 80}); queue({&Catalog[0]});
        pump(250); pump(1500);
        require(importNoticeShown() && displayed.back().body == "7 unlocked from your save - 80G" && waiting,
                "first-run summary or its acknowledgement regressed");
        finishNative(); pump(1500);
        require(unlockNotices.empty() && !waiting, "live unlock behind import did not arrive");
        finishNative();

        // A profile switch discards the previous career's notices.
        queue({&Catalog[0]}); queueImportNotice({});
        pump(250); pump(1500);
        require(!waiting && unlockNotices.empty(), "previous career notice survived profile reset");
        enableImportNotice(false); queue({&Catalog[0]});
        require(unlockNotices.empty(), "pop-ups Off accepted new notices");
        enableImportNotice(true); queue({&Catalog[0]}); enableImportNotice(false);
        pump(2000);
        require(unlockNotices.empty(), "pop-ups Off kept stale live notices");

        // No usable HUD: retain the notice for a later native update.
        enableImportNotice(true); queue({&Catalog[0]}); acquired = false;
        pump(250); pump(1500);
        require(unlockNotices.size() == 1, "missing HUD lost a queued notice");
        acquired = true; accepted = false; pump(250);
        require(unlockNotices.size() == 1, "unaccepted native dispatch lost a queued notice");
        accepted = true; pump(250);
        require(unlockNotices.empty(), "native dispatch was not retried");
        finishNative();

        // Off keeps lifetime watches, so a live HUD remains safe to reuse on On.
        queue({&Catalog[0]}); stopImportNotice(true);
        require(unlockNotices.empty() && activeHooks == std::set<FH3Hook*>{
                    reinterpret_cast<FH3Hook*>(1), reinterpret_cast<FH3Hook*>(2)}, "Off left notification updates active");
        onConstruct(secondComponent.data()); onDestruct(firstComponent.data());
        require(components.size() == 1 && components.front() == secondComponent.data(), "Off lost component lifetimes");
        signatures(); require(installImportNotice(&api, mod, game), "resident On failed"); bindNative();
        require(definitions == 3 && components.size() == 1, "resident On duplicated hooks or lost its existing HUD");
        enableImportNotice(true); queue({&Catalog[0]}); pump(250); pump(1500);
        require(unlockNotices.empty(), "resident On did not use its existing native HUD");
        stopImportNotice();
        onUpdate(nullptr);
        require(inheritedFrames == 1, "disabled notification update skipped the game's original update");
        require(activeHooks.empty() && components.empty(), "full shutdown left native HUD hooks active");
        VirtualFree(const_cast<uint8_t*>(game), 0, MEM_RELEASE);
        std::cout << "PASS: native HUD dispatch, trophy token, wording/UTF-8, 2+ grouping, queued batches, import acknowledgement, busy/retry gates, pop-ups Off, profile reset and resident/full shutdown.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
