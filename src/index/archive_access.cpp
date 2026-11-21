// SPDX-License-Identifier: MIT

#include "index/archive_access.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>

#include <fmt/format.h>
#include <sqlite3.h>

#include "index/archive_constants.hpp"
#include "index/archive_sql.hpp"
#include "index/sqlite_util.hpp"
#include "parsers/bytes.hpp"
#include "parsers/compression.hpp"
#include "parsers/gma.hpp"

namespace sauce::fs
{

namespace
{

namespace fs = std::filesystem;

constexpr std::uint64_t kFileCacheBytes = 256ull * 1024 * 1024;
constexpr std::int32_t kVpkEmbedded = 0x7FFF;

Error make_error(ErrorCode code, std::string message)
{
    return Error{code, std::move(message)};
}

std::string lower_ascii(std::string_view text)
{
    std::string result;
    result.reserve(text.size());
    for (const char character : text)
    {
        result.push_back(character >= 'A' && character <= 'Z'
                             ? static_cast<char>(character - 'A' + 'a')
                             : character);
    }
    return result;
}

Result<std::vector<std::string>> split_path(std::string_view path)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= path.size())
    {
        const std::size_t slash = path.find('/', start);
        const std::string_view part = path.substr(
            start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (!part.empty() && part != ".")
        {
            if (part == "..")
            {
                return std::unexpected(
                    make_error(ErrorCode::InvalidPath, "path traversal is not allowed"));
            }
            parts.push_back(lower_ascii(part));
        }
        if (slash == std::string_view::npos)
        {
            break;
        }
        start = slash + 1;
    }
    return parts;
}

std::string join_path(const std::vector<std::string>& parts, std::size_t start = 0)
{
    std::string path;
    for (std::size_t index = start; index < parts.size(); ++index)
    {
        if (!path.empty())
        {
            path.push_back('/');
        }
        path += parts[index];
    }
    return path;
}

Result<std::string> normalized_path(std::string_view path)
{
    auto parts = split_path(path);
    if (!parts)
    {
        return std::unexpected(parts.error());
    }
    return join_path(*parts);
}

std::vector<std::string> split_mount_path(std::string_view path)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= path.size())
    {
        const std::size_t slash = path.find('/', start);
        const std::string_view part = path.substr(
            start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (!part.empty())
        {
            parts.emplace_back(part);
        }
        if (slash == std::string_view::npos)
        {
            break;
        }
        start = slash + 1;
    }
    return parts;
}

bool append_range(const std::string& path, std::uint64_t offset, std::uint64_t length, Bytes& out,
                  std::string& error)
{
    if (length == 0)
    {
        return true;
    }
    Bytes chunk;
    if (!parsers::read_file_range(fs::path(path), offset, length, chunk, &error))
    {
        return false;
    }
    out.insert(out.end(), chunk.begin(), chunk.end());
    return true;
}

std::string side_archive_path(const IndexDatabase::ArchiveInfo& archive, std::int32_t chunk)
{
    fs::path path(archive.path);
    std::string stem = archive.stem;
    if (parsers::ends_with_ci(stem, constants::kVpkDirectoryStemSuffix) && stem.size() > 4)
    {
        stem.resize(stem.size() - constants::kVpkDirectoryStemSuffix.size());
    }
    return (path.parent_path() / (stem + fmt::format("_{:03}.vpk", chunk))).string();
}

std::string container_path(const IndexDatabase::ArchiveInfo& archive, std::int32_t chunk)
{
    if (archive.variant == constants::kVpk && chunk >= 0 && chunk != kVpkEmbedded)
    {
        return side_archive_path(archive, chunk);
    }
    return archive.path;
}

} // namespace

struct IndexDatabase::Impl
{
    struct CacheEntry
    {
        Blob blob;
        std::uint64_t used = 0;
    };

    struct Entry
    {
        bool directory = false;
        std::uint64_t size = 0;
        std::uint64_t offset = 0;
        std::int32_t chunk = -1;
        std::uint64_t length = 0;
        std::uint8_t method = 0;
        std::uint64_t preload_offset = 0;
        std::uint32_t preload_length = 0;
    };

