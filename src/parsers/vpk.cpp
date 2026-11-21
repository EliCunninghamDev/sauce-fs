// Ported from seeass-index's seeass_index.h.
// SPDX-License-Identifier: MIT

#include "vpk.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "bytes.hpp"

namespace sauce::fs::parsers
{

bool is_vpk_chunk_name(std::string_view name) noexcept
{
    if (!ends_with_ci(name, ".vpk") || name.size() < 8)
    {
        return false;
    }
    const std::string_view stem = name.substr(0, name.size() - 4);
    const std::size_t underscore = stem.find_last_of('_');
    if (underscore == std::string_view::npos)
    {
        return false;
    }
    const std::string_view suffix = stem.substr(underscore + 1);
    return suffix.size() == 3 && suffix[0] >= '0' && suffix[0] <= '9' && suffix[1] >= '0' &&
           suffix[1] <= '9' && suffix[2] >= '0' && suffix[2] <= '9';
}

/// Given a gameinfo/CLI path, find the actual VPK directory file.  Source's
/// gameinfo commonly names foo.vpk while the file is foo_dir.vpk.
std::filesystem::path resolve_vpk_path(const std::filesystem::path& input)
{
    std::error_code ec;
    const std::string name = input.filename().string();
    if (is_vpk_chunk_name(name))
    {
        const std::string stem = name.substr(0, name.size() - 4);
        const std::size_t underscore = stem.find_last_of('_');
        if (underscore != std::string::npos)
        {
            std::filesystem::path candidate = input;
            candidate.replace_filename(stem.substr(0, underscore) + "_dir.vpk");
            if (std::filesystem::is_regular_file(candidate, ec))
            {
                return candidate;
            }
        }
    }
    if (std::filesystem::is_regular_file(input, ec))
    {
        if (ends_with_ci(name, ".vpk") && !ends_with_ci(name.substr(0, name.size() - 4), "_dir"))
        {
            const std::string stem = name.substr(0, name.size() - 4);
            std::filesystem::path candidate = input;
            candidate.replace_filename(stem + "_dir.vpk");
            if (std::filesystem::is_regular_file(candidate, ec))
            {
                return candidate;
            }
        }
        return input;
    }
    if (ends_with_ci(name, ".vpk"))
    {
        const std::string stem = name.substr(0, name.size() - 4);
        if (!ends_with_ci(stem, "_dir"))
        {
            std::filesystem::path candidate = input;
            candidate.replace_filename(stem + "_dir.vpk");
            if (std::filesystem::is_regular_file(candidate, ec))
            {
                return candidate;
            }
        }
    }
    else
    {
        std::filesystem::path candidate = input;
        candidate += "_dir.vpk";
        if (std::filesystem::is_regular_file(candidate, ec))
        {
            return candidate;
        }
        candidate = input;
        candidate += ".vpk";
        if (std::filesystem::is_regular_file(candidate, ec))
        {
            return candidate;
        }
    }
    return input;
}

std::filesystem::path vpk_side_archive(const std::filesystem::path& directory_file, unsigned index)
{
    std::string name = directory_file.filename().string();
    std::string stem;
    if (ends_with_ci(name, ".vpk"))
    {
        stem = name.substr(0, name.size() - 4);
    }
    else
    {
        stem = name;
    }
    if (ends_with_ci(stem, "_dir"))
    {
        stem.resize(stem.size() - 4);
    }
    return directory_file.parent_path() / (stem + fmt::format("_{:03}.vpk", index));
}

bool parse_vpk(const std::filesystem::path& path, std::vector<ScannedEntry>& entries,
               std::string* error, std::vector<std::string>& warnings)
{
    FileHandle file;
    if (!file.open(path, error))
    {
        return false;
    }
    std::uint8_t header[28] = {};
    if (file.size() < 12 || !file.read_at(0, header, 12))
    {
        if (error)
        {
            *error = "VPK is too small: " + path.string();
        }
        return false;
    }
    auto le32 = [](const std::uint8_t* p)
    {
        return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
               (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
    };
    if (le32(header) != 0x55AA1234u)
    {
        if (error)
        {
            *error = "not a VPK (bad signature): " + path.string();
        }
        return false;
    }
    const std::uint32_t version = le32(header + 4);
    if (version != 1 && version != 2)
    {
        if (error)
        {
            *error = "unsupported VPK version " + std::to_string(version) + ": " + path.string();
        }
        return false;
    }
    const std::size_t header_size = version == 1 ? 12u : 28u;
    if (header_size == 28 && (!file.read_at(0, header, 28)))
    {
        if (error)
        {
            *error = "truncated VPK header: " + path.string();
        }
        return false;
    }
    const std::uint64_t tree_size = le32(header + 8);
    std::uint64_t tree_offset = header_size;
    std::uint64_t data_offset = 0;
    if (!safe_add(tree_offset, tree_size, data_offset) || data_offset > file.size())
    {
        if (error)
        {
            *error = "VPK directory tree is larger than the file: " + path.string();
        }
        return false;
    }
    if (tree_size > static_cast<std::uint64_t>(SIZE_MAX))
    {
        if (error)
        {
            *error = "VPK directory tree is too large: " + path.string();
        }
        return false;
    }
    std::vector<std::uint8_t> tree;
    if (!read_file_range(path, tree_offset, tree_size, tree, error))
    {
        return false;
    }

    Cursor cursor(tree.data(), tree.size());
    std::string extension;
    std::string directory;
    std::string name;
    std::vector<ScannedEntry> parsed;

    for (;;)
    {
        if (!cursor.string(extension))
        {
            if (error)
            {
                *error = "truncated VPK extension: " + path.string();
            }
            return false;
        }
        if (extension.empty())
        {
            break;
        }
        for (;;)
        {
            if (!cursor.string(directory))
            {
                if (error)
                {
                    *error = "truncated VPK directory: " + path.string();
                }
                return false;
            }
            if (directory.empty())
            {
                break;
            }
            for (;;)
            {
                if (!cursor.string(name))
                {
                    if (error)
                    {
                        *error = "truncated VPK filename: " + path.string();
                    }
                    return false;
                }
                if (name.empty())
                {
                    break;
                }
                std::uint32_t crc = 0;
                std::uint16_t preload = 0;
                std::uint16_t archive = 0;
                std::uint32_t entry_offset = 0;
                std::uint32_t entry_length = 0;
                std::uint16_t terminator = 0;
                if (!cursor.u32(crc) || !cursor.u16(preload) || !cursor.u16(archive) ||
                    !cursor.u32(entry_offset) || !cursor.u32(entry_length) ||
                    !cursor.u16(terminator))
                {
                    if (error)
                    {
                        *error = "truncated VPK entry: " + path.string();
                    }
                    return false;
                }
                if (terminator != 0xFFFFu)
                {
                    if (error)
                    {
                        *error = "invalid VPK entry terminator: " + path.string();
                    }
                    return false;
                }
                // Preload bytes are stored inline in the tree immediately
                // after this entry's record, not in a block at the end.
                const std::size_t preload_position = cursor.position();
                if (preload != 0)
                {
                    const std::uint8_t* preload_data = nullptr;
                    if (!cursor.bytes(preload, preload_data))
                    {
                        if (error)
                        {
                            *error = "truncated VPK preload data: " + path.string();
                        }
                        return false;
                    }
                }
                // A few shipped VPKs use a single-space directory string for
                // the archive root.  Treat a whitespace-only component as
                // empty, but preserve real spaces inside names.
                auto blank = [](const std::string& text)
                { return text.find_first_not_of(" \t") == std::string::npos; };
                const std::string clean_directory = blank(directory) ? std::string() : directory;
                const std::string clean_name = blank(name) ? std::string() : name;
                const std::string clean_extension = blank(extension) ? std::string() : extension;
                if (clean_name.empty())
                {
                    continue;
                }
                std::string virtual_path;
                if (!clean_directory.empty())
                {
                    virtual_path = clean_directory;
                    virtual_path.push_back('/');
                }
                virtual_path += clean_name;
                if (!clean_extension.empty())
                {
                    virtual_path.push_back('.');
                    virtual_path += clean_extension;
                }
                ScannedEntry entry;
                entry.path = normalize_path(virtual_path);
                if (entry.path.empty() || entry.path.find("..") != std::string::npos)
                {
                    continue;
                }
                entry.preload_length = preload;
                if (preload != 0)
                {
                    entry.preload_offset = tree_offset + preload_position;
                }
                entry.archive = archive;
                entry.offset = entry_offset;
                entry.length = entry_length;
                entry.size = static_cast<std::uint64_t>(preload) + entry_length;
                entry.crc = crc;
                entry.crc_known = crc != 0;
                parsed.push_back(std::move(entry));
            }
        }
    }
    for (ScannedEntry& entry : parsed)
    {
        if (entry.archive == 0x7FFFu)
        {
            std::uint64_t absolute = 0;
            std::uint64_t end = 0;
            if (!safe_add(data_offset, entry.offset, absolute) ||
                !safe_add(absolute, entry.length, end) || end > file.size())
            {
                if (error)
                {
                    *error = "VPK entry is outside the directory file: " + path.string();
                }
                return false;
            }
            entry.offset = absolute;
        }
        else
        {
            const std::filesystem::path side = vpk_side_archive(path, entry.archive);
            std::error_code ec;
            const std::uint64_t side_size = std::filesystem::file_size(side, ec);
            std::uint64_t end = 0;
            if (ec || !safe_add(entry.offset, entry.length, end) || end > side_size)
            {
                if (warnings.size() < 1000)
                {
                    warnings.push_back("VPK entry outside side archive " + side.string() + ": " +
                                       entry.path);
                }
                continue;
            }
        }
        entries.push_back(std::move(entry));
    }
    return true;
}

} // namespace sauce::fs::parsers
