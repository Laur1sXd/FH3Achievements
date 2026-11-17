// SPDX-License-Identifier: GPL-3.0-only
#include "rules.hpp"
#include "sqlite3.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <set>
#include <stdexcept>

namespace ach {
namespace {
enum class StuntKind { jump, drift, speed };
struct Stunt {
    const char* state;
    StuntKind kind;
    double bronze;
    double gold;
};

#include "rule_data.inc"

struct StuntTotals {
    int starred = 0;
    int threeStars = 0;
    bool goldJump = false;
    bool goldDrift = false;
};

StuntTotals stunts(const Snapshot& save, std::span<const Stunt> entries) {
    StuntTotals result;
    for (const auto& entry : entries) {
        const auto text = save.get(entry.state, "PersonalBest");
        if (text.empty()) continue;
        double value = 0;
        const auto [end, err] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (err != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) continue;
        if (value >= entry.bronze) ++result.starred;
        if (value >= entry.gold) {
            ++result.threeStars;
            result.goldJump |= entry.kind == StuntKind::jump;
            result.goldDrift |= entry.kind == StuntKind::drift;
        }
    }
    return result;
}

class Career {
    sqlite3* db = nullptr;
public:
    explicit Career(Bytes bytes) {
        if (bytes.empty()) return;
        if (sqlite3_open_v2(":memory:", &db, SQLITE_OPEN_CREATE | SQLITE_OPEN_READWRITE | SQLITE_OPEN_PRIVATECACHE,
                            nullptr) == SQLITE_OK &&
            sqlite3_deserialize(db, "main", const_cast<unsigned char*>(bytes.data()),
                                bytes.size(), bytes.size(), SQLITE_DESERIALIZE_READONLY) == SQLITE_OK) {
            sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
            sqlite3_db_config(db, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr);
            return;
        }
        sqlite3_close(db);
        db = nullptr;
    }
    ~Career() { sqlite3_close(db); }
    Career(const Career&) = delete;
    Career& operator=(const Career&) = delete;
    int64_t total(const char* query) const {
        if (!db) return 0;
        sqlite3_stmt* stmt = nullptr;
        int64_t result = 0;
        if (sqlite3_prepare_v2(db,query,-1,&stmt,nullptr) == SQLITE_OK &&
            sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt,0) == SQLITE_INTEGER)
            result = std::clamp<int64_t>(sqlite3_column_int64(stmt,0),0,100000000);
        sqlite3_finalize(stmt);
        return result;
    }
    std::set<int> ids(const char* query) const {
        std::set<int> result;
        if (!db) return result;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, query, -1, &stmt, nullptr) == SQLITE_OK) {
            int status;
            while ((status = sqlite3_step(stmt)) == SQLITE_ROW)
                result.insert(sqlite3_column_int(stmt, 0));
            if (status != SQLITE_DONE) result.clear();
        }
        sqlite3_finalize(stmt);
        return result;
    }
};

int64_t bounded(int64_t value, int64_t max) { return std::clamp<int64_t>(value, 0, max); }

constexpr int AustraliaBarns[] = {326, 444, 1063, 1278, 2176, 2416, 2417, 2504,
                                  2505, 2506, 2510, 2556, 2559, 2563, 2566};
constexpr const char* Festivals[] = {"byron_bay_festival_save_state", "surfers_paradise_festival_save_state",
                                    "yarra_valley_festival_save_state", "outback_festival_save_state"};

int64_t matching(std::span<const int> want, const std::set<int>& have) {
    return std::ranges::count_if(want, [&](int id) { return have.contains(id); });
}
}