    sqlite::Database db;
    mutable std::mutex mutex;
    sqlite::Statement entry_statement;
    sqlite::Statement list_statement;
    sqlite::Statement data_version_statement;
    std::uint64_t data_version = 0;
    std::vector<GameInfo> games;
    std::vector<ArchiveInfo> archives;
    std::unordered_map<std::string, std::size_t> game_by_key;
    std::unordered_map<std::int64_t, std::size_t> game_by_id;
    std::unordered_map<std::int64_t, std::size_t> archive_by_id;
    std::vector<std::vector<std::int64_t>> archives_by_game;
    std::uint64_t file_count = 0;
    std::uint64_t logical_bytes = 0;

    std::unordered_map<std::string, CacheEntry> file_cache;
    std::uint64_t file_cache_bytes = 0;
    std::uint64_t cache_tick = 0;
    std::unordered_map<std::int64_t, Blob> bin_cache;

    ~Impl() = default;

    bool read_data_version_locked(std::uint64_t& version, std::string& error)
    {
        const int result = sqlite3_step(data_version_statement.get());
        if (result != SQLITE_ROW)
        {
            error = result == SQLITE_DONE ? "database did not report a data version"
                                          : sqlite3_errmsg(db.get());
            sqlite::reset(data_version_statement.get());
            return false;
        }
        version = static_cast<std::uint64_t>(sqlite3_column_int64(data_version_statement.get(), 0));
        sqlite::reset(data_version_statement.get());
        return true;
    }

    void clear_metadata_locked()
    {
        games.clear();
        archives.clear();
        game_by_key.clear();
        game_by_id.clear();
        archive_by_id.clear();
        archives_by_game.clear();
        file_count = 0;
        logical_bytes = 0;
        file_cache.clear();
        file_cache_bytes = 0;
        cache_tick = 0;
        bin_cache.clear();
    }

    bool load_metadata_locked(std::string& error)
    {
        clear_metadata_locked();

        sqlite::Statement games_statement = sqlite::prepare(db.get(), sql::kSelectGames, error);
        if (!games_statement)
        {
            return false;
        }
        while (sqlite3_step(games_statement.get()) == SQLITE_ROW)
        {
            GameInfo game;
            game.id = sqlite3_column_int64(games_statement.get(), 0);
            game.slug = sqlite::column_text(games_statement.get(), 1);
            game.title = sqlite::column_text(games_statement.get(), 2);
            game.game_path = sqlite::column_text(games_statement.get(), 3);
            game_by_id.emplace(game.id, games.size());
            game_by_key.emplace(lower_ascii(game.slug), games.size());
            games.push_back(std::move(game));
        }

        sqlite::Statement archives_statement =
            sqlite::prepare(db.get(), sql::kSelectArchiveSummaries, error);
        if (!archives_statement)
        {
            return false;
        }
        archives_by_game.resize(games.size());
        while (sqlite3_step(archives_statement.get()) == SQLITE_ROW)
        {
            ArchiveInfo archive;
            archive.id = sqlite3_column_int64(archives_statement.get(), 0);
            archive.game_id = sqlite3_column_int64(archives_statement.get(), 1);
            archive.path = sqlite::column_text(archives_statement.get(), 2);
            archive.mount_path = sqlite::column_text(archives_statement.get(), 3);
            archive.stem = sqlite::column_text(archives_statement.get(), 4);
            archive.variant = sqlite::column_text(archives_statement.get(), 5);
            archive.hash = sqlite::column_text(archives_statement.get(), 6);
            archive.addon_name = sqlite::column_text(archives_statement.get(), 7);
            archive.addon_description = sqlite::column_text(archives_statement.get(), 8);
            archive.addon_author = sqlite::column_text(archives_statement.get(), 9);
            archive.addon_version =
                sqlite::column_is_null(archives_statement.get(), 10)
                    ? 0
                    : static_cast<std::int32_t>(sqlite3_column_int(archives_statement.get(), 10));
            archive.steamid64 =
                static_cast<std::uint64_t>(sqlite3_column_int64(archives_statement.get(), 11));
            archive.timestamp = sqlite3_column_int64(archives_statement.get(), 12);
            const auto game = game_by_id.find(archive.game_id);
            if (game == game_by_id.end())
            {
                error = "archive references an unknown game: " + archive.path;
                return false;
            }
            archives_by_game[game->second].push_back(archive.id);
            archive_by_id.emplace(archive.id, archives.size());
            archives.push_back(std::move(archive));
        }

        sqlite::Statement totals = sqlite::prepare(db.get(), sql::kSelectEntryTotals, error);
        if (totals)
        {
            if (sqlite3_step(totals.get()) == SQLITE_ROW)
            {
                file_count = static_cast<std::uint64_t>(sqlite3_column_int64(totals.get(), 0));
                logical_bytes = static_cast<std::uint64_t>(sqlite3_column_int64(totals.get(), 1));
            }
        }
        return true;
    }

