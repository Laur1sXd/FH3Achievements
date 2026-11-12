// SPDX-License-Identifier: GPL-3.0-only
#include "save.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ach {
namespace {
constexpr size_t MaxSave = 16 * 1024 * 1024;
constexpr uint32_t GameStamp = 0x5B16ECCA, GameSize = 0x05495000;
#include "crypto_tables.inc"

class Reader {
    Bytes rest;
public:
    explicit Reader(Bytes data) : rest(data) {}
    Bytes take(size_t length) {
        if (length > rest.size()) throw std::runtime_error("truncated save data");
        const auto result = rest.first(length);
        rest = rest.subspan(length);
        return result;
    }
    template<class T> T read() {
        T value;
        std::memcpy(&value, take(sizeof value).data(), sizeof value);
        return value;
    }
    std::string string(size_t length) {
        auto data = take(length);
        return {reinterpret_cast<const char*>(data.data()), data.size()};
    }
    bool empty() const { return rest.empty(); }
};

int64_t number(std::string_view text) {
    if (text.empty()) return 0;
    int64_t result = 0;
    if (text == "true") return 1;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    return error == std::errc{} && end == text.data() + text.size() ? result : 0;
}

void readProperties(Reader& reader, Snapshot& result, const std::string& path,
                    unsigned depth, unsigned& remaining) {
    const auto count = reader.read<uint32_t>();
    if (depth > 64 || count > remaining) throw std::runtime_error("invalid property tree");
    for (uint32_t i = 0; i < count; ++i) {
        if (remaining-- == 0) throw std::runtime_error("property limit exceeded");
        reader.take(5); // HashName case and bit-count fields.
        auto name = path + reader.string(reader.read<uint32_t>());
        const auto type = reader.read<uint32_t>();
        int64_t value;
        switch (type) {
        case 0: case 1: value = reader.read<uint8_t>(); break;
        case 2: value = reader.read<uint16_t>(); break;
        case 3: case 17: value = reader.read<uint32_t>(); break;
        case 4: {
            const auto n = reader.read<uint64_t>();
            if (n > INT64_MAX) continue;
            value = static_cast<int64_t>(n);
            break;
        }
        case 5: value = reader.read<int8_t>(); break;
        case 6: value = reader.read<int16_t>(); break;
        case 7: value = reader.read<int32_t>(); break;
        case 8: value = reader.read<int64_t>(); break;
        case 9: reader.take(4); continue;
        case 10: reader.take(8); continue;
        case 11: reader.take(reader.read<uint32_t>()); continue;
        case 12: reader.take(size_t{2} * reader.read<uint32_t>()); continue;
        case 15: readProperties(reader, result, name + "/", depth + 1, remaining); continue;
        default: throw std::runtime_error("unknown profile property encoding");
        }
        result.profile.emplace(std::move(name), value);
    }
}

class StateReader {
    Reader data;
    std::vector<std::string> strings;
    unsigned remaining = 200000;
    const std::string& index() {
        const auto n = strings.size() > 65535 ? data.read<uint32_t>() :
                       strings.size() > 255 ? data.read<uint16_t>() : data.read<uint8_t>();
        if (n >= strings.size()) throw std::runtime_error("invalid BXML string index");
        return strings[n];
    }
    void node(Snapshot& result, std::string_view owner, unsigned depth) {
        if (depth > 128 || remaining-- == 0) throw std::runtime_error("invalid BXML tree");
        const auto flags = data.read<uint8_t>();
        if (flags & ~7) throw std::runtime_error("unsupported BXML node");
        const auto& tag = index();
        std::string_view id, key, value;
        if (flags & 2) {
            const auto count = data.read<uint8_t>();
            for (unsigned i = 0; i < count; ++i) {
                const auto& name = index();
                const auto& val = index();
                if (name == "Id") id = val;
                if (name == "Name") key = val;
                if (name == "Value") value = val;
            }
        }
        if (tag == "Object" && !id.empty()) owner = id;
        if (tag == "Property" && !owner.empty() && !key.empty())
            result.objects[std::string(owner)].insert_or_assign(std::string(key), std::string(value));
        if (flags & 4) {
            const auto count = data.read<uint16_t>();
            for (unsigned i = 0; i < count; ++i) node(result, owner, depth + 1);
        }
    }
public:
    explicit StateReader(Bytes bytes) : data(bytes) {}
    void parse(Snapshot& result) {
        if (data.read<uint32_t>() != 0x4C4D5842 || data.read<uint8_t>() != 2)
            throw std::runtime_error("unsupported SaveState BXML");
        const auto count = data.read<uint32_t>();
        const auto length = data.read<uint32_t>();
        if (count == 0 || count > 1000000 || count > length / 2)
            throw std::runtime_error("invalid BXML string count");
        Reader table(data.take(length));
        strings.reserve(count);
        for (uint32_t i = 0; i < count; ++i) strings.push_back(table.string(table.read<uint16_t>()));
        if (!table.empty()) throw std::runtime_error("invalid BXML string table");
        data.take(1);
        node(result, {}, 0);
        if (!data.empty()) throw std::runtime_error("unexpected BXML tail");
    }
};

// Save format compatible with the Azimuth FH3 Save Editor. Lookups are read from
// the running game's validated image. The mod ships no game data or key bytes.
using Block = std::array<uint8_t, 16>;
constexpr Block Direct = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
constexpr Block Forward = {0, 5, 10, 15, 3, 4, 9, 14, 2, 7, 8, 13, 1, 6, 11, 12};
constexpr Block Reverse = {0, 7, 10, 13, 1, 4, 11, 14, 2, 5, 8, 15, 3, 6, 9, 12};

template<size_t Rounds>
Block transform(const uint8_t* game, Block state, uint32_t keys,
                const uint32_t (&tables)[Rounds][16], const Block& order) {
    for (size_t round = 0; round < Rounds; ++round) {
        const auto& lanes = round < 2 || round == Rounds - 1 ? Direct : order;
        uint32_t next[4];
        std::memcpy(next, game + keys + round * 16, sizeof next);
        for (size_t lane = 0; lane < 16; ++lane) {
            const auto index = lanes[lane];
            uint32_t entry;
            std::memcpy(&entry, game + tables[round][index] + 4 * state[index], sizeof entry);
            next[lane / 4] ^= entry;
        }
        std::memcpy(state.data(), next, sizeof next);
    }
    return state;
}

Block doubled(const Block& input) {
    Block result;
    unsigned carry = 0;
    for (int i = 15; i >= 0; --i) {
        result[i] = static_cast<uint8_t>((input[i] << 1) | carry);
        carry = input[i] >> 7;
    }
    if (carry) result.back() ^= 0x87;
    return result;
}

void xorWith(Block& a, const Block& b) {
    for (size_t i = 0; i < a.size(); ++i) a[i] ^= b[i];
}

Block authenticate(const uint8_t* game, Bytes message, const Block& fullKey, const Block& shortKey) {
    Block chain{};
    while (message.size() > 16) {
        Block block;
        std::copy_n(message.begin(), 16, block.begin());
        xorWith(block, chain);
        chain = transform(game, block, 0x4733250, AuthRva, Forward);
        message = message.subspan(16);
    }
    Block last{};
    std::copy(message.begin(), message.end(), last.begin());
    if (message.size() < 16) last[message.size()] = 0x80;
    xorWith(last, message.size() == 16 ? fullKey : shortKey);
    xorWith(last, chain);
    return transform(game, last, 0x4733250, AuthRva, Forward);
}

std::vector<uint8_t> decrypt(const uint8_t* game, Bytes bytes) {
    constexpr size_t Header = 36, Record = 528, Data = 512;
    if (bytes.size() <= Header || (bytes.size() - Header) % Record)
        throw std::runtime_error("invalid encrypted ProfileData size");
    const uint32_t total = static_cast<uint32_t>((bytes.size() - Header) / Record * Data);
    uint32_t padding;
    std::memcpy(&padding, bytes.data() + 16, sizeof padding);
    if (padding >= Data) throw std::runtime_error("invalid encrypted ProfileData padding");
    const auto fullKey = doubled(transform(game, Block{}, 0x4733250, AuthRva, Forward));
    const auto shortKey = doubled(fullKey);
    std::array<uint8_t, 24> header;
    std::memcpy(header.data(), &total, sizeof total);
    std::copy_n(bytes.begin(), 20, header.begin() + 4);
    const auto headerTag = authenticate(game, header, fullKey, shortKey);
    if (!std::equal(headerTag.begin(), headerTag.end(), bytes.begin() + 20))
        throw std::runtime_error("ProfileData header authentication failed");
    std::vector<uint8_t> plain;
    plain.reserve(total);
    Block previous;
    std::copy_n(bytes.begin(), 16, previous.begin());
    for (size_t offset = Header; offset < bytes.size(); offset += Record) {
        std::array<uint8_t, Record> record;
        for (size_t i = 0; i < Record; i += 16) {
            Block cipher;
            std::copy_n(bytes.begin() + offset + i, 16, cipher.begin());
            auto block = transform(game, cipher, 0x4737F70, DecodeRva, Reverse);
            xorWith(block, previous);
            std::copy(block.begin(), block.end(), record.begin() + i);
            previous = cipher;
        }
        const auto tag = authenticate(game, Bytes(record).first(Data), fullKey, shortKey);
        if (!std::equal(tag.begin(), tag.end(), record.begin() + Data))
            throw std::runtime_error("ProfileData record authentication failed");
        plain.insert(plain.end(), record.begin(), record.begin() + Data);
    }
    plain.resize(total - padding);
    return plain;
}

std::string sha256(std::vector<uint8_t>& contents) {
    struct Provider {
        BCRYPT_ALG_HANDLE value = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        ~Provider() {
            if (hash) BCryptDestroyHash(hash);
            if (value) BCryptCloseAlgorithmProvider(value, 0);
        }
    } provider;
    std::array<uint8_t, 32> digest;
    if (BCryptOpenAlgorithmProvider(&provider.value, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptCreateHash(provider.value, &provider.hash, nullptr, 0, nullptr, 0, 0) < 0 ||
        BCryptHashData(provider.hash, contents.data(), static_cast<ULONG>(contents.size()), 0) < 0 ||
        BCryptFinishHash(provider.hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
        throw std::runtime_error("cannot verify ProfileData SHA-256");
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (uint8_t byte : digest) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 15]);
    }
    return result;
}
}

