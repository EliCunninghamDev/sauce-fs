// SPDX-License-Identifier: MIT

#include "core/detail/archive_scan.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <xxhash.h>

#include "parsers/bsp.hpp"
#include "parsers/bytes.hpp"
#include "parsers/gma.hpp"
#include "parsers/vpk.hpp"

namespace sauce::fs::core
{

namespace
{

namespace fs = std::filesystem;

constexpr std::size_t kReadChunk = 1u << 20;

bool has_extension(std::string_view path, std::initializer_list<std::string_view> extensions)
{
    for (const std::string_view extension : extensions)
    {
        if (parsers::ends_with_ci(path, extension))
        {
            return true;
        }
    }
    return false;
}

/// Asset type is a coarse classification of the path's extension.
AssetType classify_asset_type(std::string_view path)
{
    if (has_extension(path, {".mdl", ".vvd", ".vtx", ".phy", ".ani", ".vta"}))
    {
        return AssetType::Model;
    }
    if (has_extension(path, {".vmt"}))
    {
        return AssetType::Material;
    }
    if (has_extension(path, {".vtf", ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".dds"}))
    {
        return AssetType::Texture;
    }
    if (has_extension(path, {".wav", ".mp3", ".ogg", ".flac"}))
    {
        return AssetType::Sound;
    }
    if (has_extension(path, {".bsp", ".vmf"}))
    {
        return AssetType::Map;
    }
    if (has_extension(path, {".lua", ".cfg", ".txt", ".vbs", ".js", ".nut", ".sq"}))
    {
        return AssetType::Script;
    }
    return AssetType::Unknown;
}

void append_entry_paths(std::string_view archive_path, ArchiveVariant variant,
                        std::vector<parsers::ScannedEntry> entries,
                        std::vector<ArchiveContent>& contents)
{
    // Case-normalized paths can collide inside one archive (VPK trees are not
    // guaranteed unique). The table keys on (archive, path), so collapse them
    // before deriving directories.
    std::sort(entries.begin(), entries.end(),
              [](const parsers::ScannedEntry& left, const parsers::ScannedEntry& right)
              { return left.path < right.path; });
    entries.erase(
        std::unique(entries.begin(), entries.end(),
                    [](const parsers::ScannedEntry& left, const parsers::ScannedEntry& right)
                    { return left.path == right.path; }),
        entries.end());

    std::set<std::string> directories;
    contents.reserve(contents.size() + entries.size() + 64);
    for (parsers::ScannedEntry& entry : entries)
    {
        if (entry.path.empty())
        {
            continue;
        }

        // Archive listings contain files only. Materialize every ancestor so
        // readdir() can use an equality lookup even for directories that hold
        // only subdirectories.
        for (std::size_t slash = entry.path.find('/'); slash != std::string::npos;
             slash = entry.path.find('/', slash + 1))
        {
            directories.insert(entry.path.substr(0, slash));
        }

        ArchiveContent content;
        content.archive_path = archive_path;
        content.path = std::move(entry.path);
        content.offset = entry.offset;
        content.chunk =
            variant == ArchiveVariant::Vpk ? static_cast<std::int32_t>(entry.archive) : -1;
        content.length = entry.length;
        content.size = entry.size;
        content.method = entry.method;
        content.preload_offset = entry.preload_offset;
        content.preload_length = entry.preload_length;
        content.asset_type = classify_asset_type(content.path);
        contents.push_back(std::move(content));
    }

    for (const std::string& directory : directories)
    {
        ArchiveContent content;
        content.archive_path = archive_path;
        content.path = directory;
        content.directory = true;
        contents.push_back(std::move(content));
    }
}

/// Parse only the container's directory/index; entry payloads are never read.
} // namespace

bool compute_hash(const fs::path& path, std::uint64_t& hash, std::string& error)
{
    parsers::FileHandle file;
    if (!file.open(path, &error))
    {
        return false;
    }

    XXH3_state_t* state = XXH3_createState();
    if (state == nullptr)
    {
        error = "cannot allocate hash state for " + path.string();
        return false;
    }
    struct StateGuard
    {
        XXH3_state_t* state;

        ~StateGuard()
        {
            XXH3_freeState(state);
        }
    } state_guard{state};

    XXH3_64bits_reset(state);

    // One reusable buffer per worker thread: hashing is dispatched as many
    // small jobs, so per-file allocation would otherwise dominate.
    thread_local std::vector<std::uint8_t> buffer(kReadChunk);
    std::uint64_t remaining = file.size();
    while (remaining > 0)
    {
        const std::size_t chunk =
            static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        if (!file.read(buffer.data(), chunk))
        {
            error = "cannot read " + path.string();
            return false;
        }
        XXH3_64bits_update(state, buffer.data(), chunk);
        remaining -= chunk;
    }
    hash = XXH3_64bits_digest(state);
    return true;
}

std::string hash_hex(std::uint64_t hash)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string text(16, '0');
    for (std::size_t index = 0; index < text.size(); ++index)
    {
        text[text.size() - 1 - index] = digits[hash & 0xFu];
        hash >>= 4;
    }
    return text;
}