    bool ensure_current_locked(std::string& error, bool& changed)
    {
        std::uint64_t version = 0;
        if (!read_data_version_locked(version, error))
        {
            return false;
        }
        changed = version != data_version;
        if (!changed)
        {
            return true;
        }
        if (!load_metadata_locked(error))
        {
            return false;
        }
        data_version = version;
        return true;
    }

    bool load(const fs::path& path, std::string& error)
    {
        db = sqlite::open(path, sqlite::OpenMode::ReadOnly, error);
        if (!db)
        {
            return false;
        }

        entry_statement = sqlite::prepare(db.get(), sql::kSelectEntry, error);
        list_statement = sqlite::prepare(db.get(), sql::kSelectDirectory, error);
        data_version_statement = sqlite::prepare(db.get(), sql::kDataVersion, error);
        if (!entry_statement || !list_statement || !data_version_statement)
        {
            return false;
        }

        if (!read_data_version_locked(data_version, error))
        {
            return false;
        }
        return load_metadata_locked(error);
    }

    Result<NodeInfo> stat_entry_locked(std::int64_t archive_id, std::string_view path) const
    {
        if (path.empty())
        {
            return NodeInfo{NodeInfo::Kind::Directory, 0};
        }
        sqlite3_bind_int64(entry_statement.get(), 1, archive_id);
        sqlite::bind_text(entry_statement.get(), 2, path);
        const int result = sqlite3_step(entry_statement.get());
        if (result == SQLITE_DONE)
        {
            sqlite::reset(entry_statement.get());
            return std::unexpected(make_error(ErrorCode::NotFound, "path not found"));
        }
        if (result != SQLITE_ROW)
        {
            const std::string message = sqlite3_errmsg(db.get());
            sqlite::reset(entry_statement.get());
            return std::unexpected(make_error(ErrorCode::Database, message));
        }
        NodeInfo info;
        info.kind = sqlite::column_text(entry_statement.get(), 0) == constants::kDirectoryKind
                        ? NodeInfo::Kind::Directory
                        : NodeInfo::Kind::File;
        info.size = static_cast<std::uint64_t>(sqlite3_column_int64(entry_statement.get(), 1));
        if (info.kind == NodeInfo::Kind::Directory)
        {
            info.size = 0;
        }
        sqlite::reset(entry_statement.get());
        return info;
    }

    Result<std::vector<DirectoryEntry>> list_entries_locked(std::int64_t archive_id,
                                                            std::string_view parent) const
    {
        std::vector<DirectoryEntry> entries;
        sqlite3_bind_int64(list_statement.get(), 1, archive_id);
        sqlite::bind_text(list_statement.get(), 2, parent);
        int result = SQLITE_ROW;
        while ((result = sqlite3_step(list_statement.get())) == SQLITE_ROW)
        {
            if (sqlite::column_is_null(list_statement.get(), 0))
            {
                continue;
            }
            DirectoryEntry entry;
            entry.name = sqlite::column_text(list_statement.get(), 0);
            entry.info.kind =
                sqlite::column_text(list_statement.get(), 1) == constants::kDirectoryKind
                    ? NodeInfo::Kind::Directory
                    : NodeInfo::Kind::File;
            entry.info.size =
                entry.info.kind == NodeInfo::Kind::File
                    ? static_cast<std::uint64_t>(sqlite3_column_int64(list_statement.get(), 2))
                    : 0;
            entries.push_back(std::move(entry));
        }
        if (result != SQLITE_DONE)
        {
            const std::string message = sqlite3_errmsg(db.get());
            sqlite::reset(list_statement.get());
            return std::unexpected(make_error(ErrorCode::Database, message));
        }
        sqlite::reset(list_statement.get());
        return entries;
    }

