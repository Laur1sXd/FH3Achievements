// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "save.hpp"
#include <array>
#include <cstdint>

namespace ach {
enum class Group { base, mountain, wheels, hoonigan, forzathon };
struct Achievement {
    int id;
    Group group;
    int gamerscore;
    const wchar_t* title;
    const wchar_t* condition;
    bool online;
};
inline constexpr Achievement Catalog[] = {
#include "catalog.inc"
};

constexpr bool validCatalog() {
    int totals[5]{};
    int counts[5]{};
    int id = 0;
    for (auto& item : Catalog) {
        if (item.id != ++id) return false;
        totals[static_cast<int>(item.group)] += item.gamerscore;
        ++counts[static_cast<int>(item.group)];
    }
    return id == 135 && counts[0] == 64 && counts[1] == 28 && counts[2] == 29 &&
           counts[3] == 10 && counts[4] == 4 && totals[0] == 1000 && totals[1] == 500 &&
           totals[2] == 500 && totals[3] == 500 && totals[4] == 500;
}
static_assert(validCatalog());

struct Progress {
    int64_t current = 0;
    int64_t target = 0;
    bool complete() const { return target > 0 && current >= target; }
};
using ProgressList = std::array<Progress, std::size(Catalog) + 1>;
using Unlocks = std::array<int64_t, std::size(Catalog) + 1>;
ProgressList evaluate(const Snapshot& snapshot);
ProgressList initialProgress();
}
