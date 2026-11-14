// SPDX-License-Identifier: GPL-3.0-only
#include "fh3mod.h"
#include "events.hpp"
#include "import_notice.hpp"
#include "journal.hpp"
#include "overlay.hpp"
#include "runtime.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <ctime>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace {
using namespace ach;
constexpr wchar_t Section[] = L"Achievements";
constexpr char Version[] = "1.0.0";

const FH3ModApi* api = nullptr;
FH3Mod* mod = nullptr;
fs::path root, settings, saves;
std::thread worker;
std::atomic<bool> stopping = false, popups = true;

void log(const std::string& line) { api->log(mod, line.c_str()); }

void writeSetting(const wchar_t* key, uint32_t value) {
    WritePrivateProfileStringW(Section, key, std::to_wstring(value).c_str(), settings.c_str());
}

void onPopups(void*, const char*, int value) {
    popups = value != 0;
    enableImportNotice(popups);
    writeSetting(L"Popups", value != 0);
}

bool loadRequested(const fs::path& install) {
    // The compatibility loader registers opt-in mods even when they start off,
    // so their startup-only scene can be mounted before the first F9 enable.
    const auto file = install / L"fh3-mods" / L"platform-disabled.txt";
    if (!fs::exists(file)) return true;
    std::ifstream in(file);
    if (!in) throw std::runtime_error("cannot read the platform Load setting");
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (first && line.starts_with("\xef\xbb\xbf")) line.erase(0, 3);
        first = false;
        const auto begin = line.find_first_not_of(" \t\r");
        if (begin == std::string::npos || line[begin] == '#') continue;
        line = line.substr(begin, line.find_last_not_of(" \t\r") - begin + 1);
        std::ranges::transform(line, line.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (line == "fh3achievements.dll") return false;
    }
    if (!in.eof()) throw std::runtime_error("cannot finish reading the platform Load setting");
    return true;
}
struct Record {
    uint64_t revision = 0;
    Unlocks unlocks{};
    LiveProgress live{};
    bool needsImportNotice = true;
    ImportSummary import{};
};

Record loadRecord(const fs::path& path) {
    Record result;
    if (!fs::exists(path)) return result;
    const auto bytes = readFile(path, 32 * 1024);
    std::istringstream in(std::string(bytes.begin(), bytes.end()));
    std::string kind;
    if (!(in >> kind >> std::hex >> result.revision >> std::dec) || (kind != "FH3ACH2" && kind != "FH3ACH3"))
        throw std::runtime_error("invalid local achievement record");
    std::string line;
    std::getline(in, line);
    if (kind == "FH3ACH3") {
        std::istringstream header(line);
        auto& [count, score] = result.import;
        std::string tail;
        if (!(header >> count >> score) || (header >> tail) || count < 0 || count > std::ssize(Catalog) ||
            score < 0 || score > 3000 || (count == 0) != (score == 0))
            throw std::runtime_error("invalid first-run achievement summary");
        result.needsImportNotice = false;
    }
    std::array<bool, std::size(Catalog) + 1> seen{};
    int id;
    int64_t date, current;
    while (std::getline(in, line)) {
        std::istringstream entry(line);
        std::string tail;
        if (!(entry >> id >> date >> current) || (entry >> tail))
            throw std::runtime_error("invalid entry in local achievement record");
        if (id < 1 || id >= static_cast<int>(result.unlocks.size()) ||
            date < 0 || current < 0 || current > 160935 || seen[id] || Catalog[id - 1].online)
            throw std::runtime_error("invalid entry in local achievement record");
        seen[id] = true;
        result.unlocks[id] = date;
        result.live[id] = current;
    }
    if (!in.eof()) throw std::runtime_error("could not read the local achievement record");
    return result;
}

