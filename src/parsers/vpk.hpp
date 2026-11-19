// Ported from seeass-index's seeass_index.h.
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "scan.hpp"

namespace sauce::fs::parsers
{

/// True for `foo_000.vpk`-style side archive names.
bool is_vpk_chunk_name(std::string_view name) noexcept;

/// Given a gameinfo/CLI path, find the actual VPK directory file.  Source's
/// gameinfo commonly names foo.vpk while the file is foo_dir.vpk.
std::filesystem::path resolve_vpk_path(const std::filesystem::path& input);

/// Parse a VPK v1/v2 directory file and append its entries.  Embedded data and
/// side archives are resolved to absolute file offsets.
bool parse_vpk(const std::filesystem::path& path, std::vector<ScannedEntry>& entries,
               std::string* error, std::vector<std::string>& warnings);

} // namespace sauce::fs::parsers
