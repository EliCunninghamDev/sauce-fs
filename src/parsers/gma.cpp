// Ported from seeass-index's seeass_index.h.
// SPDX-License-Identifier: MIT

#include "gma.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "bytes.hpp"
#include "compression.hpp"

namespace sauce::fs::parsers
{

bool is_legacy_bin_name(std::string_view name) noexcept
{
    return ends_with_ci(name, "_legacy.bin");
}

/// Decode `<fileid>_legacy.bin`: an LZMA-alone GMAD archive with an
/// `0xBEEFCACE` footer.  Returns the decompressed GMA bytes.
bool decode_legacy_bin(const std::filesystem::path& path, std::vector<std::uint8_t>& out,
                       std::string* error)
{
    FileHandle file;
    if (!file.open(path, error))
    {
        return false;
    }
    const std::uint64_t total = file.size();
    if (total < 13 + 8)
    {
        if (error)
        {
            *error = "legacy .bin is too small: " + path.string();
        }
        return false;
    }
    std::vector<std::uint8_t> raw;
    if (!read_file_range(path, 0, total, raw, error))
    {
        return false;
    }
    if (raw.size() >= 4 && std::memcmp(raw.data(), "GMAD", 4) == 0)
    {
        if (error)
        {
            *error = "not a legacy .bin (this is a plain GMA): " + path.string();
        }
        return false;
    }
    const std::size_t footer = raw.size() - 8;
    const std::uint32_t footer_size = load_le32(raw.data() + footer);
    const std::uint32_t footer_magic = load_le32(raw.data() + footer + 4);
    if (footer_magic != 0xBEEFCACEu)
    {
        if (error)
        {
            *error = "not a legacy .bin (footer magic is not BEEFCACE): " + path.string();
        }
        return false;
    }
    std::uint64_t declared = load_le64(raw.data() + 5);
    if (declared == UINT64_MAX)
    {
        declared = footer_size;
    }
    else if ((declared & 0xFFFFFFFFu) != footer_size)
    {
        if (error)
        {
            *error = "legacy .bin footer size does not match its header: " + path.string();
        }
        return false;
    }
    std::vector<std::uint8_t> decoded;
    if (!lzma_alone_decode(raw.data(), raw.size() - 8, declared, decoded, error))
    {
        if (error && !error->empty())
        {
            *error += ": " + path.string();
        }
        return false;
    }
    if (decoded.size() < 5 || std::memcmp(decoded.data(), "GMAD", 4) != 0)
    {
        if (error)
        {
            *error = "legacy .bin did not decompress to a GMAD archive: " + path.string();
        }
        return false;
    }
    out = std::move(decoded);
    return true;
}

template <typename Reader>
bool parse_gma_reader(Reader& file, const std::string& label, std::vector<ScannedEntry>& entries,
                      GmaMetadata* metadata, std::string* error)
{
    if (file.size() < 5)
    {
        if (error)
        {
            *error = "GMA is too small: " + label;
        }
        return false;
    }
    std::uint8_t magic[5] = {};
    if (!file.read_at(0, magic, 5))
    {
        if (error)
        {
            *error = "cannot read GMA header: " + label;
        }
        return false;
    }
    if (std::memcmp(magic, "GMAD", 4) != 0)
    {
        if (error)
        {
            *error = "not a GMAD addon (or a GMAD payload is not at the start): " + label;
        }
        return false;
    }
    const std::uint8_t version = magic[4];
    if (version < 1 || version > 3)
    {
        if (error)
        {
            *error = "unsupported GMA version " + std::to_string(version) + ": " + label;
        }
        return false;
    }
    // Header: steamid u64, timestamp u64, then (v2+) the required-content list,
    // then the addon name, description, and author, followed by an i32 addon
    // version.
    if (!file.seek(5))
    {
        if (error)
        {
            *error = "truncated GMA header: " + label;
        }
        return false;
    }
    std::uint8_t steamid_raw[8] = {};
    std::uint8_t timestamp_raw[8] = {};
    if (!file.read(steamid_raw, 8) || !file.read(timestamp_raw, 8))
    {
        if (error)
        {
            *error = "truncated GMA header: " + label;
        }
        return false;
    }

    GmaMetadata header;
    header.steamid64 = load_le64(steamid_raw);
    header.timestamp = static_cast<std::int64_t>(load_le64(timestamp_raw));

    std::string scratch;
    if (version >= 2)
    {
        // Required content is a list of strings terminated by an empty string.
        for (;;)
        {
            if (!file.read_string(scratch, 1u << 20, error))
            {
                return false;
            }
            if (scratch.empty())
            {
                break;
            }
        }
    }
    if (!file.read_string(header.name, 1u << 20, error) ||
        !file.read_string(header.description, 1u << 20, error) ||
        !file.read_string(header.author, 1u << 20, error))
    {
        return false;
    }
    std::uint8_t version_raw[4] = {};
    if (!file.read(version_raw, 4))
    {
        if (error)
        {
            *error = "truncated GMA header: " + label;
        }
        return false;
    }
    header.version = static_cast<std::int32_t>(load_le32(version_raw));

    std::vector<ScannedEntry> parsed;
    std::uint64_t data_bytes = 0;
    for (;;)
    {
        std::uint8_t raw[4];
        if (!file.read(raw, 4))
        {
            if (error)
            {
                *error = "truncated GMA index: " + label;
            }
            return false;
        }
        const std::uint32_t index =
            static_cast<std::uint32_t>(raw[0]) | (static_cast<std::uint32_t>(raw[1]) << 8) |
            (static_cast<std::uint32_t>(raw[2]) << 16) | (static_cast<std::uint32_t>(raw[3]) << 24);
        if (index == 0)
        {
            break;
        }
        std::string entry_path;
        if (!file.read_string(entry_path, 1u << 20, error))
        {
            return false;
        }
        std::int64_t size = 0;
        std::uint32_t crc = 0;
        std::uint8_t size_raw[8];
        std::uint8_t crc_raw[4];
        if (!file.read(size_raw, 8) || !file.read(crc_raw, 4))
        {
            if (error)
            {
                *error = "truncated GMA index: " + label;
            }
            return false;
        }
        std::uint64_t size_bits = 0;
        for (int i = 0; i < 8; ++i)
        {
            size_bits |= static_cast<std::uint64_t>(size_raw[i]) << (i * 8);
        }
        size = static_cast<std::int64_t>(size_bits);
        crc = static_cast<std::uint32_t>(crc_raw[0]) |
              (static_cast<std::uint32_t>(crc_raw[1]) << 8) |
              (static_cast<std::uint32_t>(crc_raw[2]) << 16) |
              (static_cast<std::uint32_t>(crc_raw[3]) << 24);
        if (size < 0)
        {
            if (error)
            {
                *error = "negative GMA entry size: " + label;
            }
            return false;
        }
        ScannedEntry entry;
        entry.path = normalize_path(trim(entry_path));
        if (entry.path.empty() || entry.path.find("..") != std::string::npos)
        {
            continue;
        }
        entry.offset = data_bytes;
        entry.length = static_cast<std::uint64_t>(size);
        entry.size = static_cast<std::uint64_t>(size);
        entry.crc = crc;
        entry.crc_known = crc != 0;
        std::uint64_t next_data_bytes = 0;
        if (!safe_add(data_bytes, entry.size, next_data_bytes))
        {
            if (error)
            {
                *error = "GMA entry sizes overflow: " + label;
            }
            return false;
        }
        data_bytes = next_data_bytes;
        parsed.push_back(std::move(entry));
    }
    const std::uint64_t data_offset = file.tell();
    if (data_offset == UINT64_MAX)
    {
        if (error)
        {
            *error = "cannot locate GMA data section: " + label;
        }
        return false;
    }
    std::uint64_t end = 0;
    if (!safe_add(data_offset, data_bytes, end) || end > file.size())
    {
        if (error)
        {
            *error = "GMA index declares more data than the file holds: " + label;
        }
        return false;
    }
    for (ScannedEntry& entry : parsed)
    {
        entry.offset += data_offset;
        entries.push_back(std::move(entry));
    }
    if (metadata != nullptr)
    {
        *metadata = std::move(header);
    }
    return true;
}

bool parse_gma(const std::filesystem::path& path, std::vector<ScannedEntry>& entries,
               GmaMetadata* metadata, std::string* error)
{
    FileHandle file;
    if (!file.open(path, error))
    {
        return false;
    }
    return parse_gma_reader(file, path.string(), entries, metadata, error);
}

/// Parse a GMAD archive held in memory (used for decompressed legacy bins).
bool parse_gma_buffer(const std::uint8_t* data, std::size_t size,
                      std::vector<ScannedEntry>& entries, GmaMetadata* metadata, std::string* error)
{
    MemoryReader reader(data, size);
    return parse_gma_reader(reader, "<memory>", entries, metadata, error);
}

} // namespace sauce::fs::parsers
