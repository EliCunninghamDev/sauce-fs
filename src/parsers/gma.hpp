// Ported from seeass-index's seeass_index.h.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "scan.hpp"

namespace sauce::fs::parsers
{

/// Metadata carried in a GMAD header. `description` is the raw addon.json
/// string; Garry's Mod stores the addon description, type, and tags there.
struct GmaMetadata
{
    std::string name;
    std::string description;
    std::string author;
    std::int32_t version = 0;
    std::uint64_t steamid64 = 0;
    std::int64_t timestamp = 0;
};

/// Parse a GMAD (Garry's Mod addon) archive from disk. When `metadata` is not
/// null it receives the header fields; it is only written on success.
bool parse_gma(const std::filesystem::path& path, std::vector<ScannedEntry>& entries,
               GmaMetadata* metadata, std::string* error);

/// Parse a GMAD archive held in memory (used for decompressed legacy bins).
bool parse_gma_buffer(const std::uint8_t* data, std::size_t size,
                      std::vector<ScannedEntry>& entries, GmaMetadata* metadata,
                      std::string* error);

/// True for `*_legacy.bin` Workshop container names.
bool is_legacy_bin_name(std::string_view name) noexcept;

/// Decode `<fileid>_legacy.bin`: an LZMA-alone GMAD archive with an
/// `0xBEEFCACE` footer.  Returns the decompressed GMA bytes.
bool decode_legacy_bin(const std::filesystem::path& path, std::vector<std::uint8_t>& out,
                       std::string* error);

} // namespace sauce::fs::parsers
