// Ported from seeass-index's seeass_index.h.
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace sauce::fs::parsers
{

/// One `SearchPaths` entry from gameinfo.txt.
struct GameInfoEntry
{
    std::string types;
    std::string path;
};

/// Read the `SearchPaths` block from gameinfo.txt.  Missing or malformed files
/// produce warnings, not failures.
std::vector<GameInfoEntry> parse_gameinfo_search_paths(const std::filesystem::path& file,
                                                       std::vector<std::string>& warnings);

/// Case-insensitive `*` / `?` glob match.
bool glob_match(std::string_view pattern, std::string_view text);

/// Expand `*` and `?` in the final path component.  Paths without wildcards are
/// returned unchanged.
std::vector<std::filesystem::path> expand_wildcard(const std::filesystem::path& pattern);

} // namespace sauce::fs::parsers
