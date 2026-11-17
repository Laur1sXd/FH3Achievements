// SPDX-License-Identifier: GPL-3.0-only
#include "../src/events.cpp"
#include "journal.hpp"
#include <iostream>
#include <stdexcept>

namespace {
using namespace ach;
constexpr uintptr_t Base = 0x140000000;
unsigned forwarded = 0, disabled = 0;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
uint32_t nativeWrite(const void*, const void*, uint64_t, uint32_t, const Descriptor*) {
    ++forwarded;
    return 77;
}
FH3Hook* pointerHook(FH3Mod*, const char*, void** slot, void*, void** next) {
    require(reinterpret_cast<uintptr_t>(slot) == Base + 0x2cf01a0, "wrong native event import");
    *next = reinterpret_cast<void*>(nativeWrite);
    return reinterpret_cast<FH3Hook*>(1);
}
int enable(FH3Mod*, FH3Hook*) { return 1; }
int disable(FH3Mod*, FH3Hook*) { ++disabled; return 1; }
void emit(uintptr_t descriptor, uint32_t count, const Descriptor* fields) {
    require(eventWrite(reinterpret_cast<void*>(Base + descriptor), nullptr, 0, count, fields) == 77,
            "native event forwarding changed");
}
std::string cardText(const std::string& page, const char* title) {
    const auto begin = page.find(title);
    require(begin != std::string::npos, "online achievement missing from journal");
    return page.substr(begin, page.find("</Button>", begin) - begin);
}
}

int main() {
    using namespace ach;
    try {
        // Saved completed events, not races entered/won, provide online backfill.
        Snapshot save;
        save.profile["MiscStats/OnlineRacesEntered"] = 100;
        save.profile["CompareStats/OnlineRacesWon"] = 100;
        auto progress = evaluate(save);
        require(progress[37].target == 5 && progress[38].target == 25 &&
                !progress[37].current && !progress[38].current,
                "online entries/wins were counted as completed events");
        save.profile["MiscStats/OnlineEventsFinished"] = 4;
        progress = evaluate(save);
        require(!progress[37].complete() && !progress[38].complete(), "online finish threshold is too low");
        save.profile["MiscStats/OnlineEventsFinished"] = 5;
        progress = evaluate(save);
        require(progress[37].complete() && progress[38].current == 5 && !progress[38].complete(),
                "five saved finishes did not unlock only the first online goal");
        save.profile["MiscStats/OnlineEventsFinished"] = 25;
        progress = evaluate(save);
        require(progress[37].complete() && progress[38].complete(), "25-event online backfill failed");
        save.profile["MiscStats/OnlineEventsFinished"] = -1;
        require(evaluate(save)[37].current == 0, "negative saved counter was accepted");

        FH3ModApi api{};
        api.hook_pointer = pointerHook; api.enable_hooks = enable; api.disable_hooks = disable;
        auto* owner = reinterpret_cast<FH3Mod*>(1);
        require(installEventReader(&api, owner, reinterpret_cast<uint8_t*>(Base)), "native event hook failed");
        uint32_t car = 2751, price = 10000;
        std::array<Descriptor, 7> fields{};
        fields[3] = {reinterpret_cast<uint64_t>(&car), sizeof car, 0};
        fields[4] = {reinterpret_cast<uint64_t>(&price), sizeof price, 0};
        emit(0x4462390, 6, fields.data()); // Wrong descriptor count.
        emit(0x44623c8, 7, nullptr);       // Missing transaction fields.
        auto malformed = fields;
        malformed[4].size = 8;
        emit(0x4462390, 7, malformed.data());
        malformed = fields;
        malformed[3].data = 1;
        emit(0x44623c8, 7, malformed.data());
        emit(0x4462358, 13, nullptr);      // Autoshow purchase is a different goal.
        auto changes = takeEvents();
        require(!changes[35] && !changes[36] && changes[9] == 1,
                "malformed/unrelated auction events unlocked an online goal");

        LiveProgress totals{};
        emit(0x4462390, 7, fields.data());
        for (int i = 0; i < 4; ++i) emit(0x44623c8, 7, fields.data());
        mergeLiveProgress(totals, takeEvents());
        save.profile["MiscStats/OnlineEventsFinished"] = 25;
        progress = evaluate(save);
        addLiveProgress(progress, totals);
        require(progress[36].complete() && progress[35].current == 4 && !progress[35].complete(),
                "auction claim or four-sale progress failed");
        require(progress[37].complete() && progress[38].complete(),
                "live counters overwrote saved online-event progress");
        emit(0x44623c8, 7, fields.data());
        mergeLiveProgress(totals, takeEvents());
        addLiveProgress(progress, totals);
        require(progress[35].current == 5 && progress[35].complete(), "fifth auction sale did not unlock");
        const auto stable = totals;
        mergeLiveProgress(totals, takeEvents());
        require(totals == stable, "draining the same event batch duplicated progress");
        emit(0x4462390, 7, fields.data());
        emit(0x44623c8, 7, fields.data());
        mergeLiveProgress(totals, takeEvents());
        require(totals[35] == 5 && totals[36] == 1, "auction counters exceeded their goals");

        for (const auto& entry : Catalog)
            if (entry.online && !onlineTrackingSupported(entry.id))
                require(!progress[entry.id].target && !progress[entry.id].complete(),
                        "an unverified online achievement became earnable");
        const auto page = journalXaml(progress, {});
        for (const char* title : {"Auction Fever", "5 Star Seller - A+++++", "Welcome to Horizon Online", "Online Adventurer"}) {
            const auto card = cardText(page, title);
            require(card.find("ONLINE ONLY") == std::string::npos && card.find("UNTRACKED") == std::string::npos,
                    "supported online achievement is still hard-disabled in the journal");
        }
        require(cardText(page, "Bring a Friend").find("ONLINE ONLY") == std::string::npos,
                "verified Co-op event tracker was not enabled");
        stopEventReader(&api, owner);
        emit(0x4462390, 7, fields.data());
        emit(0x44623c8, 7, fields.data());
        changes = takeEvents();
        require(disabled == 1 && !changes[35] && !changes[36] && forwarded == 15,
                "Off accepted auction events or failed to forward the game's calls");
        std::cout << "PASS: online finish backfill, 5/25 thresholds, auction claims/sales, typed payload guards, bounded counters, journal status, unverified locks and Off forwarding.\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
