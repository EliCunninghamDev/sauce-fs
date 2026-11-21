// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <sauce/core/archive_index.hpp>

namespace sauce::fs::core
{

/// One `archives` row read back from the database for incremental updates.
struct StoredArchive
{
    std::int64_t id = 0;
    std::int64_t game_id = 0;
    std::string path;
    std::string mount_path;
    std::string stem;
    std::int64_t archive_type_id = 0;
    std::string hash;
    std::uint64_t hash_value = 0;
    std::uint64_t size = 0;
    std::int64_t modified = 0;
    std::string addon_name;
    std::string addon_description;
    std::string addon_author;
    std::int32_t addon_version = 0;
    std::uint64_t steamid64 = 0;
    std::int64_t timestamp = 0;
};

/// Incremental changes computed by watch mode and applied in one transaction.
struct DatabaseDelta
{
    std::vector<GameRecord> games;
    std::vector<ArchiveRecord> archives;
    /// Archives whose entry rows must be replaced. An absent key means only
    /// the archive row's metadata changed.
    std::unordered_map<std::string, std::vector<ArchiveContent>> replaced;
    std::vector<std::string> removed;
};

/// Replace the database with a full scan result.
bool write_database(const std::filesystem::path& database, const std::vector<GameRecord>& games,
                    const std::vector<ArchiveRecord>& archives,
                    const std::vector<ArchiveContent>& contents, std::string& error);

/// Load the current `archives` rows keyed by on-disk path.
bool read_archive_rows(const std::filesystem::path& database,
                       std::unordered_map<std::string, StoredArchive>& rows, std::string& error);

/// Apply an incremental update in a single transaction.
bool apply_database_delta(const std::filesystem::path& database, const DatabaseDelta& delta,
                          std::string& error);

} // namespace sauce::fs::core