    Result<Blob> decoded_bin_locked(const ArchiveInfo& archive)
    {
        const auto found = bin_cache.find(archive.id);
        if (found != bin_cache.end())
        {
            return found->second;
        }
        Bytes decoded;
        std::string message;
        if (!parsers::decode_legacy_bin(fs::path(archive.path), decoded, &message))
        {
            return std::unexpected(make_error(ErrorCode::Io, message));
        }
        Blob blob = std::make_shared<Bytes>(std::move(decoded));
        bin_cache.emplace(archive.id, blob);
        return blob;
    }

    Result<Bytes> materialize_locked(const ArchiveInfo& archive, const Entry& entry)
    {
        Bytes out;
        std::string message;
        if (entry.preload_length != 0 &&
            !append_range(archive.path, entry.preload_offset, entry.preload_length, out, message))
        {
            return std::unexpected(make_error(ErrorCode::Io, message));
        }

        if (archive.variant == constants::kGmaBin)
        {
            auto decoded = decoded_bin_locked(archive);
            if (!decoded)
            {
                return std::unexpected(decoded.error());
            }
            const Bytes& bytes = **decoded;
            if (entry.offset > bytes.size() ||
                entry.length > static_cast<std::uint64_t>(bytes.size()) - entry.offset)
            {
                return std::unexpected(
                    make_error(ErrorCode::Io, "entry is outside the decoded legacy bin"));
            }
            const auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(entry.offset);
            out.insert(out.end(), begin, begin + static_cast<std::ptrdiff_t>(entry.length));
        }
        else if (entry.method == 0)
        {
            if (!append_range(container_path(archive, entry.chunk), entry.offset, entry.length, out,
                              message))
            {
                return std::unexpected(make_error(ErrorCode::Io, message));
            }
        }
        else if (entry.method == 8 || entry.method == 14)
        {
            Bytes compressed;
            if (!parsers::read_file_range(fs::path(container_path(archive, entry.chunk)),
                                          entry.offset, entry.length, compressed, &message))
            {
                return std::unexpected(make_error(ErrorCode::Io, message));
            }
            Bytes decoded;
            const bool ok =
                entry.method == 8
                    ? parsers::inflate_raw(compressed.data(), compressed.size(),
                                           static_cast<std::size_t>(entry.size), decoded, &message)
                    : parsers::lzma_zip_decode(compressed.data(), compressed.size(), entry.size,
                                               decoded, &message);
            if (!ok)
            {
                return std::unexpected(make_error(ErrorCode::Io, message));
            }
            out.insert(out.end(), decoded.begin(), decoded.end());
        }
        else
        {
            return std::unexpected(
                make_error(ErrorCode::Unsupported,
                           "unsupported compression method " + std::to_string(entry.method)));
        }

        if (out.size() != entry.size)
        {
            return std::unexpected(
                make_error(ErrorCode::Io, "entry size mismatch in " + archive.path));
        }
        return out;
    }

