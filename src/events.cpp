// SPDX-License-Identifier: GPL-3.0-only
#include "events.hpp"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <optional>

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
LiveProgress pending{};

struct Rule { int id; int target; bool increment; };
constexpr Rule Rules[] = {
    {7, 1, false}, {9, 1, false}, {19, 4, false}, {26, 10, true},
    {27, 160935, false}, {32, 5, true}, {78, 1, false},
};

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
    pending[id] = increment ? (std::min<int64_t>)(pending[id] + current, 1000)
                            : (std::max)(pending[id], current);
}

uint32_t eventWrite(const void* descriptor, const void* provider, uint64_t handle,
                    uint32_t count, const Descriptor* fields) {
    // Compare addresses of the validated game's exact descriptors. The payload is
    // read locally and only numeric counters are retained. Forward the original call.
    const auto address = reinterpret_cast<uintptr_t>(descriptor);
    if (address >= reinterpret_cast<uintptr_t>(game)) {
        const auto rva = address - reinterpret_cast<uintptr_t>(game);
        switch (rva) {
        case 0x44628d0: // DrivatarHired: IsSwap.
            if (count == 7 && value<uint32_t>(fields, count, 3).value_or(0) == 1) observe(7, 1);
            break;
        case 0x4462358: // CarPurchased (separate from CarAddedToGarage and auction purchases).
            if (count == 13) observe(9, 1);
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
    return reinterpret_cast<EventWrite>(original)(descriptor, provider, handle, count, fields);
}
}

bool installEventReader(const FH3ModApi* api, FH3Mod* mod, const uint8_t* base) {
    game = base;
    if (!api->hook_pointer || !api->enable_hooks) return false;
    auto* slot = const_cast<uint8_t*>(base) + 0x2cf01a0;
    hook = api->hook_pointer(mod, "FH3 achievement events (EtxEventWrite)", reinterpret_cast<void**>(slot),
                             reinterpret_cast<void*>(eventWrite), &original);
    return hook && original && api->enable_hooks(mod, hook);
}
void stopEventReader(const FH3ModApi* api, FH3Mod* mod) {
    if (hook) api->disable_hooks(mod, hook);
}

LiveProgress takeEvents() {
    std::lock_guard lock(mutex);
    const auto result = pending;
    pending.fill(0);
    return result;
}

void addLiveProgress(ProgressList& progress, const LiveProgress& values) {
    for (const auto& rule : Rules)
        progress[rule.id] = {std::clamp<int64_t>(values[rule.id], 0, rule.target), rule.target};
}

void mergeLiveProgress(LiveProgress& totals, const LiveProgress& changes) {
    for (const auto& rule : Rules) {
        auto& total = totals[rule.id];
        total = rule.increment ? (std::min<int64_t>)(rule.target, total + changes[rule.id])
                               : (std::max)(total, changes[rule.id]);
    }
}
}
