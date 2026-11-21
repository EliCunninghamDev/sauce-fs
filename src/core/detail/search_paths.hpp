// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <sauce/core/archive_index.hpp>

namespace sauce::fs::core
{

/// Absolute path to the game's gameinfo file, or empty when absent.
std::filesystem::path find_gameinfo(const std::filesystem::path& game_path);

/// Parse and resolve the game's `SearchPaths` block.
///
/// Tokens such as `|gameinfo_path|` and `|all_source_engine_paths|` are
/// substituted, wildcards are expanded, and `foo.vpk` is mapped to the actual
/// `foo_dir.vpk` when only the directory file exists.
std::vector<GameSearchPath> load_search_paths(const std::filesystem::path& game_path,
                                              const std::filesystem::path& gameinfo,
                                              std::vector<std::string>& warnings);

/// Read Garry's Mod `cfg/mount.cfg`, if present.
std::vector<GameMount> load_game_mounts(const std::filesystem::path& game_path,
                                        std::vector<std::string>& warnings);

} // namespace sauce::fs::core
