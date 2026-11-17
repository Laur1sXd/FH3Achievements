// SPDX-License-Identifier: GPL-3.0-only
#include "events.hpp"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <optional>
#include <deque>
#include <string>

namespace ach {
namespace {
struct Descriptor {
    uint64_t data;
    uint32_t size;
    uint32_t reserved;
};
using EventWrite = uint32_t (*)(const void*, const void*, uint64_t, uint32_t, const Descriptor*);

const uint8_t* game = nullptr;
FH3Hook* hook = nullptr;
void* original = nullptr;
std::mutex mutex;
EventBatch pending{};
std::deque<std::pair<int, GUID>> rounds;
bool accepting = false;

struct Rule { int id; int target; bool increment; bool pr = false; };
enum class PrKind { jump, drift, speed, zone };
struct PrStunt { uint32_t key; int world; const char* name; PrKind kind; float gold; };
#include "pr_data.inc"
constexpr Rule Rules[] = {
    {44,1,false,true}, {59,1,false,true}, {64,1,false,true}, {86,1,false,true}, {87,1,false,true},
    {110,1,false,true}, {111,1,false,true}, {117,1,false,true}, {129,1,false,true}, {131,1,false,true},
    {7,1,false}, {9,1,false}, {12,1,false}, {13,1,false}, {17,1,false}, {18,10,true},
    {19,4,false}, {26,10,true}, {27,160935,false}, {31,488,false}, {32,5,true},
    {35,5,true}, {36,1,false}, {37,5,true}, {38,25,true}, {39,1,true}, {40,10,true},
    {42,1,false}, {43,1,false}, {47,1,false}, {50,1,false}, {51,1,false}, {63,1,false},
    {67,1,false}, {71,26,false}, {74,27,false}, {75,1,false}, {78,1,false}, {79,1,false},
    {80,50,false}, {83,20,true}, {84,5,true}, {85,1,false}, {88,10,true}, {89,10,true},
    {90,1,false}, {91,1,false}, {92,1,false}, {96,1,false}, {100,1,false}, {104,1,false}, {108,20,true},
    {112,5,true}, {113,10,true}, {114,1,false}, {115,1,false}, {116,1,false}, {118,1,false},
    {124,1,false}, {125,1,false}, {126,1,false}, {127,25,true}, {130,1,false},
    {132,2500,true}, {133,2000,true}, {134,1500,true}, {135,1000,true},
};
struct Route { int track; int route; };
#include "event_data.inc"

template<size_t N> bool contains(const int (&items)[N], int item) {
    return std::ranges::find(items, item) != std::end(items);
}
template<size_t N> bool routeOn(const Route (&items)[N], int route) {
    return std::ranges::any_of(items, [&](const auto& item) { return item.route == route || item.track == route; });
}

template<typename T>
std::optional<T> value(const Descriptor* fields, uint32_t size, uint32_t index) {
    if (index >= size || !fields) return {};
    Descriptor field;
    SIZE_T read;
    if (!ReadProcessMemory(GetCurrentProcess(), fields + index, &field, sizeof field, &read) ||
        read != sizeof field || field.size != sizeof(T)) return {};
    T data;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(field.data), &data,
                           sizeof data, &read) || read != sizeof data) return {};
    return data;
}

void observe(int id, int64_t current, bool increment = false) {
    std::lock_guard lock(mutex);
    if (!accepting) return;
    if (current < 0) return;
    auto& total = pending.counters[id];
    total = increment ? (std::min<int64_t>)(total + current, 160935) : (std::max)(total, current);
}

