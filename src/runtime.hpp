// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "fh3mod.h"

namespace ach {
struct RuntimeActions {
    bool (*requested)();
    bool (*start)();
    void (*stop)();
    bool (*canStop)();
};
void startRuntimeControl(const FH3ModApi* api, FH3Mod* mod, bool active, RuntimeActions actions);
void stopRuntimeControl();
bool runtimeControlEnabled();
bool applyRuntimeLoad();
int runtimeLoadState();
void refreshRuntimeNotice(void* page);
bool installRuntimePreview();
void stopRuntimePreview();
}