bool replaceFile(const fs::path& path, const std::string& text) {
    const auto temporary = fs::path(path.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp");
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool complete = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                          written == text.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (complete && MoveFileExW(temporary.c_str(), path.c_str(),
                                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    DeleteFileW(temporary.c_str());
    return false;
}

bool persist(const fs::path& path, const Record& record) {
    std::ostringstream out;
    out << "FH3ACH3 " << std::hex << record.revision << std::dec << ' '
        << record.import.count << ' ' << record.import.score << '\n';
    for (const auto& item : Catalog) {
        const auto stamp = record.unlocks[item.id], current = record.live[item.id];
        if (stamp || current) out << item.id << ' ' << stamp << ' ' << current << '\n';
    }
    return replaceFile(path, out.str());
}

struct WorkerState {
    ProgressList progress = initialProgress();
    Record record;
    fs::path currentCommit;
    std::wstring profile;
    bool synced = false;
    std::string lastError;
    LiveProgress pending{};

    void acknowledgeImportNotice() {
        if (!record.import.count || preserveLedger || !importNoticeShown()) return;
        auto next = record;
        next.import = {};
        if (!persist(root / (profile + L".txt"), next))
            throw std::runtime_error("cannot acknowledge the first-run achievement summary");
        record = next;
    }

    void refresh(const uint8_t* game, Overlay& overlay, bool eventsEnabled) {
        mergeLiveProgress(pending, takeEvents());
        auto revision = latestRevision(saves);
        if (!revision) {
            if (!currentCommit.empty()) {
                progress = initialProgress();
                if (eventsEnabled) addLiveProgress(progress, {});
                record = {};
                publishJournal(progress, record.unlocks);
                currentCommit.clear();
                profile.clear();
                overlay.reset();
                queueImportNotice({});
                synced = false;
                pending.fill(0);
            }
            return;
        }
        const bool saveChanged = revision->commit != currentCommit;
        const bool eventsChanged = std::ranges::any_of(pending, [](int64_t n) { return n != 0; });
        if (!saveChanged && !eventsChanged) return;

        // Read and evaluate the whole committed revision before changing the journal.
        auto nextProgress = progress;
        bool olderRevision = false;
        if (saveChanged) {
            try {
                nextProgress = evaluate(readSave(game, *revision));
            } catch (const std::exception&) {
                // At startup Azimuth can still have an older complete revision.
                // Once synchronized, keep the displayed progress and retry the new file.
                if (synced) throw;
                auto older = latestRevision(saves, revision->number);
                if (!older || older->profile != revision->profile) throw;
                nextProgress = evaluate(readSave(game, *older));
                revision = std::move(older);
                olderRevision = true;
            }
        }
        const auto recordPath = root / (revision->profile + L".txt");
        const bool changedProfile = profile != revision->profile ||
                                    (synced && revision->number < record.revision);
        Record next;
        bool writable = true;
        if (changedProfile) {
            try {
                next = loadRecord(recordPath);
            } catch (const std::exception& error) {
                // Keep an unreadable ledger for recovery. Save-derived results are still visible.
                log(error.what());
                writable = false;
            }
            if (!olderRevision && revision->number < next.revision) next = {};
        } else {
            next = record;
            writable = !preserveLedger;
        }
        if (synced && changedProfile) pending.fill(0);
        mergeLiveProgress(next.live, pending);
        const bool summarizeImport = next.needsImportNotice && popups && writable;
        std::array<bool, std::size(Catalog) + 1> inImport{};
        if (summarizeImport) {
            for (const auto& item : Catalog) {
                if (item.online || !nextProgress[item.id].complete()) continue;
                inImport[item.id] = true;
                ++next.import.count;
                next.import.score += item.gamerscore;
            }
        }
        next.needsImportNotice = false;
        if (eventsEnabled) addLiveProgress(nextProgress, next.live);
        next.revision = olderRevision ? (std::max)(next.revision, revision->number) : revision->number;
        const auto timestamp = (std::max<int64_t>)(1, std::time(nullptr));
        std::vector<const Achievement*> unlockedNow;
        std::vector<const Achievement*> popupUnlocks;
        int gained = 0, popupScore = 0;
        for (const auto& item : Catalog) {
            if (item.online || next.unlocks[item.id] || !nextProgress[item.id].complete()) continue;
            next.unlocks[item.id] = timestamp;
            unlockedNow.push_back(&item);
            gained += item.gamerscore;
            if (!inImport[item.id]) {
                popupUnlocks.push_back(&item);
                popupScore += item.gamerscore;
            }
        }
        if (writable && !persist(recordPath, next))
            throw std::runtime_error("cannot write the local achievement record");
        pending.fill(0);

        if (changedProfile) {
            overlay.reset();
            queueImportNotice(next.import);
        }
        if (popups && !popupUnlocks.empty()) {
            if (popupUnlocks.size() == 1)
                overlay.toast(popupUnlocks.front()->title, L"+" + std::to_wstring(popupScore) + L" G");
            else
                overlay.toast(std::to_wstring(popupUnlocks.size()) + L" achievements earned",
                              L"+" + std::to_wstring(popupScore) + L" G");
        }
        if (!synced || changedProfile || !unlockedNow.empty()) {
            std::ostringstream line;
            line << "Read save revision " << std::hex << revision->number << std::dec
                 << "; level " << readLevel(nextProgress) << "; "
                 << unlockedNow.size() << " new local unlocks, +" << gained << "G";
            log(line.str());
        }
        progress = nextProgress;
        record = next;
        publishJournal(progress, record.unlocks);
        profile = revision->profile;
        currentCommit = revision->commit;
        synced = true;
        preserveLedger = !writable;
    }

private:
    bool preserveLedger = false;
    static int64_t readLevel(const ProgressList& next) { return next[49].current; }
};

void run(HINSTANCE instance) {
    try {
        const uint8_t* game = nullptr;
        for (int i = 0; i < 120 && !stopping; ++i) {
            game = static_cast<const uint8_t*>(api->game_base());
            if (game) break;
            Sleep(500);
        }
        if (stopping) return;
        if (!supportedGame(game)) {
            log("FH3 Xbox One 1.0.125.0 was not found. Achievement tracking is idle.");
            return;
        }
        struct Lease {
            HANDLE file;
            ~Lease() { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); }
        } lease{CreateFileW((root / L"session.lock").c_str(), GENERIC_WRITE, 0, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (lease.file == INVALID_HANDLE_VALUE) {
            log("Cannot open the achievement journal for writing. Another session may be using it.");
            return;
        }
        bool eventsEnabled = false;
        bool codeReady = false;
        for (int waited = 0; !stopping && api->wait_for_code && waited < 8000; waited += 250) {
            if (api->wait_for_code(250) == game) { codeReady = true; break; }
        }
        if (stopping) return;
        if (codeReady) {
            eventsEnabled = installEventReader(api, mod, game);
            log(installImportNotice(api, mod, game) ? "First-run achievement HUD ready to wait for free roam." :
                                                    "First-run achievement HUD notification unavailable.");
        }
        log(eventsEnabled ? "Watching FH3 achievement events locally." :
                            "Live events unavailable. Save-backed achievements are active.");
        WorkerState state;
        if (eventsEnabled) addLiveProgress(state.progress, {});
        publishJournal(state.progress, state.record.unlocks);
        Overlay overlay(instance);
        bool journalLinked = false;
        if (codeReady) log("Waiting for Mod Platform's MODS menu before linking Achievements.");
        else log("Native achievement page route unavailable.");
        ULONGLONG journalRetryAfter = 0;
        ULONGLONG readAfter = 0;
        while (!stopping) {
            const auto now = GetTickCount64();
            if (codeReady && !journalLinked && now >= journalRetryAfter) {
                journalRetryAfter = now + 250;
                if (platformJournalRouteReady(game)) {
                    journalLinked = installJournal(api, mod, game);
                    if (journalLinked) {
                        log("Native Progress / Achievements tile linked.");
                        installRuntimePreview();
                    }
                }
            }
            if (now >= readAfter) {
                readAfter = now + 2000;
                try {
                    state.acknowledgeImportNotice();
                    state.refresh(game, overlay, eventsEnabled);
                    state.lastError.clear();
                } catch (const std::exception& error) {
                    if (state.lastError != error.what()) {
                        state.lastError = error.what();
                        log("Will retry: " + state.lastError);
                    }
                }
            }
            overlay.update(popups);
            Sleep(32);
        }
        // Stop accepting events before the final drain so an F9 disable cannot
        // lose live counters collected since the last two-second save scan.
        if (eventsEnabled) stopEventReader(api, mod);
        state.refresh(game, overlay, eventsEnabled);
        state.acknowledgeImportNotice();
    } catch (const std::exception& error) {
        log(std::string("Achievement worker stopped: ") + error.what());
    }
}

bool resumeTracking() {
    if (worker.joinable()) return false;
    HMODULE instance{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&stopping), &instance)) return false;
    stopping = false;
    enableImportNotice(popups);
    worker = std::thread(run, instance);
    return true;
}

void stopTracking() {
    enableImportNotice(false);
    stopping = true;
    if (worker.joinable()) worker.join();
    stopJournal(runtimeControlEnabled());
    stopImportNotice();
    if (api && mod) stopEventReader(api, mod);
    if (api && mod) log("Achievements stopped; worker, tracking hooks and notification window are off.");
}
bool canStopTracking() {
    if (journalCanShutdown()) return true;
    log("Close the Achievements journal before applying its Load setting with F9.");
    return false;
}
bool requestedRuntimeLoad() { return loadRequested(fs::path(api->azimuth_dir)); }
}

FH3MOD_EXPORT int fh3mod_init(const FH3ModApi* platform) {
    if (!platform || platform->api_version < 9 || !platform->azimuth_dir) return 1;
    api = platform;
    FH3ModInfo info{};
    info.size = sizeof info;
    info.name = "FH3 Achievements";
    info.version = Version;
    info.github_repo = "Laur1sXd/FH3Achievements";
    info.check_updates = 1;
    info.platform_version = "1.2.1";
    info.description = "Local Xbox One achievements. Open Pause > Progress > Achievements.";
    info.offline_only = 1;
    mod = api->register_mod(&info);
    if (!mod) return 1;
    try {
        const fs::path install(api->azimuth_dir);
        root = install / L"fh3-mods" / L"FH3Achievements";
        settings = root / L"Settings.ini";
        saves = install / L"data" / L"titles" / L"4CE1DFAB" / L"saves" / L"player-1";
        fs::create_directories(root);
        popups = GetPrivateProfileIntW(Section, L"Popups", 1, settings.c_str()) != 0;
        enableImportNotice(popups);

        // Register the scene before FH3 mounts its loose files. Creating a
        // new scene only after the tile click can leave the native page with
        // no loaded resource tree. The click still refreshes its save data.
        const auto initialPage = journalXaml(initialProgress(), {});
        if (!api->set_file || !api->set_file(mod, JournalFile, initialPage.data(), initialPage.size()))
            log("Could not register the achievement journal scene at startup.");

        FH3Setting setting{};
        setting.size = sizeof setting;
        setting.key = "AchievementPopups";
        setting.label = "Achievement pop-ups";
        setting.description = "Show a pop-up when new achievements are recorded.";
        setting.value = popups;
        setting.on_change = onPopups;
        api->add_setting(mod, &setting);

        const bool wanted = loadRequested(install);
        startRuntimeControl(api, mod, wanted, {requestedRuntimeLoad, resumeTracking, stopTracking, canStopTracking});
        if (!wanted) {
            // A stock hot reload may replace an already-Off DLL. Its resident
            // controls must be available before tracking is started again.
            const auto* game = api->game_base ? static_cast<const uint8_t*>(api->game_base()) : nullptr;
            if (runtimeControlEnabled() && game && installJournalNoticeRefresh(api, mod, game))
                installRuntimePreview();
            log("Load Achievements is OFF; registered the scene and settings without starting tracking or hooks.");
            return 0;
        }

        if (!resumeTracking()) return 1;
        return 0;
    } catch (const std::exception& error) {
        log(error.what());
        return 1;
    }
}

FH3MOD_EXPORT void fh3mod_shutdown() {
    stopRuntimeControl();
    stopTracking();
    stopRuntimePreview();
}

// Lifecycle capabilities for host loaders; stock-platform switching lives in runtime.cpp.
FH3MOD_EXPORT int fh3mod_runtime_toggle() { return 1; }
FH3MOD_EXPORT int fh3mod_can_shutdown() {
    return canStopTracking() ? 1 : 0;
}
FH3MOD_EXPORT int fh3mod_apply_runtime_load() { return applyRuntimeLoad() ? 1 : 0; }
FH3MOD_EXPORT int fh3mod_runtime_load_state() { return runtimeLoadState(); }