std::optional<std::wstring> stringValue(const Descriptor* fields, uint32_t count, uint32_t index) {
    Descriptor field{};
    SIZE_T read = 0;
    if (!fields || index >= count ||
        !ReadProcessMemory(GetCurrentProcess(), fields + index, &field, sizeof field, &read) ||
        read != sizeof field || field.size < 2 || field.size > 512 || field.size % 2) return {};
    std::wstring text(field.size / 2, L'\0');
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(field.data),
                           text.data(), field.size, &read) || read != field.size || text.back()) return {};
    text.pop_back();
    if (text.find(L'\0') != text.npos) return {};
    return text;
}
void unique(int id, uint32_t key) {
    if (!validEvidence(id, key)) return;
    std::lock_guard lock(mutex);
    if (accepting) pending.unique[id].insert(key);
}
bool newRound(int domain, const Descriptor* fields, uint32_t count) {
    const auto id = value<GUID>(fields, count, 7);
    if (!id || IsEqualGUID(*id, GUID{})) return false;
    std::lock_guard lock(mutex);
    if (!accepting) return false;
    if (std::ranges::any_of(rounds, [&](const auto& old) {
            return old.first == domain && IsEqualGUID(old.second, *id);
        })) return false;
    rounds.emplace_back(domain, *id);
    if (rounds.size() > 512) rounds.pop_front();
    return true;
}
void raceEnd(uint32_t count, const Descriptor* fields, bool online, bool street = false) {
    auto integer = [&](int index) { return value<int32_t>(fields, count, index).value_or(-1); };
    // Native ExitStatus: 0 undefined, 1 aborted, 2 succeeded, 3 failed,
    // 4 draw, 5 incomplete. A successful result is required, not merely an entry.
    if (count == 13 || integer(9) != 2 || integer(8) <= 0 ||
        (!street && integer(online ? 12 : 17) != 0)) return;
    if ((!online || integer(18) == 0) && newRound(0, fields, count))
        for (int id : {132,133,134,135}) observe(id, 1, true);
    if (!newRound(online ? 2 : street ? 3 : 1, fields, count)) return;
    const auto car = integer(8), place = integer(11);
    const auto seconds = value<float>(fields, count, 10).value_or(-1);
    if (street) {
        if (car == 2729 && place == 1) observe(130, 1);
        return;
    }
    const auto world = integer(online ? 26 : 22);
    const auto route = integer(online ? 19 : 12);
    const bool mountain = world == 553 && (routeOn(MountainRoutes,route) || (!online && contains(MountainRaceEvents,route)));
    const bool island = world == 554 && (routeOn(IslandRoutes,route) || (!online && contains(IslandRaceEvents,route)));
    if (online) {
        const auto rivals = integer(12), playground = integer(18), coop = integer(25);
        if (rivals != 0 || (coop != 0 && coop != 1) || (playground != 0 && playground != 1)) return;
        if (!coop) { observe(37,1,true); observe(38,1,true); }
        if (coop && !playground) { observe(39,1,true); observe(40,1,true); }
        if (mountain && !playground) observe(coop ? 84 : 83,1,true);
        if (island && !playground) observe(108,1,true);
        if (world == 552 && !playground && !coop && route == 3011 && integer(20) >= 0 && integer(20) <= 6)
            unique(45, integer(20));
        // Infection is catalog mode 26 / gameplay mode 5. Require the Playground flag as well.
        if (playground == 1 && (integer(15) == 26 || integer(15) == 5) && car == 2728) observe(125,1);
    }
    if (island && contains(Cars1968, car)) observe(104,1);
    if (island && car == 2751 && place == 1 && integer(online ? 20 : 15) == 4) observe(118,1);
    if (!online && integer(17) == 0) {
        if (mountain && car == 2648 && contains(MountainRushEvents, route) && seconds > 0 && seconds < 120) observe(85,1);
        if (island && car == 2474 && contains(HighSpeedHighwayEvents, route) && seconds > 0 && seconds < 120) observe(114,1);
        if (island && car == 1175 && contains(WheelsGoliathEvents, route) && seconds > 0 && seconds < 390) observe(116,1);
        if (island && contains(WheelsGoliathEvents, route) && place == 1) observe(100,1);
        if (mountain && contains(KingmakerEvents, route) && place == 1) observe(75,1);
    }
}
void skill(uint32_t count, const Descriptor* fields) {
    const auto type = stringValue(fields,count,3);
    const auto grade = value<uint32_t>(fields,count,4);
    const auto car = value<int32_t>(fields,count,6);
    const auto world = value<int32_t>(fields,count,8);
    if (!type || !grade || !car || !world || *grade > 4) return;
    // Grade is zero-based: normal 0, great 1, awesome 2, ultimate 3,
    // Hot Wheels 4. Combo types (Kangaroo, Snowman, FruitSalad) have one grade.
    if (*grade == 3) observe(132,1,true);
    if (*type == L"Kangaroo") observe(134,1,true);
    if (*world == 553 && *type == L"Snowman") {
        observe(135,1,true);
        if (*car == 2662) observe(88,1,true);
    }
    if (*world == 553 && *car == 2663 && *type == L"Air" && *grade == 3) observe(89,1,true);
    if (*world == 554 && *grade == 4) observe(133,1,true);
    if (*world == 554 && *car == 2576 && *type == L"Drift" && *grade == 4) observe(112,1,true);
    if (*world == 554 && *car == 2742 && *type == L"LoopTheLoop" && *grade == 3) observe(113,1,true);
    if (*type == L"BarrelRoll") {
        if (*car == 2656) observe(115,1);
        if (contains(BuggyCars,*car)) observe(63,1);
    }
    if (contains(HooniganCars,*car) && *type == L"FruitSalad") observe(124,1);
    if (*car == 2732 && *type == L"Drift" && *grade == 3) observe(127,1,true);
}