    Result<Blob> entry_blob_locked(std::int64_t archive_id, std::string_view path)
    {
        sqlite3_bind_int64(entry_statement.get(), 1, archive_id);
        sqlite::bind_text(entry_statement.get(), 2, path);
        const int result = sqlite3_step(entry_statement.get());
        if (result == SQLITE_DONE)
        {
            sqlite::reset(entry_statement.get());
            return std::unexpected(make_error(ErrorCode::NotFound, "path not found"));
        }
        if (result != SQLITE_ROW)
        {
            const std::string message = sqlite3_errmsg(db.get());
            sqlite::reset(entry_statement.get());
            return std::unexpected(make_error(ErrorCode::Database, message));
        }

        Entry entry;
        entry.directory =
            sqlite::column_text(entry_statement.get(), 0) == constants::kDirectoryKind;
        entry.size = static_cast<std::uint64_t>(sqlite3_column_int64(entry_statement.get(), 1));
        entry.offset = static_cast<std::uint64_t>(sqlite3_column_int64(entry_statement.get(), 2));
        entry.chunk = sqlite::column_is_null(entry_statement.get(), 3)
                          ? -1
                          : sqlite3_column_int(entry_statement.get(), 3);
        entry.length = static_cast<std::uint64_t>(sqlite3_column_int64(entry_statement.get(), 4));
        entry.method =
            sqlite::column_is_null(entry_statement.get(), 5)
                ? 0
                : static_cast<std::uint8_t>(sqlite3_column_int(entry_statement.get(), 5));
        entry.preload_offset =
            static_cast<std::uint64_t>(sqlite3_column_int64(entry_statement.get(), 6));
        entry.preload_length =
            static_cast<std::uint32_t>(sqlite3_column_int64(entry_statement.get(), 7));
        sqlite::reset(entry_statement.get());

        if (entry.directory)
        {
            return std::unexpected(make_error(ErrorCode::NotAFile, "path is a directory"));
        }

        const std::string key = std::to_string(archive_id) + ":" + std::string(path);
        const auto cached = file_cache.find(key);
        if (cached != file_cache.end())
        {
            cached->second.used = ++cache_tick;
            return cached->second.blob;
        }

        const auto archive_index = archive_by_id.find(archive_id);
        if (archive_index == archive_by_id.end())
        {
            return std::unexpected(make_error(ErrorCode::NotFound, "archive is not indexed"));
        }
        try
        {
            auto bytes = materialize_locked(archives[archive_index->second], entry);
            if (!bytes)
            {
                return std::unexpected(bytes.error());
            }

            Blob blob = std::make_shared<Bytes>(std::move(*bytes));
            if (blob->size() <= kFileCacheBytes)
            {
                while (file_cache_bytes + blob->size() > kFileCacheBytes && !file_cache.empty())
                {
                    auto victim = file_cache.begin();
                    for (auto it = file_cache.begin(); it != file_cache.end(); ++it)
                    {
                        if (it->second.used < victim->second.used)
                        {
                            victim = it;
                        }
                    }
                    file_cache_bytes -= victim->second.blob->size();
                    file_cache.erase(victim);
                }
                file_cache_bytes += blob->size();
                file_cache.emplace(key, CacheEntry{blob, ++cache_tick});
            }
            return blob;
        }
        catch (const std::bad_alloc&)
        {
            return std::unexpected(make_error(ErrorCode::Io, "out of memory reading entry"));
        }
    }
};

IndexDatabase::IndexDatabase() : impl_(std::make_unique<Impl>()) {}

IndexDatabase::~IndexDatabase() = default;

Result<std::shared_ptr<IndexDatabase>> IndexDatabase::open(const std::filesystem::path& database)
{
    auto result = std::shared_ptr<IndexDatabase>(new IndexDatabase());
    std::string error;
    if (!result->impl_->load(database, error))
    {
        return std::unexpected(make_error(ErrorCode::Database, error));
    }
    return result;
}

Result<std::vector<IndexDatabase::GameInfo>> IndexDatabase::games() const
{
    std::lock_guard lock(impl_->mutex);
    std::string error;
    bool changed = false;
    if (!impl_->ensure_current_locked(error, changed))
    {
        return std::unexpected(make_error(ErrorCode::Database, error));
    }
    return impl_->games;
}

Result<std::vector<IndexDatabase::ArchiveInfo>> IndexDatabase::archives() const
{
    std::lock_guard lock(impl_->mutex);
    std::string error;
    bool changed = false;
    if (!impl_->ensure_current_locked(error, changed))
    {
        return std::unexpected(make_error(ErrorCode::Database, error));
    }
    return impl_->archives;
}

Result<bool> IndexDatabase::refresh() const
{
    std::lock_guard lock(impl_->mutex);
    std::string error;
    bool changed = false;
    if (!impl_->ensure_current_locked(error, changed))
    {
        return std::unexpected(make_error(ErrorCode::Database, error));
    }
    return changed;
}

Result<NodeInfo> IndexDatabase::stat_entry(std::int64_t archive_id, std::string_view path) const
{
    std::lock_guard lock(impl_->mutex);
    std::string error;
    bool changed = false;
    if (!impl_->ensure_current_locked(error, changed))
    {
        return std::unexpected(make_error(ErrorCode::Database, error));
    }
    return impl_->stat_entry_locked(archive_id, path);
}

