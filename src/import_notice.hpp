// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "fh3mod.h"
#include "rules.hpp"
#include <span>

namespace ach {
struct ImportSummary {
    int count = 0;
    int score = 0;
};

bool installImportNotice(const FH3ModApi* api, FH3Mod* mod, const uint8_t* game);
void stopImportNotice(bool keepLifetimeWatch = false);
void queueImportNotice(ImportSummary summary);
void queueAchievementNotices(std::span<const Achievement* const> achievements);
void enableImportNotice(bool enabled);
bool importNoticeShown();
}
