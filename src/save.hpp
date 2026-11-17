// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ach {
namespace fs = std::filesystem;
using Bytes = std::span<const uint8_t>;
using Properties = std::map<std::string, std::string, std::less<>>;

struct Snapshot {
    std::map<std::string, int64_t, std::less<>> profile;
    std::map<std::string, Properties, std::less<>> objects;
    std::vector<uint8_t> database;

    int64_t stat(std::string_view key) const;
    std::string_view get(std::string_view object, std::string_view key) const;
    int64_t value(std::string_view object, std::string_view key) const;
};

struct Revision {
    fs::path commit;
    std::wstring profile;
    uint64_t number;
};

std::optional<std::wstring> activeProfile(const uint8_t* game);
std::optional<Revision> latestRevision(const fs::path& saves, std::optional<uint64_t> before = {},
                                       std::wstring_view profile = {});
Snapshot readSave(const uint8_t* game, const Revision& revision);
Snapshot parseSave(Bytes plaintext);
std::vector<uint8_t> readFile(const fs::path& file, size_t maxSize);
std::vector<uint8_t> readGameImage(const fs::path& file);
bool supportedGame(const uint8_t* image);
}