ProgressList evaluate(const Snapshot& save) {
    ProgressList result{};
    auto put = [&](int id, int64_t current, int64_t target = 1) {
        result[id] = {bounded(current, target), target};
    };
    auto stat = [&](int id, const char* key, int64_t target = 1) {
        put(id, save.stat(key), target);
    };
    auto built = [&](const char* site) { return save.get(site, "Built") == "true"; };
    auto level = [&](const char* site) { return built(site) ? save.value(site, "Level") : 0; };
    const auto firstGameComplete = save.get("iestate", "IEState") == "Complete";
    const auto exhibitions = save.stat("CompareStats/CampaignSlotExhibitionsCompleted");
    const auto exhibitionsWon = save.stat("CompareStats/CarChampionshipEventsWon");
    const auto raced = save.stat("MiscStats/CarChampionshipEventsRaced");
    const auto streak = bounded(save.stat("MiscStats/StreetRacesWon"), 16) +
                        bounded(save.stat("MiscStats/MidnightBattlesWon"), 4);
    const auto pr = stunts(save, AustraliaStunts);
    const auto hotPr = stunts(save, IslandStunts);
    const Career career(save.database);
    const auto perks = career.ids("SELECT UnlockId FROM PerkUnlock");
    const auto garage = career.ids("SELECT CarId FROM Career_Garage");
    const auto restored = career.ids(
        "SELECT b.CarOrdinal FROM BarnFinds b JOIN Career_Garage g ON lower(g.Guid) = lower(b.VIN) "
        "WHERE b.State > 0 AND b.CarOrdinal = g.CarId");
    int64_t upgraded = 0, maxed = 0, maxLevel = 0;
    for (const auto* site : Festivals) {
        const auto siteLevel = level(site);
        upgraded += bounded(siteLevel - 1, 4);
        maxed += siteLevel >= 5;
        maxLevel = (std::max)(maxLevel, siteLevel);
    }

    put(1, firstGameComplete || save.stat("CompareStats/ShowcasesWon") > 0);
    put(2, upgraded);
    put(3, exhibitions > 0 && raced >= exhibitions && exhibitionsWon > raced - exhibitions);
    put(4, pr.goldJump);
    put(5, pr.goldDrift);
    stat(6, "MiscStats/DrivatarLineupCount");
    put(8, built(Festivals[1]));
    stat(10, "CompareStats/CarChampionshipsWon");
    stat(11, "MiscStats/StreetRacesWon");
    stat(14, "MiscStats/MidnightBattlesWon");
    stat(15, "CompareStats/BarnFinds");
    put(16, matching(AustraliaBarns, restored), std::size(AustraliaBarns));
    put(20, built(Festivals[2]));
    put(21, built(Festivals[3]));
    put(22, maxLevel >= 5);
    put(23, upgraded, 10);
    put(24, maxed, std::size(Festivals));
    stat(25, "CompareStats/ShowcasesWon", 5);
    stat(28, "CompareStats/BucketlistEventsCompleted", 30);
    stat(29, "CompareStats/BucketlistEventsCompleted", 5);
    put(30, bounded(save.stat("CompareStats/FastTravelBoardsSmashed"), 50) +
            bounded(save.stat("CompareStats/SmallValueBoardsSmashed"), 50) +
            bounded(save.stat("CompareStats/MediumValueBoardsSmashed"), 35) +
            bounded(save.stat("CompareStats/HighValueBoardsSmashed"), 15), 150);
    stat(31, "CompareStats/RoadsDiscovered", 488);
    put(33, perks.contains(45) || perks.contains(61) || perks.contains(77));
    put(34, matching(PermanentPerks, perks), std::size(PermanentPerks));
    // The game persists finishes across Online Adventure and Free Roam here.
    // Do not count races entered, podiums or wins as completed online events.
    stat(37, "MiscStats/OnlineEventsFinished", 5);
    stat(38, "MiscStats/OnlineEventsFinished", 25);
    put(43, career.ids("SELECT CarId FROM Career_Garage WHERE PricePaid >= 1000000").size());
    put(55, matching(std::array{660,661}, career.ids("SELECT ManufacturerId FROM Career_PurchasedParts")), 2);
    put(41, career.ids("SELECT CarOrdinal FROM PhotoCaptures").size(), 300);
    put(46, matching(HorizonEditionCars, garage));
    stat(48, "Main/Level", 20);
    stat(49, "Main/Level", 150);
    put(52, exhibitions, 63);
    stat(53, "CompareStats/CampaignSlotChampionshipsCompleted", 63);
    put(56, streak, 20);
    put(57, pr.starred, 50);
    put(58, pr.threeStars, std::size(AustraliaStunts));
    stat(60, "MiscStats/BeautySpotsDiscovered", 13);
    put(61, career.ids("SELECT CarId FROM Career_Garage WHERE CarId = 2616 AND NumVictories > 0").size());

    constexpr auto Mountain = "blizzard_mountain_festival_save_state";
    put(65, built(Mountain));
    stat(66, "Mountain/Stars");
    stat(68, "Mountain/Stars", 5);
    stat(69, "Mountain/Stars", 90);
    stat(70, "Mountain/Stars", 160);
    stat(71, "Mountain/KingOfTheMountainEventsCompleted", 26);
    stat(74, "Mountain/ChampionshipsCompleted", 27);
    put(72, restored.contains(2558));
    stat(73, "Mountain/ChampionshipsCompleted");
    put(75,career.ids("SELECT r.RaceId FROM Career_RaceResults r JOIN CareerRaces c ON c.CareerRaceId=r.RaceId WHERE c.TrackId=1212 AND r.Place=1 AND r.TimesCompleted>0").size());
    stat(76, "Mountain/BonusBoardsSmashed", 25);
    stat(77, "Mountain/BeautySpotsDiscovered", 5);
    stat(80, "Mountain/RoadsDiscovered", 50);
    stat(81, "Mountain/BucketlistEventsCompleted");
    stat(82, "Mountain/BucketlistEventsCompleted", 7);

    constexpr auto Wheels = "opus_island_festival_save_state";
    put(93, save.get("iestateopusisland", "IEState") == "Complete");
    stat(94, "OpusIsland/ExhibitionEventsCompleted");
    put(95, career.ids("SELECT CareerRaceId FROM CareerRacesStuntTrackPieces").size());
    put(97, level(Wheels) >= 2);
    put(98, level(Wheels) >= 4);
    put(99, level(Wheels) >= 6);
    put(96,save.stat("OpusIsland/Stars")>=177);
    put(100,career.ids("SELECT r.RaceId FROM Career_RaceResults r JOIN CareerRaces c ON c.CareerRaceId=r.RaceId WHERE c.TrackId=1242 AND r.Place=1 AND r.TimesCompleted>0").size());
    stat(101, "OpusIsland/ExhibitionEventsCompleted", 28);
    put(102, hotPr.threeStars, std::size(IslandStunts));
    stat(103, "OpusIsland/Stars", 177);
    put(105, restored.contains(2372));
    stat(106, "OpusIsland/BucketlistEventsCompleted");
    stat(107, "OpusIsland/BucketlistEventsCompleted", 5);
    stat(109, "OpusIsland/BonusBoardsSmashed", 25);
    stat(119, "OpusIsland/RoadsDiscovered", 141);
    stat(120, "OpusIsland/ChampionshipsCompleted");
    stat(121, "OpusIsland/ChampionshipsCompleted", 28);
    put(126, career.ids("SELECT CarId FROM Career_Garage WHERE CarId = 2730 AND HighestSkillScore >= 300000").size());
    // RaceResults records completions, unlike Main/NumRaces and RacesEntered.
    // Preserve exact combined alternatives documented by Forza Support.
    const auto finishes = career.total("SELECT SUM(TimesCompleted) FROM Career_RaceResults WHERE TimesCompleted > 0");
    int64_t ultimate = 0;
    for (const auto* key : {"Main/NumUltimateWreckages", "Main/NumUltimateJTurns", "Main/NumUltimateSpeeds",
             "Main/NumUltimateEDrifts", "Main/NumUltimateDrafts", "Main/NumUltimateNearMisses",
             "Main/NumUltimateAirs", "Main/NumUltimatePasses", "Main/NumUltimateOneEighties",
             "Main/NumUltimateTwoWheels", "Main/NumUltimateDrifts", "Main/NumUltimateBurnouts"})
        ultimate += bounded(save.stat(key),2500);
    put(132,finishes+ultimate,2500);
    put(133,finishes,2000);
    put(134,finishes,1500);
    put(135,finishes+bounded(save.stat("Mountain/SnowmenSmashed"),1000),1000);
    return result;
}

ProgressList initialProgress() { return evaluate({}); }
}
