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

/// Parse a GMAD (Garry's Mod addon) archive from disk.
bool parse_gma(const std::filesystem::path& path, std::vector<ScannedEntry>& entries,
               std::string* error);

/// Parse a GMAD archive held in memory (used for decompressed legacy bins).
bool parse_gma_buffer(const std::uint8_t* data, std::size_t size,
                      std::vector<ScannedEntry>& entries, std::string* error);

/// True for `*_legacy.bin` Workshop container names.
bool is_legacy_bin_name(std::string_view name) noexcept;

/// Decode `<fileid>_legacy.bin`: an LZMA-alone GMAD archive with an
/// `0xBEEFCACE` footer.  Returns the decompressed GMA bytes.
bool decode_legacy_bin(const std::filesystem::path& path, std::vector<std::uint8_t>& out,
                       std::string* error);

} // namespace sauce::fs::parsers
