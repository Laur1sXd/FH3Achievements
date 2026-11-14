// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "fh3mod.h"
#include "rules.hpp"

namespace ach {
inline constexpr int JournalSections = 11;
inline constexpr char JournalFile[] = "UI/Scenes/Anthem/A_AchJournal.xaml";

std::string journalXaml(const ProgressList& progress, const Unlocks& unlocks);

bool installJournal(const FH3ModApi* api, FH3Mod* mod, const uint8_t* game);
bool platformJournalRouteReady(const uint8_t* game);
void publishJournal(const ProgressList& progress, const Unlocks& unlocks);
void stopJournal();
}
