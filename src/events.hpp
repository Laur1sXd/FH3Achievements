// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "fh3mod.h"
#include "rules.hpp"

namespace ach {
using LiveProgress = std::array<int64_t, std::size(Catalog) + 1>;
bool installEventReader(const FH3ModApi* api, FH3Mod* mod, const uint8_t* base);
void stopEventReader(const FH3ModApi* api, FH3Mod* mod);
LiveProgress takeEvents();
void addLiveProgress(ProgressList& progress, const LiveProgress& values);
void mergeLiveProgress(LiveProgress& totals, const LiveProgress& changes);
}
