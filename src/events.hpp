// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "fh3mod.h"
#include "rules.hpp"
#include <set>

namespace ach {
using LiveProgress = std::array<int64_t, std::size(Catalog) + 1>;
using Evidence = std::array<std::set<uint32_t>, std::size(Catalog) + 1>;
struct EventBatch {
    LiveProgress counters{};
    Evidence unique{};
    bool empty() const;
};
bool installEventReader(const FH3ModApi* api, FH3Mod* mod, const uint8_t* base);
void stopEventReader(const FH3ModApi* api, FH3Mod* mod);
LiveProgress takeEvents();
EventBatch takeEventBatch();
bool validEvidence(int id, uint32_t key);
void mergeEventBatch(EventBatch& totals, const EventBatch& changes);
void addEventProgress(ProgressList& progress, const EventBatch& values, bool prSupported = false);
void recordPrResult(int world, std::string_view name, int car, int carClass, float kilograms, float score, float gold);
void addLiveProgress(ProgressList& progress, const LiveProgress& values);
void mergeLiveProgress(LiveProgress& totals, const LiveProgress& changes);
}
