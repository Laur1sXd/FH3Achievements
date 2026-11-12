// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "fh3mod.h"

namespace ach {
struct ImportSummary {
    int count = 0;
    int score = 0;
};

bool installImportNotice(const FH3ModApi* api, FH3Mod* mod, const uint8_t* game);
void stopImportNotice();
void queueImportNotice(ImportSummary summary);
void enableImportNotice(bool enabled);
bool importNoticeShown();
}
