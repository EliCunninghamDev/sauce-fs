// SPDX-License-Identifier: MIT

/// \file
/// \brief Index construction, watch mode, and the records written to SQLite.
///
/// This header is the embedding API for the indexer. Build an index once with
/// build_archive_index(), or keep it current with ArchiveIndexWatcher, then
/// read the result back through IndexDatabase in archive_access.hpp.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "log.hpp"

namespace sauce::fs::core
{

/// \brief Kind of main archive indexed in the database.
enum class ArchiveVariant : std::uint8_t
{
    /// \brief A VPK directory file (`*_dir.vpk`) plus its side chunks.
    Vpk,
    /// \brief A pakfile embedded in a BSP map.
    Bsp,
    /// \brief A GMAD addon (`.gma`).
    Gma,
    /// \brief A legacy Garry's Mod addon directory with a `.bin` index.
    GmaBin,
};

/// \brief `archive_type.name` value for \p variant.
///
/// \param variant Archive kind.
/// \return \c "vpk", \c "bsp-pak", \c "gma", or \c "gma-bin".
std::string_view archive_variant_name(ArchiveVariant variant) noexcept;

/// \brief `archive_type.id` for \p variant.
///
/// \param variant Archive kind.
/// \return Stable primary key of the matching archive_type row.
std::int64_t archive_type_id(ArchiveVariant variant) noexcept;

/// \brief Kind of file listed inside an archive.
enum class AssetType : std::uint8_t
{
    /// \brief Extension did not map to a known asset category.
    Unknown,
    /// \brief Model source or compiled model.
    Model,
    /// \brief Material or shader definition.
    Material,
    /// \brief Texture or other image asset.
    Texture,
    /// \brief Sound or music asset.
    Sound,
    /// \brief Map or map-related asset.
    Map,
    /// \brief Script or configuration file.
    Script,
};

/// \brief SQL `assettype` value for \p type.
///
/// \param type Asset category.
/// \return \c "Unknown", \c "Model", \c "Material", \c "Texture", \c "Sound",
/// \c "Map", or \c "Script".
std::string_view asset_type_name(AssetType type) noexcept;

/// \brief One installed game.
///
/// \details The mount exposes it as `sourcegames/<slug>`.
struct GameRecord
{
    /// \brief Mount directory name, unique across games (e.g.
    /// "half-life-2").
    std::string slug;

    /// \brief Display name: the Steam manifest name, or the folder name for
    /// explicit scans.
    std::string title;

    /// \brief Absolute content root (the directory containing gameinfo.txt).
    std::string game_path;
};

/// \brief One row of the archive table.
///
/// \details The hash is the XXH3-64 digest of the archive file itself.
struct ArchiveRecord
{
    /// \brief Absolute path to the archive on disk.
    std::string path;

    /// \brief Owning GameRecord::game_path.
    std::string game_path;

    /// \brief Game-relative directory name for the mount, without the archive
    /// extension.
    ///
    /// For example `maps/c1m1_hotel` for a BSP, `hl2_misc` for
    /// `hl2_misc_dir.vpk`, or `addons/foo` for `addons/foo.gma`. Unique per
    /// game.
    std::string mount_path;

    /// \brief Archive name without its format extension.
    std::string stem;

    /// \brief Archive format.
    ArchiveVariant variant = ArchiveVariant::Vpk;

    /// \brief XXH3-64 digest of the archive file.
    std::uint64_t hash = 0;

    /// \brief Physical archive size in bytes.
    ///
    /// Watch mode compares this and \ref modified against the database to
    /// avoid rehashing unchanged archives.
    std::uint64_t size = 0;

    /// \brief Last-write timestamp as file_time_type ticks since its epoch.
    ///
    /// Watch mode only compares this value with the previous scan; the epoch
    /// is implementation-defined.
    std::int64_t modified = 0;

    /// \brief GMAD addon title; empty for VPK and BSP archives.
    std::string addon_name;

    /// \brief Raw addon.json string: description, type, and tags.
    std::string addon_description;

    /// \brief GMAD addon author; empty for VPK and BSP archives.
    std::string addon_author;