Result<std::vector<DirectoryEntry>> IndexDatabase::list_entries(std::int64_t archive_id,
                                                                std::string_view parent) const
{
    std::lock_guard lock(impl_->mutex);
    std::string error;
    bool changed = false;
    if (!impl_->ensure_current_locked(error, changed))
    {
        return std::unexpected(make_error(ErrorCode::Database, error));
    }
    return impl_->list_entries_locked(archive_id, parent);
}

Result<Blob> IndexDatabase::entry_blob(std::int64_t archive_id, std::string_view path) const
{
    std::lock_guard lock(impl_->mutex);
    std::string error;
    bool changed = false;
    if (!impl_->ensure_current_locked(error, changed))
    {
        return std::unexpected(make_error(ErrorCode::Database, error));
    }
    return impl_->entry_blob_locked(archive_id, path);
}

Result<ProviderPtr> IndexDatabase::archive_provider(std::int64_t archive_id) const
{
    std::lock_guard lock(impl_->mutex);
    std::string error;
    bool changed = false;
    if (!impl_->ensure_current_locked(error, changed))
    {
        return std::unexpected(make_error(ErrorCode::Database, error));
    }
    if (impl_->archive_by_id.find(archive_id) == impl_->archive_by_id.end())
    {
        return std::unexpected(make_error(ErrorCode::NotFound, "archive is not indexed"));
    }
    return ProviderPtr(std::make_shared<ArchiveProvider>(shared_from_this(), archive_id));
}

Result<ProviderPtr> IndexDatabase::game_provider(std::string_view slug,
                                                 std::span<const std::int64_t> archive_order) const
{
    std::lock_guard lock(impl_->mutex);
    std::string error;
    bool changed = false;
    if (!impl_->ensure_current_locked(error, changed))
    {
        return std::unexpected(make_error(ErrorCode::Database, error));
    }
    const auto game = impl_->game_by_key.find(lower_ascii(slug));
    if (game == impl_->game_by_key.end())
    {
        return std::unexpected(make_error(ErrorCode::NotFound, "game is not indexed"));
    }

    std::vector<std::int64_t> ids;
    if (!archive_order.empty())
    {
        ids.assign(archive_order.begin(), archive_order.end());
    }
    else
    {
        ids = impl_->archives_by_game[game->second];
    }

    std::vector<ProviderPtr> layers;
    layers.reserve(ids.size());
    for (const std::int64_t archive_id : ids)
    {
        if (impl_->archive_by_id.find(archive_id) == impl_->archive_by_id.end())
        {
            return std::unexpected(make_error(ErrorCode::NotFound, "archive is not indexed"));
        }
        layers.push_back(std::make_shared<ArchiveProvider>(shared_from_this(), archive_id));
    }
    if (layers.empty())
    {
        return ProviderPtr(std::make_shared<TreeProvider>(std::vector<TreeProvider::Child>{}));
    }
    return ProviderPtr(std::make_shared<LayeredProvider>(std::move(layers)));
}