uint32_t eventWrite(const void* descriptor, const void* provider, uint64_t handle,
                    uint32_t count, const Descriptor* fields) {
    // Compare addresses of the validated game's exact descriptors. The payload is
    // read locally and only numeric counters are retained. Forward the original call.
    const auto address = reinterpret_cast<uintptr_t>(descriptor);
    try {
    if (address >= reinterpret_cast<uintptr_t>(game)) {
        const auto rva = address - reinterpret_cast<uintptr_t>(game);
        switch (rva) {
        case 0x4461ef8: if (count == 32) raceEnd(count,fields,true); break;
        case 0x4461f68: if (count == 26) raceEnd(count,fields,false); break;
        case 0x4463ac0: if (count == 13) raceEnd(count,fields,false); break;
        case 0x44640a8: if (count == 23) raceEnd(count,fields,false,true); break;
        case 0x4463ee8: if (count == 10) skill(count,fields); break;
        case 0x4463f58: // SkillChainCompleted: base points * multiplier, Track and car.
            if (count == 15) {
                const auto base = value<int32_t>(fields,count,3).value_or(0);
                const auto multiplier = value<float>(fields,count,5).value_or(0);
                const auto car = value<int32_t>(fields,count,10).value_or(0);
                const double score = static_cast<double>(base)*multiplier;
                if (base > 0 && multiplier >= 1 && multiplier <= 20 && std::isfinite(score)) {
                    if (car == 2715 && value<int32_t>(fields,count,6) == 553 && score >= 100000) observe(91,1);
                    if (car == 2730 && score >= 300000) observe(126,1);
                }
            }
            break;
        case 0x44629b0: // Completed Exhibition, including blueprint ownership.
        case 0x4463510: // Mountain completion also provides HadBlizzard.
            if ((rva == 0x44629b0 && count == 15) || (rva == 0x4463510 && count == 19)) {
                const auto blueprint = value<uint32_t>(fields,count,4);
                const auto author = value<uint32_t>(fields,count,5);
                if (blueprint == 1 && author && *author <= 1) observe(*author ? 12 : 13,1);
                if (blueprint == 1 && author == 1 && value<int32_t>(fields,count,6) == 25 &&
                    value<int32_t>(fields,count,7) == 6 && value<uint32_t>(fields,count,8) == 1) observe(51,1);
                if (rva == 0x4463510 && value<int32_t>(fields,count,14) == 553) {
                    if (blueprint == 1 && author == 1 && value<uint32_t>(fields,count,15) == 1) observe(79,1);
                    const auto tyres = value<int32_t>(fields,count,16);
                    if (value<int32_t>(fields,count,3) == 1 && tyres && *tyres >= 0 && *tyres <= 9 && *tyres != 7)
                        observe(92,1);
                }
            }
            break;
        case 0x4462470: // ChampionshipCompleted.
            if (count == 16) {
                if (value<uint32_t>(fields,count,4) == 1 && value<uint32_t>(fields,count,5) == 1) {
                    if (value<uint32_t>(fields,count,6) == 1) observe(42,1);
                    if (value<int32_t>(fields,count,7) == 10 && value<int32_t>(fields,count,8).value_or(0) > 160934)
                        observe(50,1);
                }
                const auto car = value<int32_t>(fields,count,9);
                if (car && contains(HooniganCars,*car)) unique(122,*car);
            }
            break;
        case 0x4462198:
        case 0x44633f8: // Bucket List end: completion and UGC ownership are separate flags.
            if (count == 21 && value<uint32_t>(fields,count,15) == 1) {
                const auto author = value<uint32_t>(fields,count,17);
                if (author == 1 && value<uint32_t>(fields,count,6) == 1) observe(17,1);
                if (author == 0) observe(18,1,true);
            }
            break;
        case 0x44634d8: // MountainCompletedEvent: never accept a mainland/island result here.
            if (count == 29 && value<int32_t>(fields,count,16) == 554 &&
                routeOn(IslandRoutes,value<int32_t>(fields,count,9).value_or(-1)) &&
                value<int32_t>(fields,count,20) == 3) observe(96,1);
            if (count == 29 && value<int32_t>(fields,count,16) == 553) {
                if (value<int32_t>(fields,count,20) == 3) observe(67,1);
                observe(71,value<int32_t>(fields,count,21).value_or(0));
                if (value<int32_t>(fields,count,12) == 1 && value<int32_t>(fields,count,11) == 2623 &&
                    contains(BlizzardWeather,value<int32_t>(fields,count,22).value_or(-1))) observe(90,1);
            }
            break;
        case 0x44632e0: // Cumulative mountain-specific career progress.
            if (count == 10) {
                observe(71,value<int32_t>(fields,count,5).value_or(0));
                observe(74,value<int32_t>(fields,count,6).value_or(0));
            }
            break;
        case 0x4463d60: if (count == 5) observe(31,value<int32_t>(fields,count,3).value_or(0)); break;
        case 0x44635f0: if (count == 5) observe(80,value<int32_t>(fields,count,3).value_or(0)); break;
        case 0x4462da0: if (count == 5) observe(47,1); break; // LaunchedForzaHub, not just viewing its tile.
        case 0x4462e80: if (count == 7) unique(54,0); break;
        case 0x44642a0: if (count == 6) unique(54,1); break;
        case 0x4463a18: if (count == 6 && value<int32_t>(fields,count,3).value_or(0) > 0) unique(54,2); break;
        case 0x4464380:
            if (count == 8) {
                const auto brand = value<uint32_t>(fields,count,5);
                if (brand && (*brand == 660 || *brand == 661)) unique(55,*brand);
            }
            break;
        case 0x44628d0: // DrivatarHired: IsSwap.
            if (count == 7 && value<uint32_t>(fields, count, 3).value_or(0) == 1) observe(7, 1);
            break;
        case 0x4462358: // CarPurchased (separate from CarAddedToGarage and auction purchases).
            if (count == 13) {
                observe(9, 1);
                if (value<uint32_t>(fields,count,5) == 1 && value<int32_t>(fields,count,7).value_or(0) >= 1000000)
                    observe(43,1);
            }
            break;
        case 0x4462390: // CarPurchasedInAuction: two typed DWORD event fields after the common header.
            if (count == 7 && value<uint32_t>(fields, count, 3) && value<uint32_t>(fields, count, 4))
                { observe(36, 1); if (value<int32_t>(fields,count,3).value_or(0) >= 1000000) observe(43,1); }
            break;
        case 0x44623c8: // CarSoldInAuction: successful sale/collection, not listing a car.
            if (count == 7 && value<uint32_t>(fields, count, 3) && value<uint32_t>(fields, count, 4))
                observe(35, 1, true);
            break;
        case 0x4463ba0: // RadioStationUnlocked: station name then cumulative UnlockCount.
            if (count == 7) observe(19, value<int32_t>(fields, count, 4).value_or(0));
            break;
        case 0x4462668: // ConvoyRaceEnded.
            if (count == 10) observe(26, 1, true);
            break;
        case 0x4462898: // DistanceDrivenInConvoy: cumulative distance in metres.
            if (count == 12) {
                const float distance = value<float>(fields, count, 3).value_or(0);
                if (std::isfinite(distance) && distance >= 0)
                    observe(27, static_cast<int64_t>((std::min)(distance, 160935.f)));
            }
            break;
        case 0x4463fc8: // SkillSongsParticipated: one event per participation.
            if (count == 5) observe(32, 1, true);
            break;
        case 0x44633c0: // MountainBucketListChallengeCreated.
            if (count == 8) observe(78, 1);
            break;
        }
    }
    } catch (...) { /* Tracking must never interrupt the game's native event path. */ }
    return reinterpret_cast<EventWrite>(original)(descriptor, provider, handle, count, fields);
}
}