int64_t Snapshot::stat(std::string_view key) const {
    const auto it = profile.find(key);
    return it == profile.end() ? 0 : it->second;
}

std::string_view Snapshot::get(std::string_view object, std::string_view key) const {
    const auto it = objects.find(object);
    if (it == objects.end()) return {};
    const auto property = it->second.find(key);
    return property == it->second.end() ? std::string_view{} : std::string_view(property->second);
}

int64_t Snapshot::value(std::string_view object, std::string_view key) const {
    return number(get(object, key));
}

std::vector<uint8_t> readFile(const fs::path& file, size_t maxSize) {
    std::ifstream input(file, std::ios::binary | std::ios::ate);
    const auto length = input.tellg();
    if (length < 0 || static_cast<uintmax_t>(length) > maxSize)
        throw std::runtime_error("file missing, unreadable or too large");
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(bytes.data()), length))
        throw std::runtime_error("file changed while reading");
    return bytes;
}

std::optional<Revision> latestRevision(const fs::path& saves, std::optional<uint64_t> before) {
    // Azimuth's single-player slot. Match profile containers only, never thumbnails.
    std::optional<Revision> result;
    if (!fs::is_directory(saves)) return result;
    for (const auto& container : fs::directory_iterator(saves)) {
        const auto name = container.path().filename().wstring();
        if (!name.starts_with(L"User_") || name.size() <= 5 || name.size() > 37 ||
            name.find_first_not_of(L"0123456789abcdefABCDEF", 5) != std::wstring::npos) continue;
        const auto revisions = container.path() / L"revisions";
        if (!container.is_directory() || !fs::is_directory(revisions)) continue;
        // Refuse ambiguous roots rather than mixing unlocks between two profiles.
        std::optional<Revision> newest;
        for (const auto& item : fs::directory_iterator(revisions)) {
            const auto id = item.path().filename().string();
            uint64_t n;
            const auto [end, error] = std::from_chars(id.data(), id.data() + id.size(), n, 16);
            if (id.size() != 16 || error != std::errc{} || end != id.data() + id.size() ||
                (before && n >= *before) || (newest && n <= newest->number) || !item.is_directory()) continue;
            const auto commit = item.path() / L"commit";
            if (fs::is_regular_file(commit) && fs::is_regular_file(item.path() / L"data" / L"ProfileData"))
                newest = Revision{commit, name, n};
        }
        if (!newest) continue;
        if (result) throw std::runtime_error("more than one FH3 profile in the player-1 slot");
        result = std::move(newest);
    }
    return result;
}