    /// \brief GMAD addon version; zero when not applicable.
    std::int32_t addon_version = 0;

    /// \brief Author's SteamID64 from the GMAD header; zero when absent.
    std::uint64_t steamid64 = 0;

    /// \brief GMAD publication timestamp; zero when absent.
    std::int64_t timestamp = 0;
};

/// \brief One entry (file or directory) listed inside an archive.
///
/// \details Offset locates the stored bytes: absolute in the archive file for
/// BSP, GMA, and embedded VPK entries; relative to the side chunk named by
/// \ref chunk for chunked VPK entries; relative to the decoded GMAD payload for
/// legacy `.bin` archives.
///
/// Directories are emitted explicitly so a read-only mount can answer
/// readdir() with one indexed lookup instead of a prefix scan. A directory row
/// has `directory = true` and no storage fields.
struct ArchiveContent
{
    /// \brief ArchiveRecord::path of the containing archive.
    std::string archive_path;

    /// \brief Archive-internal path, `/`-separated with no leading slash.
    ///
    /// For a directory this is the directory path, not a prefix ending in `/`.
    std::string path;

    /// \brief \c true for a directory row, \c false for a file row.
    bool directory = false;

    /// \brief Byte offset of the stored payload, interpreted as described
    /// above.
    std::uint64_t offset = 0;

    /// \brief VPK side-archive index, or 0x7FFF when the entry data lives in
    /// the directory file itself.
    ///
    /// The value is -1 when chunking does not apply (BSP and GMA).
    std::int32_t chunk = -1;

    /// \brief Stored byte length; the compressed length for compressed
    /// entries.
    std::uint64_t length = 0;

    /// \brief Logical uncompressed size in bytes.
    std::uint64_t size = 0;

    /// \brief Container compression method.
    ///
    /// 0 is stored, 8 is deflate, and 14 is LZMA. Only BSP pakfile ZIP entries
    /// can be compressed today; VPK and GMA entries are stored.
    std::uint8_t method = 0;

    /// \brief Offset of the VPK preload bytes in the `_dir.vpk` file.
    ///
    /// Preload bytes are logically prepended to the \ref length bytes at
    /// \ref offset. Zero for other archive types.
    std::uint64_t preload_offset = 0;

    /// \brief Length of the VPK preload payload; zero when absent.
    std::uint32_t preload_length = 0;

    /// \brief Asset category inferred from the entry extension.
    AssetType asset_type = AssetType::Unknown;
};

/// \brief Options for one full index build.
struct ArchiveIndexOptions
{
    /// \brief Game directories to scan.
    ///
    /// When empty, installed Source titles are discovered through Steam using
    /// \ref steam_roots (or the default Steam locations).
    std::vector<std::filesystem::path> game_folders;

    /// \brief Steam roots used only when \ref game_folders is empty.
    std::vector<std::filesystem::path> steam_roots;

    /// \brief SQLite database path. Defaults to the file named "test".
    std::filesystem::path database = "test";

    /// \brief Diagnostics sink.
    ///
    /// Empty by default, so the library stays silent unless the caller
    /// installs a callback.
    LogOptions log;
};

/// \brief Counts and warnings produced by one index build.
struct ArchiveIndexSummary
{
    /// \brief Number of game folders included in the scan.
    std::size_t game_folders = 0;

    /// \brief Number of Garry's Mod addon roots crawled.
    std::size_t gmod_crawlers = 0;

    /// \brief Number of VPK archives indexed.
    std::size_t vpk = 0;

    /// \brief Number of BSP pakfiles indexed.
    std::size_t bsp = 0;

    /// \brief Number of `.gma` addons indexed.
    std::size_t gma = 0;

    /// \brief Number of legacy `.bin` addons indexed.
    std::size_t gma_bin = 0;

    /// \brief Directory rows included in ArchiveIndexResult::contents.
    std::size_t directories = 0;

    /// \brief Archives whose hash could not be computed.
    std::size_t unreadable = 0;

