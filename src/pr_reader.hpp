// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "fh3mod.h"
#include <cstdint>
namespace ach {
bool installPrReader(const FH3ModApi* api, FH3Mod* mod, const uint8_t* game);
void stopPrReader(const FH3ModApi* api, FH3Mod* mod);
}
