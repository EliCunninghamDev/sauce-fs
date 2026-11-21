// Ported from seeass-index's seeass_index.h.
// SPDX-License-Identifier: MIT

#include "bsp.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "bytes.hpp"

namespace sauce::fs::parsers
{

bool parse_bsp_pakfile(const std::filesystem::path& path, std::uint64_t& pak_offset,
                       std::uint64_t& pak_length, std::vector<ScannedEntry>& entries,
                       std::string* error, std::vector<std::string>& warnings)
{
    FileHandle file;
    if (!file.open(path, error))
    {
        return false;
    }
    std::uint8_t header[8 + 64 * 16] = {};
    if (file.size() < sizeof(header) || !file.read_at(0, header, sizeof(header)))
    {
        if (error)
        {
            *error = "BSP is too small: " + path.string();
        }
        return false;
    }
    if (std::memcmp(header, "VBSP", 4) != 0)
    {
        if (error)
        {
            *error = "not a BSP (bad signature): " + path.string();
        }
        return false;
    }
    pak_offset = load_le32(header + 8 + 40 * 16);
    pak_length = load_le32(header + 8 + 40 * 16 + 4);
    if (pak_length == 0)
    {
        return true;
    }
    std::uint64_t pak_end = 0;
    if (!safe_add(pak_offset, pak_length, pak_end) || pak_end > file.size())
    {
        if (error)
        {
            *error = "BSP pakfile lump is outside the file: " + path.string();
        }
        return false;
    }
    if (pak_length < 22)
    {
        return true;
    }
    const std::uint64_t tail_length = std::min<std::uint64_t>(pak_length, 65557);
    std::vector<std::uint8_t> tail;
    if (!read_file_range(path, pak_offset + pak_length - tail_length, tail_length, tail, error))
    {
        return false;
    }
    std::int64_t eocd = -1;
    for (std::int64_t i = static_cast<std::int64_t>(tail.size()) - 22; i >= 0; --i)
    {
        if (tail[static_cast<std::size_t>(i)] == 'P' &&
            tail[static_cast<std::size_t>(i) + 1] == 'K' &&
            tail[static_cast<std::size_t>(i) + 2] == 0x05 &&
            tail[static_cast<std::size_t>(i) + 3] == 0x06)
        {
            eocd = i;
            break;
        }
    }
    if (eocd < 0)
    {
        warnings.push_back("BSP pakfile has no ZIP end record: " + path.string());
        return true;
    }
    Cursor eocd_cursor(tail.data() + eocd, tail.size() - static_cast<std::size_t>(eocd));
    std::uint32_t signature = 0;
    std::uint16_t disk = 0;
    std::uint16_t cd_disk = 0;
    std::uint16_t disk_entries = 0;
    std::uint16_t total_entries = 0;
    std::uint32_t cd_size = 0;
    std::uint32_t cd_offset = 0;
    std::uint16_t comment = 0;
    if (!eocd_cursor.u32(signature) || !eocd_cursor.u16(disk) || !eocd_cursor.u16(cd_disk) ||
        !eocd_cursor.u16(disk_entries) || !eocd_cursor.u16(total_entries) ||
        !eocd_cursor.u32(cd_size) || !eocd_cursor.u32(cd_offset) || !eocd_cursor.u16(comment))
    {
        warnings.push_back("malformed BSP pakfile end record: " + path.string());
        return true;
    }
    (void)disk;
    (void)cd_disk;
    (void)disk_entries;
    (void)comment;
    std::uint64_t cd_end = 0;
    if (!safe_add(cd_offset, cd_size, cd_end) || cd_end > pak_length)
    {
        warnings.push_back("BSP pakfile central directory is out of range: " + path.string());
        return true;
    }
    std::vector<std::uint8_t> cd;
    if (!read_file_range(path, pak_offset + cd_offset, cd_size, cd, error))
    {
        return true;
    }
    Cursor cursor(cd.data(), cd.size());
    std::uint32_t parsed = 0;
    while (parsed < total_entries && cursor.remaining() >= 46)
    {
        std::uint32_t record_sig = 0;
        std::uint16_t version_made = 0;
        std::uint16_t version_needed = 0;
        std::uint16_t flags = 0;
        std::uint16_t method = 0;
        std::uint16_t mod_time = 0;
        std::uint16_t mod_date = 0;
        std::uint32_t crc = 0;
        std::uint32_t compressed = 0;
        std::uint32_t uncompressed = 0;
        std::uint16_t name_length = 0;
        std::uint16_t extra_length = 0;
        std::uint16_t comment_length = 0;
        std::uint16_t disk_start = 0;
        std::uint16_t internal_attrs = 0;
        std::uint32_t external_attrs = 0;
        std::uint32_t local_offset = 0;
        if (!cursor.u32(record_sig) || !cursor.u16(version_made) || !cursor.u16(version_needed) ||
            !cursor.u16(flags) || !cursor.u16(method) || !cursor.u16(mod_time) ||
            !cursor.u16(mod_date) || !cursor.u32(crc) || !cursor.u32(compressed) ||
            !cursor.u32(uncompressed) || !cursor.u16(name_length) || !cursor.u16(extra_length) ||
            !cursor.u16(comment_length) || !cursor.u16(disk_start) || !cursor.u16(internal_attrs) ||
            !cursor.u32(external_attrs) || !cursor.u32(local_offset))
        {
            break;
        }
        (void)version_made;
        (void)version_needed;
        (void)mod_time;
        (void)mod_date;
        (void)disk_start;
        (void)internal_attrs;
        (void)external_attrs;
        const std::uint8_t* name_data = nullptr;
        if (!cursor.bytes(name_length, name_data) || !cursor.skip(extra_length) ||
            !cursor.skip(comment_length))
        {
            break;
        }
        ++parsed;
        if (record_sig != 0x02014B50u)
        {
            break;
        }
        if ((flags & 0x0001u) != 0 || compressed == 0xFFFFFFFFu || uncompressed == 0xFFFFFFFFu ||
            local_offset == 0xFFFFFFFFu)
        {
            if (warnings.size() < 1000)
            {
                warnings.push_back("skipping encrypted or ZIP64 BSP pakfile entry: " +
                                   path.string());
            }
            continue;
        }
        // Method 0 (store), 8 (deflate), and 14 (LZMA) are readable.  Anything
        // else stays indexed with its method so read() can report it.
        if (method == 0 && compressed != uncompressed)
        {
            // Stored entries should agree; treat a mismatch as corrupt.
            warnings.push_back("stored ZIP entry has mismatched sizes in " + path.string());
        }
        std::string entry_path(reinterpret_cast<const char*>(name_data), name_length);
        if (!entry_path.empty() && entry_path.back() == '/')
        {
            continue;
        }
        entry_path = normalize_path(trim(entry_path));
        if (entry_path.empty() || entry_path.find("..") != std::string::npos)
        {
            continue;
        }
        std::uint8_t local[30] = {};
        if (!file.read_at(pak_offset + local_offset, local, sizeof(local)))
        {
            warnings.push_back("truncated local ZIP header in " + path.string());
            continue;
        }
        if (load_le32(local) != 0x04034B50u)
        {
            warnings.push_back("invalid local ZIP header in " + path.string());
            continue;
        }
        const std::uint16_t local_name = load_le16(local + 26);
        const std::uint16_t local_extra = load_le16(local + 28);
        std::uint64_t data_offset =
            static_cast<std::uint64_t>(local_offset) + 30 + local_name + local_extra;
        std::uint64_t data_end = 0;
        if (!safe_add(data_offset, compressed, data_end) || data_end > pak_length)
        {
            warnings.push_back("ZIP entry data is outside the BSP pakfile: " + path.string());
            continue;
        }
        ScannedEntry entry;
        entry.path = std::move(entry_path);
        entry.offset = data_offset;
        entry.length = compressed;
        entry.size = uncompressed;
        entry.crc = crc;
        entry.crc_known = true;
        entry.archive = 0x7FFFu;
        entry.method = static_cast<std::uint8_t>(method);
        entries.push_back(std::move(entry));
    }
    return true;
}

} // namespace sauce::fs::parsers