Snapshot parseSave(Bytes plaintext) {
    Snapshot result;
    Reader stream(plaintext);
    bool properties = false, state = false;
    while (!stream.empty()) {
        const auto key = stream.read<uint32_t>();
        auto bytes = stream.take(stream.read<uint32_t>());
        if (key == 0x4a8bf2b6) {
            if (properties) throw std::runtime_error("duplicate profile properties");
            Reader reader(bytes);
            unsigned remaining = 100000;
            readProperties(reader, result, "", 0, remaining);
            // The game may append auxiliary fields after the counted property tree.
            properties = true;
        } else if (key == 0xb60261fc) {
            if (state) throw std::runtime_error("duplicate SaveState");
            StateReader(bytes).parse(result);
            state = true;
        } else if (key == 0xa9f1f729) {
            constexpr char magic[] = "SQLite format 3";
            if (!result.database.empty() || bytes.size() < 16 ||
                std::memcmp(bytes.data(), magic, sizeof magic) != 0)
                throw std::runtime_error("invalid career database");
            result.database.assign(bytes.begin(), bytes.end());
        }
    }
    if (!properties || !state) throw std::runtime_error("missing FH3 save sections");
    return result;
}

Snapshot readSave(const uint8_t* game, const Revision& revision) {
    auto manifest = readFile(revision.commit, 64 * 1024);
    std::istringstream lines(std::string(manifest.begin(), manifest.end()));
    std::string version, count;
    std::getline(lines, version);
    std::getline(lines, count);
    if (version.ends_with('\r')) version.pop_back();
    if (version != "Azimuth complete save revision v2" || !count.starts_with("blobs "))
        throw std::runtime_error("unknown Azimuth save commit format");
    std::string hash, name, expectedHash;
    size_t length, expectedSize = 0;
    while (lines >> hash >> length >> name) {
        if (name != "ProfileData") continue;
        if (!expectedHash.empty()) throw std::runtime_error("duplicate ProfileData in commit");
        expectedHash = hash;
        expectedSize = length;
    }
    auto bytes = readFile(revision.commit.parent_path() / L"data" / L"ProfileData", MaxSave);
    if (expectedSize != bytes.size() || expectedHash != sha256(bytes))
        throw std::runtime_error("ProfileData does not match its committed hash");
    return parseSave(decrypt(game, bytes));
}

