// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "index/archive_index.hpp"

namespace sauce::fs
{

/// One archive candidate found on disk, before it is hashed or opened.
struct Candidate
{
    std::filesystem::path path;
    std::string game_path;
    std::string mount_path;
    ArchiveVariant variant = ArchiveVariant::Vpk;
    std::uint64_t size = 0;
    std::int64_t modified = 0;
};

/// Everything discovered by walking the game folders, before any archive is
/// opened. Both the one-shot build and watch-mode rescans start here.
struct ScanPlan
{
    std::vector<GameRecord> games;
    std::vector<Candidate> candidates;
    std::vector<std::string> warnings;
    std::vector<std::filesystem::path> watch_roots;
    std::size_t gmod_crawlers = 0;
};

/// Absolute, lexically normalized form of `path`.
std::filesystem::path absolute_normalized(const std::filesystem::path& path);

/// Discover games, archive candidates, and watch roots for one scan.
ScanPlan collect_scan_plan(const ArchiveIndexOptions& options);

} // namespace sauce::fs