    /// \brief Per-file and per-archive diagnostics collected during the scan.
    std::vector<std::string> warnings;
};

/// \brief Full snapshot produced by build_archive_index().
struct ArchiveIndexResult
{
    /// \brief Games discovered or supplied for the scan.
    std::vector<GameRecord> games;

    /// \brief Main archives found under the game folders.
    std::vector<ArchiveRecord> archives;

    /// \brief Files and directories; see ArchiveContent::directory.
    std::vector<ArchiveContent> contents;

    /// \brief Counts and warnings for the scan.
    ArchiveIndexSummary summary;

    /// \brief Set only when the database could not be written.
    std::string error;
};

/// \brief Scan installed games and write a complete index database.
///
/// \details Every main archive is scanned (VPK directory files and BSP maps),
/// plus GMA and legacy `.bin` addons when Garry's Mod is present. One worker
/// scans each game folder, and a separate worker crawls the Garry's Mod addon
/// roots; the discovered archives are then hashed in parallel across the
/// worker pool.
///
/// Each archive is also parsed for its internal file listing. The database
/// models the read-only mount directly:
///
/// - `games`: one row per installed game (mount name and content root),
/// - `archives`: game FK, on-disk path, and a game-relative mount path,
/// - `archive_entries`: one row per file *and directory*, keyed by archive.
///
/// Directories are materialized because archive listings only contain files;
/// readdir() then becomes an equality lookup on `(archive_id, parent, name)`
/// instead of a prefix scan. No inode/dentry split is needed: the namespace is
/// read-only and archives cannot express hard links or renames.
///
/// After the workers join, the rows are written to the SQLite database and a
/// summary is reported through \ref ArchiveIndexOptions::log.
///
/// \param options Paths, database target, and diagnostics sink.
/// \param[out] result Receives the full snapshot and summary.
/// \return \c true when the database was written.
/// \retval false when the database could not be written; per-file failures are
/// reported as warnings instead.
/// \note A full build recreates every table, so rows from a previously indexed
/// archive are replaced rather than merged. Use ArchiveIndexWatcher for
/// incremental updates.
bool build_archive_index(const ArchiveIndexOptions& options, ArchiveIndexResult& result);

/// \brief Tuning for ArchiveIndexWatcher.
struct ArchiveWatchOptions
{
    /// \brief Quiet period after the last filesystem event before the index is
    /// rescanned.
    ///
    /// A burst of writes (a game update replacing a VPK tree) therefore
    /// produces one update instead of one per file.
    std::chrono::milliseconds debounce{500};

    /// \brief Emit an Info message for each archive added, removed, or changed.
    bool verbose = true;
};

/// \brief Long-running indexer that keeps a database in sync with its archives.
///
/// \details run() performs a full build and then watches the scanned roots with
/// efsw, applying incremental SQLite updates whenever an archive is added,
/// replaced, or deleted.
///
/// Only archives whose size or modification time changed are rehashed, and an
/// archive that was rewritten without a content change keeps its existing
/// entry rows. Unchanged archives are never parsed again.
class ArchiveIndexWatcher
{
public:
    /// \brief Create a watcher.
    ///
    /// \param options Scan paths and database target; also copied into each
    /// incremental update.
    /// \param watch Debounce and verbosity settings.
    explicit ArchiveIndexWatcher(ArchiveIndexOptions options, ArchiveWatchOptions watch = {});
    ~ArchiveIndexWatcher();

    ArchiveIndexWatcher(const ArchiveIndexWatcher&) = delete;
    ArchiveIndexWatcher& operator=(const ArchiveIndexWatcher&) = delete;

    /// \brief Build the index once, start watching, and block until stop() is
    /// called.
    ///
    /// \param[out] result Receives the initial full snapshot.
    /// \return \c false only when the initial build or an incremental database
    /// update fails; individual archive failures are reported as warnings.
    /// \note Later updates are applied directly to the database and summarized
    /// through the log callback.
    bool run(ArchiveIndexResult& result);

    /// \brief Ask a running run() call to return.
    ///
    /// \note Safe to call from another thread.
    void stop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sauce::fs::core