bool supportedGame(const uint8_t* image) {
    if (!image) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 64 || dos->e_lfanew > 4096)
        return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE &&
           nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
           nt->FileHeader.TimeDateStamp == GameStamp && nt->OptionalHeader.SizeOfImage == GameSize;
}

std::vector<uint8_t> readGameImage(const fs::path& file) {
    const auto disk = readFile(file, 128 * 1024 * 1024);
    if (disk.size() < 8192 || !supportedGame(disk.data())) throw std::runtime_error("wrong FH3 executable");
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(disk.data());
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(disk.data() + dos->e_lfanew);
    const auto headerSize = nt->OptionalHeader.SizeOfHeaders;
    const auto* section = IMAGE_FIRST_SECTION(nt);
    std::vector<uint8_t> image(GameSize);
    if (headerSize > disk.size() || headerSize > image.size() ||
        reinterpret_cast<const uint8_t*>(section + nt->FileHeader.NumberOfSections) > disk.data() + headerSize)
        throw std::runtime_error("invalid FH3 PE headers");
    std::copy_n(disk.begin(), headerSize, image.begin());
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        const auto size = section->SizeOfRawData;
        const auto offset = section->PointerToRawData;
        const auto address = section->VirtualAddress;
        if (uint64_t(offset) + size > disk.size() || uint64_t(address) + size > image.size())
            throw std::runtime_error("invalid FH3 PE section");
        if (size) std::copy_n(disk.begin() + offset, size, image.begin() + address);
    }
    return image;
}
}