Result<ProviderPtr> IndexDatabase::mount_provider() const
{
    std::lock_guard lock(impl_->mutex);
    std::string error;
    bool changed = false;
    if (!impl_->ensure_current_locked(error, changed))
    {
        return std::unexpected(make_error(ErrorCode::Database, error));
    }

    struct Builder
    {
        std::string name;
        std::map<std::string, Builder> children;
        std::int64_t archive_id = -1;
    };

    Builder root;
    for (const GameInfo& game : impl_->games)
    {
        Builder child;
        child.name = game.slug;
        root.children.emplace(lower_ascii(game.slug), std::move(child));
    }

    for (const ArchiveInfo& archive : impl_->archives)
    {
        const auto game = impl_->game_by_id.find(archive.game_id);
        if (game == impl_->game_by_id.end())
        {
            return std::unexpected(make_error(ErrorCode::Database, "archive has no game"));
        }
        const auto game_node = root.children.find(lower_ascii(impl_->games[game->second].slug));
        if (game_node == root.children.end())
        {
            return std::unexpected(make_error(ErrorCode::Database, "game node is missing"));
        }
        Builder* node = &game_node->second;
        const std::vector<std::string> parts = split_mount_path(archive.mount_path);
        if (parts.empty())
        {
            return std::unexpected(
                make_error(ErrorCode::InvalidPath, "archive has an empty mount path"));
        }
        for (std::size_t index = 0; index < parts.size(); ++index)
        {
            const std::string key = lower_ascii(parts[index]);
            if (key.empty() || key == "." || key == "..")
            {
                return std::unexpected(
                    make_error(ErrorCode::InvalidPath, "invalid archive mount path"));
            }
            auto child = node->children.find(key);
            if (child == node->children.end())
            {
                Builder created;
                created.name = parts[index];
                child = node->children.emplace(key, std::move(created)).first;
            }
            node = &child->second;
            if (index + 1 != parts.size() && node->archive_id >= 0)
            {
                return std::unexpected(
                    make_error(ErrorCode::InvalidPath, "archive mount paths overlap"));
            }
        }
        if (node->archive_id >= 0)
        {
            return std::unexpected(
                make_error(ErrorCode::InvalidPath, "duplicate archive mount path"));
        }
        node->archive_id = archive.id;
    }

    std::function<ProviderPtr(Builder&)> build = [&](Builder& node) -> ProviderPtr
    {
        if (node.archive_id >= 0)
        {
            return std::make_shared<ArchiveProvider>(shared_from_this(), node.archive_id);
        }
        std::vector<TreeProvider::Child> children;
        children.reserve(node.children.size());
        for (auto& [key, child] : node.children)
        {
            children.push_back(TreeProvider::Child{child.name, build(child)});
        }
        return std::make_shared<TreeProvider>(std::move(children));
    };
    return build(root);
}

std::uint64_t IndexDatabase::file_count() const
{
    std::lock_guard lock(impl_->mutex);
    std::string error;
    bool changed = false;
    impl_->ensure_current_locked(error, changed);
    return impl_->file_count;
}

std::uint64_t IndexDatabase::logical_bytes() const
{
    std::lock_guard lock(impl_->mutex);
    std::string error;
    bool changed = false;
    impl_->ensure_current_locked(error, changed);
    return impl_->logical_bytes;
}

ArchiveProvider::ArchiveProvider(std::shared_ptr<const IndexDatabase> database,
                                 std::int64_t archive_id)
    : database_(std::move(database)), archive_id_(archive_id)
{
}

Result<NodeInfo> ArchiveProvider::stat(std::string_view path) const
{
    auto normalized = normalized_path(path);
    if (!normalized)
    {
        return std::unexpected(normalized.error());
    }
    if (normalized->empty())
    {
        return NodeInfo{NodeInfo::Kind::Directory, 0};
    }
    return database_->stat_entry(archive_id_, *normalized);
}

Result<std::vector<DirectoryEntry>> ArchiveProvider::list(std::string_view path) const
{
    auto normalized = normalized_path(path);
    if (!normalized)
    {
        return std::unexpected(normalized.error());
    }
    if (!normalized->empty())
    {
        auto info = database_->stat_entry(archive_id_, *normalized);
        if (!info)
        {
            return std::unexpected(info.error());
        }
        if (info->kind != NodeInfo::Kind::Directory)
        {
            return std::unexpected(make_error(ErrorCode::NotADirectory, "path is not a directory"));
        }
    }
    return database_->list_entries(archive_id_, *normalized);
}

Result<Blob> ArchiveProvider::get(std::string_view path) const
{
    auto normalized = normalized_path(path);
    if (!normalized)
    {
        return std::unexpected(normalized.error());
    }
    if (normalized->empty())
    {
        return std::unexpected(make_error(ErrorCode::NotAFile, "root is a directory"));
    }
    return database_->entry_blob(archive_id_, *normalized);
}

LayeredProvider::LayeredProvider(std::vector<ProviderPtr> layers) : layers_(std::move(layers)) {}

