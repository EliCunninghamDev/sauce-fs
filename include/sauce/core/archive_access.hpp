// SPDX-License-Identifier: MIT

/// \file
/// \brief Public error, metadata, and provider types for the archive index.
///
/// Consumers use this header to open an index database and walk its archives
/// through the provider interface. Nothing here exposes the archive parsers or
/// the SQLite schema.

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sauce::fs::core
{

/// \brief Failure categories reported by the provider API and database layer.
///
/// Callers should branch on this value; Error::message is informative only and
/// is not stable across releases.
enum class ErrorCode : std::uint8_t
{
    /// \brief The requested path or record does not exist.
    NotFound,
    /// \brief The path is malformed, for example one that escapes the root.
    InvalidPath,
    /// \brief The path names a directory, but the operation requires a file.
    NotAFile,
    /// \brief The path names a file, but the operation requires a directory.
    NotADirectory,
    /// \brief Reading or decoding archive data failed.
    Io,
    /// \brief The archive uses a feature this build does not support.
    Unsupported,
    /// \brief The SQLite index could not be read or is inconsistent.
    Database,
    /// \brief The provider does not implement the requested operation.
    NotSupported,
};

/// \brief One error value returned through Result.
struct Error
{
    /// \brief Category used by callers to branch without parsing text.
    ErrorCode code = ErrorCode::Io;

    /// \brief Human-readable diagnostic; not stable across releases.
    std::string message;
};

/// \brief Mutable byte buffer used while decoding an archive entry.
using Bytes = std::vector<std::uint8_t>;

/// \brief Immutable, shared file bytes.
///
/// Providers return this so callers can keep a reference without copying and
/// providers can evict their cache safely.
using Blob = std::shared_ptr<const Bytes>;

/// \brief Expected value or Error, used by every fallible provider operation.
/// \tparam T Value type produced on success.
template <typename T> using Result = std::expected<T, Error>;

/// \brief Metadata for one path in a provider namespace.
struct NodeInfo
{
    /// \brief Whether the path names a directory or a regular file.
    enum class Kind : std::uint8_t
    {
        /// \brief A directory that can be passed to Provider::list().
        Directory,
        /// \brief A regular file that can be passed to Provider::get().
        File,
    };

    /// \brief Kind of node.
    Kind kind = Kind::File;

    /// \brief Logical size in bytes; zero for directories.
    std::uint64_t size = 0;
};

/// \brief One entry returned by Provider::list().
struct DirectoryEntry
{
    /// \brief Name relative to the listed directory, without a trailing slash.
    std::string name;

    /// \brief Metadata for the entry.
    NodeInfo info;
};

/// \brief Raw-pointer view used by consumers that cannot handle Result<T>.
///
/// \details On success, \ref ResolvedEntry::blob keeps \ref ResolvedEntry::data
/// alive. Directories have `data == nullptr` and a zero
/// \ref ResolvedEntry::size.
struct ResolvedEntry
{
    /// \brief Metadata for the resolved path.
    NodeInfo info;

    /// \brief Owner of \ref ResolvedEntry::data for files; empty for
    /// directories.
    Blob blob;

    /// \brief File bytes, or \c nullptr for a directory.
    const std::uint8_t* data = nullptr;

    /// \brief Logical size of the file in bytes.
    std::size_t size = 0;
};

/// \brief Read-only namespace over one archive or a merged game view.
///
/// \details Paths are relative to the provider's root and use `/` separators;
/// \c "" or \c "/" names the root. Lookups are case-insensitive.
///
/// The same three primitives serve all consumers:
/// - Provider::get() for random access by path,
/// - Provider::list() for folder browsing,
/// - Provider::stat() for metadata, including the filesystem mount.
class Provider
{
public:
    virtual ~Provider() = default;

    /// \brief Look up metadata for \p path.
    ///
    /// \param path Namespace-relative path; \c "" and \c "/" name the root.
    /// \return Node kind and logical size on success.
    /// \retval ErrorCode::NotFound when \p path does not exist.
    virtual Result<NodeInfo> stat(std::string_view path) const = 0;

    /// \brief List the immediate children of a directory.
    ///
    /// \param path Directory path relative to this provider's root.
    /// \return One entry per immediate child.
    /// \retval ErrorCode::NotADirectory when \p path names a file.
    /// \retval ErrorCode::NotFound when \p path does not exist.
    virtual Result<std::vector<DirectoryEntry>> list(std::string_view path) const = 0;

    /// \brief Read and decode a file.
    ///
    /// \param path File path relative to this provider's root.
    /// \return Immutable bytes, possibly shared with the provider's cache.
    /// \retval ErrorCode::NotAFile when \p path names a directory.
    /// \retval ErrorCode::Io when the archive data cannot be read or decoded.
    virtual Result<Blob> get(std::string_view path) const = 0;

    /// \brief Non-expected, non-throwing resolver for consumers that cannot
    /// handle Result<T> or exceptions.
    ///
    /// \details Returns \c false for a missing path or an I/O/decoding
    /// failure. On success, `out.blob` owns `out.data`, and `out.size` is the
    /// logical file size. Directories return \c true with
    /// `out.data == nullptr`.
    ///
    /// \param path Namespace-relative path to resolve.
    /// \param[out] out Receives the resolved entry; reset on failure.
    /// \return \c true when the path exists and, for files, was decoded.
    /// \note Never throws.
    virtual bool resolve(std::string_view path, ResolvedEntry& out) const noexcept
    {
        out = ResolvedEntry{};
        try
        {
            auto info = stat(path);
            if (!info)
            {
                return false;
            }
            out.info = *info;
            if (info->kind == NodeInfo::Kind::Directory)
            {
                return true;
            }
            auto blob = get(path);
            if (!blob)
            {
                return false;
            }
            out.blob = std::move(*blob);
            out.data = out.blob->data();
            out.size = out.blob->size();
            return true;
        }
        catch (...)
        {
            out = ResolvedEntry{};
            return false;
        }
    }
};

/// \brief Shared, immutable handle to a Provider.
using ProviderPtr = std::shared_ptr<const Provider>;

class ArchiveProvider;
class LooseFileProvider;

/// \brief Ordering policy used by ordered_game_provider().
enum class GameOrder : std::uint8_t
{
    /// \brief Garry's Mod rules for Garry's Mod, engine search paths
    /// otherwise, and index order when neither is available.
    Auto,
    /// \brief Source engine `SearchPaths` order from gameinfo.
    Engine,
    /// \brief Garry's Mod addon and mounted-game order.
    GarrysMod,
    /// \brief Index insertion order; loose files are placed on top.
    Index,
};

/// \brief Read-only access to an index database.
///
/// \details The database object also owns the decoded-file cache, so providers
/// stay valid while they hold a shared_ptr.
class IndexDatabase : public std::enable_shared_from_this<IndexDatabase>
{
public:
    /// \brief One installed game as recorded in the index.
    struct GameInfo
    {
        /// \brief One parsed gameinfo `SearchPaths` entry.
        struct SearchPath
        {
            /// \brief Access flags, for example "game+mod".
            std::string types;

            /// \brief Raw path text from gameinfo.
            std::string path;

            /// \brief Absolute resolved path, or empty when missing.
            std::string resolved;

            /// \brief "dir", "vpk", "file", or "missing".
            std::string kind;
        };

        /// \brief One game root mounted through Garry's Mod.
        struct Mount
        {
            /// \brief Key from `cfg/mount.cfg`.
            std::string name;

            /// \brief Absolute mounted content root.
            std::string path;

            /// \brief Configuration file that declared the mount.
            std::string source;
        };

        /// \brief Database row identifier.
        std::int64_t id = 0;

        /// \brief Mount directory name, unique across games.
        std::string slug;

        /// \brief Display name from the Steam manifest or game folder.
        std::string title;

        /// \brief Absolute content root, the directory containing gameinfo.txt.
        std::string game_path;

        /// \brief Absolute gameinfo path, or empty when absent.
        std::string gameinfo;

        /// \brief Whether this game uses Garry's Mod addon mounting rules.
        bool garrys_mod = false;

        /// \brief Engine search paths in gameinfo order.
        std::vector<SearchPath> search_paths;

        /// \brief Garry's Mod `cfg/mount.cfg` entries in file order.
        std::vector<Mount> mounts;
    };

    /// \brief One archive as recorded in the index.
    struct ArchiveInfo
    {
        /// \brief Database row identifier, accepted by archive_provider().
        std::int64_t id = 0;

        /// \brief Owning game row identifier.
        std::int64_t game_id = 0;

        /// \brief Absolute path to the archive on disk.
        std::string path;

        /// \brief Game-relative path used by mount_provider().
        std::string mount_path;

        /// \brief Archive name without its format extension.
        std::string stem;

        /// \brief `archive_type.name`: "vpk", "bsp-pak", "gma", or "gma-bin".
        std::string variant;

        /// \brief XXH3-64 digest of the archive file, as hexadecimal text.
        std::string hash;

        /// \brief GMAD addon title; empty for other archive types.
        std::string addon_name;

        /// \brief Raw addon.json string with description, type, and tags.
        std::string addon_description;

        /// \brief GMAD addon author; empty for other archive types.
        std::string addon_author;

        /// \brief GMAD addon version; zero when not applicable.
        std::int32_t addon_version = 0;

        /// \brief Author's SteamID64 from the GMAD header; zero when absent.
        std::uint64_t steamid64 = 0;

        /// \brief GMAD publication timestamp; zero when absent.
        std::int64_t timestamp = 0;
    };

    /// \brief Open an existing SQLite index.
    ///
    /// \param database Path to a database written by build_archive_index() or
    /// ArchiveIndexWatcher.
    /// \return A shared database handle.
    /// \retval ErrorCode::Database when the file is missing, unreadable, or not
    /// a sauce-fs index.
    static Result<std::shared_ptr<IndexDatabase>> open(const std::filesystem::path& database);

    ~IndexDatabase();
    IndexDatabase(const IndexDatabase&) = delete;
    IndexDatabase& operator=(const IndexDatabase&) = delete;

    /// \brief Every game recorded in the index.
    Result<std::vector<GameInfo>> games() const;

    /// \brief Every archive recorded in the index.
    Result<std::vector<ArchiveInfo>> archives() const;

    /// \brief Check whether the watcher committed a newer database snapshot.
    ///
    /// \details If a newer snapshot exists, reload metadata and drop
    /// decoded-file caches. Lookups call this implicitly, so a long-lived
    /// mount sees updates without being restarted.
    ///
    /// \return \c true when the snapshot changed.
    /// \retval ErrorCode::Database when the new snapshot cannot be read.
    Result<bool> refresh() const;

    /// \brief Open one archive as a provider.
    ///
    /// \param archive_id ArchiveInfo::id of the archive to open.
    /// \return Provider whose paths are archive-internal.
    /// \retval ErrorCode::NotFound when no archive has that identifier.
    Result<ProviderPtr> archive_provider(std::int64_t archive_id) const;

    /// \brief Build a merged view of one game.
    ///
    /// \details Loose files below the game root are placed above the archive
    /// layers. Archive layers are tried in order; the first layer that has a
    /// path wins, and Provider::list() unions the layers.
    ///
    /// \param slug GameInfo::slug of the game to open.
    /// \param archive_order Archive identifiers in precedence order; an empty
    /// span uses the index order.
    /// \return Provider whose paths are game-internal.
    Result<ProviderPtr> game_provider(std::string_view slug,
                                      std::span<const std::int64_t> archive_order = {}) const;

    /// \brief Build a merged view using the Source engine's gameinfo order.
    ///
    /// \details Each search path becomes one layer, in file order. Directory
    /// entries contribute loose files; VPK entries contribute the matching
    /// archive. Map pakfiles are appended as the lowest-precedence layers
    /// because they are scoped to one map.
    ///
    /// \param slug GameInfo::slug of the game to open.
    /// \return Provider whose paths are game-internal.
    /// \retval ErrorCode::NotFound when no game has that slug.
    Result<ProviderPtr> engine_game_provider(std::string_view slug) const;

    /// \brief Build a merged view using Garry's Mod's mount rules.
    ///
    /// \details Workshop addons override local addons, local addons override
    /// engine content, and `cfg/mount.cfg` games sit below Garry's Mod's own
    /// content. Addon archives are ordered by mount path, with later names
    /// winning, matching Garry's Mod's alphabetical addon order.
    ///
    /// \param slug GameInfo::slug of the game to open.
    /// \return Provider whose paths are game-internal.
    /// \retval ErrorCode::NotFound when no game has that slug.
    Result<ProviderPtr> garrysmod_game_provider(std::string_view slug) const;

    /// \brief Build a merged view with an explicit ordering policy.
    ///
    /// \param slug GameInfo::slug of the game to open.
    /// \param order Ordering policy; GameOrder::Auto selects Garry's Mod rules
    /// for a Garry's Mod index row, engine search paths otherwise.
    /// \return Provider whose paths are game-internal.
    Result<ProviderPtr> ordered_game_provider(std::string_view slug, GameOrder order) const;

    /// \brief Build a provider over loose files below \p root.
    ///
    /// \param root Absolute content root recorded as LooseEntry::root.
    /// \param prefix Optional path below \p root to scope the provider to.
    /// \return Provider whose paths are relative to the prefix.
    Result<ProviderPtr> loose_provider(std::filesystem::path root,
                                       std::string_view prefix = {}) const;

    /// \brief Build the per-archive tree exposed by the FUSE mount.
    ///
    /// \details Each game directory contains both the archive trees and the
    /// loose files recorded below the game root. Loose files win when a loose
    /// path collides with an archive mount directory.
    ///
    /// \return Provider rooted at the equivalent of `sourcegames`, with paths
    /// of the form "game/archive-mount-path/internal/path".
    Result<ProviderPtr> mount_provider() const;

    /// \brief Number of indexed files, excluding directory rows.
    std::uint64_t file_count() const;

    /// \brief Sum of the logical (uncompressed) sizes of indexed files.
    std::uint64_t logical_bytes() const;

private:
    IndexDatabase();

    struct Impl;
    std::unique_ptr<Impl> impl_;

    Result<NodeInfo> stat_entry(std::int64_t archive_id, std::string_view path) const;
    Result<std::vector<DirectoryEntry>> list_entries(std::int64_t archive_id,
                                                     std::string_view parent) const;
    Result<Blob> entry_blob(std::int64_t archive_id, std::string_view path) const;

    Result<NodeInfo> loose_stat(std::string_view root, std::string_view key) const;
    Result<std::vector<DirectoryEntry>> loose_list(std::string_view root,
                                                   std::string_view parent_key) const;
    Result<Blob> loose_blob(std::string_view root, std::string_view key) const;

    friend class ArchiveProvider;
    friend class LooseFileProvider;
};

/// \brief One archive exposed as a read-only Provider.
///
/// \details Paths are archive-internal, such as "materials/foo.vmt".
class ArchiveProvider final : public Provider
{
public:
    /// \brief Bind a provider to one archive in \p database.
    ///
    /// \param database Open index database that owns the archive.
    /// \param archive_id ArchiveInfo::id to expose.
    ArchiveProvider(std::shared_ptr<const IndexDatabase> database, std::int64_t archive_id);

    /// \copydoc Provider::stat
    Result<NodeInfo> stat(std::string_view path) const override;

    /// \copydoc Provider::list
    Result<std::vector<DirectoryEntry>> list(std::string_view path) const override;

    /// \copydoc Provider::get
    Result<Blob> get(std::string_view path) const override;

private:
    std::shared_ptr<const IndexDatabase> database_;
    std::int64_t archive_id_ = 0;
};

/// \brief Loose files below one indexed content root.
///
/// \details Paths are relative to the provider's prefix. Listings come from the
/// index; file bytes are read from disk on demand, so edits made while a mount
/// is live are visible without reindexing.
class LooseFileProvider final : public Provider
{
public:
    /// \brief Bind a provider to loose files below \p root.
    ///
    /// \param database Open index database that owns the rows.
    /// \param root Absolute content root to serve.
    /// \param prefix Optional path below \p root used as this provider's root.
    LooseFileProvider(std::shared_ptr<const IndexDatabase> database, std::string root,
                      std::string prefix = {});

    /// \copydoc Provider::stat
    Result<NodeInfo> stat(std::string_view path) const override;

    /// \copydoc Provider::list
    Result<std::vector<DirectoryEntry>> list(std::string_view path) const override;

    /// \copydoc Provider::get
    Result<Blob> get(std::string_view path) const override;

private:
    std::shared_ptr<const IndexDatabase> database_;
    std::string root_;
    std::string prefix_key_;
};

/// \brief Overlay provider that merges several layers by precedence.
class LayeredProvider final : public Provider
{
public:
    /// \brief Create a provider over \p layers.
    ///
    /// \param layers Providers ordered by precedence; the first match wins.
    explicit LayeredProvider(std::vector<ProviderPtr> layers);

    /// \copydoc Provider::stat
    Result<NodeInfo> stat(std::string_view path) const override;

    /// \copydoc Provider::list
    Result<std::vector<DirectoryEntry>> list(std::string_view path) const override;

    /// \copydoc Provider::get
    Result<Blob> get(std::string_view path) const override;

private:
    std::vector<ProviderPtr> layers_;
};

/// \brief Synthetic directory provider used for game and archive levels.
class TreeProvider final : public Provider
{
public:
    /// \brief One named child directory.
    struct Child
    {
        /// \brief Entry name shown by Provider::list() on the parent.
        std::string name;

        /// \brief Provider backing the child entry.
        ProviderPtr provider;
    };

    /// \brief Create a directory provider over \p children.
    ///
    /// \param children Named child providers.
    explicit TreeProvider(std::vector<Child> children);

    /// \copydoc Provider::stat
    Result<NodeInfo> stat(std::string_view path) const override;

    /// \copydoc Provider::list
    Result<std::vector<DirectoryEntry>> list(std::string_view path) const override;

    /// \copydoc Provider::get
    Result<Blob> get(std::string_view path) const override;

private:
    std::map<std::string, Child> children_;
};

} // namespace sauce::fs::core
