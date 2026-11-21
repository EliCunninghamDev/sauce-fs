// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sauce::fs::core
{

enum class ErrorCode : std::uint8_t
{
    NotFound,
    InvalidPath,
    NotAFile,
    NotADirectory,
    Io,
    Unsupported,
    Database,
    NotSupported,
};

struct Error
{
    ErrorCode code = ErrorCode::Io;
    std::string message;
};

using Bytes = std::vector<std::uint8_t>;
/// Immutable, shared file bytes. Providers return this so callers can keep a
/// reference without copying and providers can evict their cache safely.
using Blob = std::shared_ptr<const Bytes>;

template <typename T> using Result = std::expected<T, Error>;

struct NodeInfo
{
    enum class Kind : std::uint8_t
    {
        Directory,
        File,
    };

    Kind kind = Kind::File;
    std::uint64_t size = 0;
};

struct DirectoryEntry
{
    std::string name;
    NodeInfo info;
};

/// Raw-pointer view used by consumers that cannot handle Result<T>.
/// `blob` keeps `data` alive; directories have `data == nullptr`.
struct ResolvedEntry
{
    NodeInfo info;
    Blob blob;
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

/// A provider is a read-only namespace. Paths are relative to its root and
/// use `/` separators; "" or "/" is the root. Lookups are case-insensitive.
///
/// The same three primitives serve all consumers:
///   get()  random access by path
///   list() folder browsing
///   stat() metadata, including the filesystem mount
class Provider
{
public:
    virtual ~Provider() = default;

    virtual Result<NodeInfo> stat(std::string_view path) const = 0;
    virtual Result<std::vector<DirectoryEntry>> list(std::string_view path) const = 0;
    virtual Result<Blob> get(std::string_view path) const = 0;

    /// Non-expected, non-throwing resolver for consumers that cannot handle
    /// Result<T> or exceptions. Returns false for a missing path or an
    /// I/O/decoding failure. On success `out.blob` owns `out.data`, and
    /// `out.size` is the logical file size. Directories return true with
    /// `out.data == nullptr`.
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

using ProviderPtr = std::shared_ptr<const Provider>;

class ArchiveProvider;

/// Read-only access to an index database. The database object also owns the
/// decoded-file cache, so providers stay valid while they hold a shared_ptr.
class IndexDatabase : public std::enable_shared_from_this<IndexDatabase>
{
public:
    struct GameInfo
    {
        std::int64_t id = 0;
        std::string slug;
        std::string title;
        std::string game_path;
    };

    struct ArchiveInfo
    {
        std::int64_t id = 0;
        std::int64_t game_id = 0;
        std::string path;
        std::string mount_path;
        std::string stem;
        /// `archive_type.name`: "vpk", "bsp-pak", "gma", or "gma-bin".
        std::string variant;
        std::string hash;
        /// GMAD header fields; empty/default for other archive types.
        std::string addon_name;
        std::string addon_description;
        std::string addon_author;
        std::int32_t addon_version = 0;
        std::uint64_t steamid64 = 0;
        std::int64_t timestamp = 0;
    };

    static Result<std::shared_ptr<IndexDatabase>> open(const std::filesystem::path& database);

    ~IndexDatabase();
    IndexDatabase(const IndexDatabase&) = delete;
    IndexDatabase& operator=(const IndexDatabase&) = delete;

    Result<std::vector<GameInfo>> games() const;
    Result<std::vector<ArchiveInfo>> archives() const;

    /// Check whether another process (the index watcher) committed a newer
    /// database snapshot. If so, reload metadata and drop decoded-file caches.
    /// Returns true when the snapshot changed. Lookups call this implicitly,
    /// so a long-lived mount sees updates without being restarted.
    Result<bool> refresh() const;

    /// One archive. Paths are archive-internal, e.g. "materials/foo.vmt".
    Result<ProviderPtr> archive_provider(std::int64_t archive_id) const;

    /// A merged game view. Paths are game-internal. Layers are tried in order;
    /// an empty order uses the index order. The first layer that has a path
    /// wins, and list() unions the layers.
    Result<ProviderPtr> game_provider(std::string_view slug,
                                      std::span<const std::int64_t> archive_order = {}) const;

    /// The per-archive tree: "game/archive-mount-path/internal/path".
    Result<ProviderPtr> mount_provider() const;

    std::uint64_t file_count() const;
    std::uint64_t logical_bytes() const;

private:
    IndexDatabase();

    struct Impl;
    std::unique_ptr<Impl> impl_;

    Result<NodeInfo> stat_entry(std::int64_t archive_id, std::string_view path) const;
    Result<std::vector<DirectoryEntry>> list_entries(std::int64_t archive_id,
                                                     std::string_view parent) const;
    Result<Blob> entry_blob(std::int64_t archive_id, std::string_view path) const;

    friend class ArchiveProvider;
};

/// One archive as a Provider. Paths are archive-internal.
class ArchiveProvider final : public Provider
{
public:
    ArchiveProvider(std::shared_ptr<const IndexDatabase> database, std::int64_t archive_id);

    Result<NodeInfo> stat(std::string_view path) const override;
    Result<std::vector<DirectoryEntry>> list(std::string_view path) const override;
    Result<Blob> get(std::string_view path) const override;

private:
    std::shared_ptr<const IndexDatabase> database_;
    std::int64_t archive_id_ = 0;
};

/// Overlay/merge provider. Layers are ordered by precedence.
class LayeredProvider final : public Provider
{
public:
    explicit LayeredProvider(std::vector<ProviderPtr> layers);

    Result<NodeInfo> stat(std::string_view path) const override;
    Result<std::vector<DirectoryEntry>> list(std::string_view path) const override;
    Result<Blob> get(std::string_view path) const override;

private:
    std::vector<ProviderPtr> layers_;
};

/// Synthetic directory provider used for game and archive levels.
class TreeProvider final : public Provider
{
public:
    struct Child
    {
        std::string name;
        ProviderPtr provider;
    };

    explicit TreeProvider(std::vector<Child> children);

    Result<NodeInfo> stat(std::string_view path) const override;
    Result<std::vector<DirectoryEntry>> list(std::string_view path) const override;
    Result<Blob> get(std::string_view path) const override;

private:
    std::map<std::string, Child> children_;
};

} // namespace sauce::fs::core