bool hasFh3mpInstallation(const fs::path& install) {
    std::error_code error;
    const auto file = [&](const fs::path& path) { return fs::is_regular_file(path, error); };
    // Encore 0.1.1 installs a bcrypt shim beside Azimuth, not FH2MP.dll.
    // A generic bcrypt.dll alone is not evidence of FH3MP.
    return file(install / L"fh3-mods" / L"FH3MP.dll") ||
           (file(install / L"bcrypt.dll") && file(install / L"fh3mp-shim.ini") &&
            file(install / L"fh3mp-cloud.exe"));
}

bool installEventReader(const FH3ModApi* api, FH3Mod* mod, const uint8_t* base) {
    game = base;
    if (!api->hook_pointer || !api->enable_hooks) return false;
    auto* slot = const_cast<uint8_t*>(base) + 0x2cf01a0;
    if (!hook) hook = api->hook_pointer(mod, "FH3 achievement events (EtxEventWrite)", reinterpret_cast<void**>(slot),
                             reinterpret_cast<void*>(eventWrite), &original);
    const bool installed = hook && original && api->enable_hooks(mod, hook);
    std::lock_guard lock(mutex);
    accepting = installed;
    return installed;
}
void stopEventReader(const FH3ModApi* api, FH3Mod* mod) {
    {
        std::lock_guard lock(mutex);
        accepting = false;
    }
    if (hook) api->disable_hooks(mod, hook);
}