std::uint64_t parse_hash_hex(std::string_view text)
{
    std::uint64_t hash = 0;
    for (const char character : text)
    {
        std::uint8_t digit = 0;
        if (character >= '0' && character <= '9')
        {
            digit = static_cast<std::uint8_t>(character - '0');
        }
        else if (character >= 'a' && character <= 'f')
        {
            digit = static_cast<std::uint8_t>(character - 'a' + 10);
        }
        else if (character >= 'A' && character <= 'F')
        {
            digit = static_cast<std::uint8_t>(character - 'A' + 10);
        }
        else
        {
            continue;
        }
        hash = (hash << 4) | digit;
    }
    return hash;
}
void scan_contents(const fs::path& path, ArchiveRecord& record,
                   std::vector<ArchiveContent>& contents, std::vector<std::string>& warnings)
{
    std::vector<parsers::ScannedEntry> entries;
    parsers::GmaMetadata metadata;
    std::string error;
    std::vector<std::string> parse_warnings;
    bool ok = false;
    switch (record.variant)
    {
    case ArchiveVariant::Vpk:
        ok = parsers::parse_vpk(path, entries, &error, parse_warnings);
        break;
    case ArchiveVariant::Bsp:
    {
        std::uint64_t pak_offset = 0;
        std::uint64_t pak_length = 0;
        ok = parsers::parse_bsp_pakfile(path, pak_offset, pak_length, entries, &error,
                                        parse_warnings);
        if (ok)
        {
            // Parser offsets are pakfile-relative; store them relative to the
            // BSP itself so offset..offset+length is a real file range.
            for (parsers::ScannedEntry& entry : entries)
            {
                entry.offset += pak_offset;
            }
        }
        break;
    }
    case ArchiveVariant::Gma:
        ok = parsers::parse_gma(path, entries, &metadata, &error);
        break;
    case ArchiveVariant::GmaBin:
    {
        std::vector<std::uint8_t> decoded;
        ok = parsers::decode_legacy_bin(path, decoded, &error) &&
             parsers::parse_gma_buffer(decoded.data(), decoded.size(), entries, &metadata, &error);
        break;
    }
    }

    warnings.insert(warnings.end(), std::make_move_iterator(parse_warnings.begin()),
                    std::make_move_iterator(parse_warnings.end()));
    if (!ok)
    {
        warnings.push_back(error.empty() ? "cannot parse " + path.string() : error);
        return;
    }
    if (record.variant == ArchiveVariant::Gma || record.variant == ArchiveVariant::GmaBin)
    {
        record.addon_name = std::move(metadata.name);
        record.addon_description = std::move(metadata.description);
        record.addon_author = std::move(metadata.author);
        record.addon_version = metadata.version;
        record.steamid64 = metadata.steamid64;
        record.timestamp = metadata.timestamp;
    }
    append_entry_paths(record.path, record.variant, std::move(entries), contents);
}

/// Game-relative mount directory for one archive: the on-disk path with the
/// archive extension replaced by a sibling directory, e.g. `maps/foo.bsp` ->
/// `maps/foo`, `hl2_misc_dir.vpk` -> `hl2_misc`, `addons/foo.gma` ->
/// `addons/foo`. Keeping the parent path avoids collisions between archives
} // namespace sauce::fs::core