Result<NodeInfo> LayeredProvider::stat(std::string_view path) const
{
    for (const ProviderPtr& layer : layers_)
    {
        auto result = layer->stat(path);
        if (result)
        {
            return result;
        }
        if (result.error().code != ErrorCode::NotFound &&
            result.error().code != ErrorCode::NotSupported)
        {
            return std::unexpected(result.error());
        }
    }
    return std::unexpected(make_error(ErrorCode::NotFound, "path not found in any layer"));
}

Result<Blob> LayeredProvider::get(std::string_view path) const
{
    for (const ProviderPtr& layer : layers_)
    {
        auto result = layer->get(path);
        if (result)
        {
            return result;
        }
        if (result.error().code != ErrorCode::NotFound &&
            result.error().code != ErrorCode::NotSupported)
        {
            return std::unexpected(result.error());
        }
    }
    return std::unexpected(make_error(ErrorCode::NotFound, "path not found in any layer"));
}

Result<std::vector<DirectoryEntry>> LayeredProvider::list(std::string_view path) const
{
    std::map<std::string, DirectoryEntry> merged;
    bool found = false;
    for (const ProviderPtr& layer : layers_)
    {
        auto result = layer->list(path);
        if (!result)
        {
            if (result.error().code == ErrorCode::NotFound ||
                result.error().code == ErrorCode::NotSupported)
            {
                continue;
            }
            return std::unexpected(result.error());
        }
        found = true;
        for (DirectoryEntry& entry : *result)
        {
            merged.try_emplace(lower_ascii(entry.name), std::move(entry));
        }
    }
    if (!found)
    {
        return std::unexpected(make_error(ErrorCode::NotFound, "directory not found in any layer"));
    }
    std::vector<DirectoryEntry> entries;
    entries.reserve(merged.size());
    for (auto& [key, entry] : merged)
    {
        entries.push_back(std::move(entry));
    }
    return entries;
}

TreeProvider::TreeProvider(std::vector<Child> children)
{
    for (Child& child : children)
    {
        const std::string key = lower_ascii(child.name);
        children_.try_emplace(key, Child{child.name, std::move(child.provider)});
    }
}

Result<NodeInfo> TreeProvider::stat(std::string_view path) const
{
    auto parts = split_path(path);
    if (!parts)
    {
        return std::unexpected(parts.error());
    }
    if (parts->empty())
    {
        return NodeInfo{NodeInfo::Kind::Directory, 0};
    }
    const auto child = children_.find((*parts)[0]);
    if (child == children_.end())
    {
        return std::unexpected(make_error(ErrorCode::NotFound, "path not found"));
    }
    if (parts->size() == 1)
    {
        return child->second.provider->stat("");
    }
    return child->second.provider->stat(join_path(*parts, 1));
}

Result<std::vector<DirectoryEntry>> TreeProvider::list(std::string_view path) const
{
    auto parts = split_path(path);
    if (!parts)
    {
        return std::unexpected(parts.error());
    }
    if (parts->empty())
    {
        std::vector<DirectoryEntry> entries;
        entries.reserve(children_.size());
        for (const auto& [key, child] : children_)
        {
            auto info = child.provider->stat("");
            if (!info)
            {
                if (info.error().code == ErrorCode::NotFound)
                {
                    continue;
                }
                return std::unexpected(info.error());
            }
            entries.push_back(DirectoryEntry{child.name, *info});
        }
        return entries;
    }

    const auto child = children_.find((*parts)[0]);
    if (child == children_.end())
    {
        return std::unexpected(make_error(ErrorCode::NotFound, "path not found"));
    }
    if (parts->size() == 1)
    {
        return child->second.provider->list("");
    }
    return child->second.provider->list(join_path(*parts, 1));
}

Result<Blob> TreeProvider::get(std::string_view path) const
{
    auto parts = split_path(path);
    if (!parts)
    {
        return std::unexpected(parts.error());
    }
    if (parts->empty())
    {
        return std::unexpected(make_error(ErrorCode::NotAFile, "root is a directory"));
    }
    const auto child = children_.find((*parts)[0]);
    if (child == children_.end())
    {
        return std::unexpected(make_error(ErrorCode::NotFound, "path not found"));
    }
    if (parts->size() == 1)
    {
        return child->second.provider->get("");
    }
    return child->second.provider->get(join_path(*parts, 1));
}

} // namespace sauce::fs