EventBatch takeEventBatch() {
    std::lock_guard lock(mutex);
    EventBatch result;
    std::swap(result,pending);
    return result;
}
LiveProgress takeEvents() { return takeEventBatch().counters; }
bool EventBatch::empty() const {
    return std::ranges::all_of(counters, [](int64_t n) { return n == 0; }) &&
           std::ranges::all_of(unique, [](const auto& entries) { return entries.empty(); });
}
bool validEvidence(int id, uint32_t key) {
    switch (id) {
    case 62: case 123: return key < std::size(PrStunts);
    case 128: return key < std::size(PrStunts) && PrStunts[key].world == 552 &&
        (std::string_view(PrStunts[key].name) == "23_A11_Goldmine" ||
         std::string_view(PrStunts[key].name) == "11_A23_Shipwreck" ||
         std::string_view(PrStunts[key].name) == "25_A35_PlaneJump");
    case 45: return key <= 6;
    case 54: return key <= 2;
    case 55: return key == 660 || key == 661;
    case 122: return contains(HooniganCars,static_cast<int>(key));
    default: return false;
    }
}
void addLiveProgress(ProgressList& progress, const LiveProgress& values) {
    for (const auto& rule : Rules) {
        if (rule.pr) continue;
        auto& item = progress[rule.id];
        item = {(std::max)(item.current, std::clamp<int64_t>(values[rule.id],0,rule.target)), rule.target};
    }
}
void mergeLiveProgress(LiveProgress& totals, const LiveProgress& changes) {
    for (const auto& rule : Rules) {
        auto& total = totals[rule.id];
        total = rule.increment ? (std::min<int64_t>)(rule.target,total+changes[rule.id])
                               : (std::max)(total,changes[rule.id]);
    }
}
void mergeEventBatch(EventBatch& totals, const EventBatch& changes) {
    mergeLiveProgress(totals.counters, changes.counters);
    for (size_t id=1; id<totals.unique.size(); ++id)
        for (auto key : changes.unique[id])
            if (validEvidence(static_cast<int>(id),key)) totals.unique[id].insert(key);
}
void addEventProgress(ProgressList& progress, const EventBatch& values, bool prSupported) {
    addLiveProgress(progress,values.counters);
    if (prSupported)
        for (const auto& rule : Rules) if (rule.pr)
            progress[rule.id] = {std::clamp<int64_t>(values.counters[rule.id],0,rule.target),rule.target};
    for (const auto [id,target] : {std::pair{45,7}, {54,3}, {55,2}, {122,7}, {62,5}, {123,20}, {128,3}}) {
        if ((id==62 || id==123 || id==128) && !prSupported) continue;
        auto& item = progress[id];
        item = {(std::max)(item.current,static_cast<int64_t>(values.unique[id].size())),target};
    }
}

void recordPrResult(int world, std::string_view name, int car, int carClass, float kilograms, float score, float gold) {
    if (!std::isfinite(score) || score < 0 || !std::isfinite(gold) || gold <= 0 || car <= 0) return;
    const auto stunt = std::ranges::find_if(PrStunts,[&](const auto& entry) { return entry.world==world && entry.name==name; });
    if (stunt==std::end(PrStunts) || std::fabs(stunt->gold-gold)>0.02f) return;
    const bool threeStars = score >= gold;
    const bool jump = stunt->kind==PrKind::jump, drift = stunt->kind==PrKind::drift;
    if (world==552 && jump && name=="25_A35_PlaneJump" && car==2563 && score>=91.44f) observe(44,1);
    if (threeStars) {
        if (drift && std::isfinite(kilograms) && kilograms>=2500) observe(59,1);
        if (car==2397) unique(62,stunt->key);
        if (carClass==1) observe(64,1);
        if (contains(HooniganCars,car)) unique(123,stunt->key);
        if (car==2733 && world==552) unique(128,stunt->key);
        if (car==2734 && world==552 && name=="17_A17_SurfersParkJump") observe(129,1);
        if (car==2736 && world==552 && name=="drift_zone_08") observe(131,1);
    }
    if (world==553) {
        if (jump && car==2660 && score>=251.46f) observe(86,1);
        if (drift && car==2661 && score>=35000) observe(87,1);
    }
    if (world==554) {
        if (drift && car==2750 && score>50000) observe(110,1);
        if (jump && car==2575 && score>152.4f) observe(111,1);
        if (stunt->kind==PrKind::speed && car==2581 && score>=89.408f) observe(117,1);
    }
}
}
