// Ported from seeass-index's seeass_index.h.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "scan.hpp"

namespace sauce::fs::parsers
{

/// Parse the ZIP pakfile embedded in a Source 1 BSP (lump 40).  `pak_offset`
/// and `pak_length` describe the lump; entries hold offsets relative to it.
bool parse_bsp_pakfile(const std::filesystem::path& path, std::uint64_t& pak_offset,
                       std::uint64_t& pak_length, std::vector<ScannedEntry>& entries,
                       std::string* error, std::vector<std::string>& warnings);

} // namespace sauce::fs::parsers
