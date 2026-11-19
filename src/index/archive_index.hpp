// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace sauce::fs
{

/// Kind of main archive indexed in the database.
enum class ArchiveVariant : std::uint8_t
{
    Vpk,
    Bsp,
    Gma,
    GmaBin,
};

/// SQL `variant` value: "vpk", "bsp-pak", "gma", or "gma-bin".
const char* archive_variant_name(ArchiveVariant variant) noexcept;

/// Kind of file listed inside an archive.
enum class AssetType : std::uint8_t
{
    Unknown,
    Model,
    Material,
    Texture,
    Sound,
    Map,
    Script,
};

/// SQL `assettype` value: "Unknown", "Model", "Material", "Texture", "Sound",
/// "Map", or "Script".
const char* asset_type_name(AssetType type) noexcept;

/// One installed game. The mount exposes it as `sourcegames/<slug>`.
struct GameRecord
{
    /// Mount directory name, unique across games (e.g. "half-life-2").
    std::string slug;
    /// Display name: the Steam manifest name, or the folder name for explicit
    /// scans.
    std::string title;
    /// Absolute content root (the directory containing `gameinfo.txt`).
    std::string game_path;
};

/// One row of the archive table. Hash is the XXH3-64 digest of the archive
/// file itself.
struct ArchiveRecord
{
    std::string path;
    /// Owning GameRecord::game_path.
    std::string game_path;
    /// Game-relative directory name for the mount, without the archive
    /// extension: `maps/c1m1_hotel` for a BSP, `hl2_misc` for
    /// `hl2_misc_dir.vpk`, `addons/foo` for `addons/foo.gma`. Unique per game.
    std::string mount_path;
    std::string stem;
    ArchiveVariant variant = ArchiveVariant::Vpk;
    std::uint64_t hash = 0;
};

/// One entry (file or directory) listed inside an archive. Offset locates the
/// stored bytes: absolute in the archive file for BSP, GMA, and embedded VPK
/// entries; relative to the side chunk named by `chunk` for chunked VPK
/// entries; relative to the decoded GMAD payload for legacy `.bin` archives.
///
/// Directories are emitted explicitly so a read-only mount can answer
/// readdir() with one indexed lookup instead of a prefix scan. A directory row
/// has `directory = true` and no storage fields.
struct ArchiveContent
{
    std::string archive_path;
    /// Archive-internal path, `/`-separated with no leading slash. For a
    /// directory this is the directory path, not a prefix ending in `/`.
    std::string path;
    bool directory = false;
    std::uint64_t offset = 0;
    /// VPK side-archive index, or 0x7FFF when the entry data lives in the
    /// directory file itself. -1 when chunking does not apply (BSP/GMA).
    std::int32_t chunk = -1;
    /// Stored bytes: the compressed length for compressed entries.
    std::uint64_t length = 0;
    /// Logical uncompressed size.
    std::uint64_t size = 0;
    /// Container compression method: 0 stored, 8 deflate, 14 LZMA. Only BSP
    /// pakfile ZIP entries can be compressed today; VPK and GMA are stored.
    std::uint8_t method = 0;
    /// VPK preload bytes: stored at `preload_offset` in the `_dir.vpk` file and
    /// logically prepended to the `length` bytes at `offset`. Zero otherwise.
    std::uint64_t preload_offset = 0;
    std::uint32_t preload_length = 0;
    AssetType asset_type = AssetType::Unknown;
};

struct ArchiveIndexOptions
{
    /// Game directories to scan. When empty, installed Source titles are
    /// discovered through Steam using steam_roots (or the default locations).
    std::vector<std::filesystem::path> game_folders;
    /// Steam roots used only when game_folders is empty.
    std::vector<std::filesystem::path> steam_roots;
    /// SQLite database path. Defaults to the file named "test".
    std::filesystem::path database = "test";
};

struct ArchiveIndexSummary
{
    std::size_t game_folders = 0;
    std::size_t gmod_crawlers = 0;
    std::size_t vpk = 0;
    std::size_t bsp = 0;
    std::size_t gma = 0;
    std::size_t gma_bin = 0;
    /// Directory rows included in `contents`.
    std::size_t directories = 0;
    /// Archives whose hash could not be computed.
    std::size_t unreadable = 0;
    std::vector<std::string> warnings;
};

struct ArchiveIndexResult
{
    std::vector<GameRecord> games;
    std::vector<ArchiveRecord> archives;
    /// Files and directories; see ArchiveContent::directory.
    std::vector<ArchiveContent> contents;
    ArchiveIndexSummary summary;
    /// Set only when the database could not be written.
    std::string error;
};

/// Scan every main archive (VPK directory files and BSP maps), plus GMA and
/// legacy `.bin` addons when Garry's Mod is present. One worker scans each game
/// folder, and a separate worker crawls the Garry's Mod addon roots; the
/// discovered archives are then hashed in parallel across the worker pool.
///
/// Each archive is also parsed for its internal file listing. The database
/// models the future read-only mount directly:
///
///   games          one row per installed game (mount name + content root)
///   archives       game FK, on-disk path, and a game-relative mount path
///   archive_entries one row per file *and directory*, keyed by archive
///
/// Directories are materialized because archive listings only contain files;
/// readdir() then becomes an equality lookup on (archive_id, parent, name)
/// instead of a prefix scan. No inode/dentry split is needed: the namespace is
/// read-only and archives cannot express hard links or renames.
///
/// After the workers join, the rows are written to the SQLite database and a
/// summary is printed to stdout. Returns false only when the database could not
/// be written; per-file failures are reported in the summary.
bool build_archive_index(const ArchiveIndexOptions& options, ArchiveIndexResult& result);

/// Print the same summary used by build_archive_index().
void print_archive_index_summary(const ArchiveIndexResult& result,
                                 const std::filesystem::path& database);

} // namespace sauce::fs
