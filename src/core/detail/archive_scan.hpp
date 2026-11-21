// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <sauce/core/archive_index.hpp>

namespace sauce::fs::core
{

/// Hash an archive file with XXH3-64.
bool compute_hash(const std::filesystem::path& path, std::uint64_t& hash, std::string& error);

/// Lowercase hexadecimal form of a 64-bit hash.
std::string hash_hex(std::uint64_t hash);

std::uint64_t parse_hash_hex(std::string_view text);

/// Coarse asset category inferred from a path's extension.
AssetType classify_asset_type(std::string_view path) noexcept;

/// Parse only the container's directory/index; entry payloads are never read.
/// Updates GMA metadata on `record` and appends files and materialized
/// directories to `contents`.
void scan_contents(const std::filesystem::path& path, ArchiveRecord& record,
                   std::vector<ArchiveContent>& contents, std::vector<std::string>& warnings);

} // namespace sauce::fs::core
