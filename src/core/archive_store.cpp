// SPDX-License-Identifier: MIT

#include "core/archive_store.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <sqlite3.h>

#include "core/archive_scan.hpp"
#include "core/archive_sql.hpp"
#include "core/sqlite_util.hpp"

namespace sauce::fs::core
{

namespace
{

namespace fs = std::filesystem;

/// Bind the six GMAD header columns, or NULL when the archive is not a GMA.
void bind_addon_metadata(sqlite3_stmt* statement, int first_index, const ArchiveRecord& record)
{
    if (record.variant == ArchiveVariant::Gma || record.variant == ArchiveVariant::GmaBin)
    {
        sqlite::bind_text(statement, first_index, record.addon_name);
        sqlite::bind_text(statement, first_index + 1, record.addon_description);
        sqlite::bind_text(statement, first_index + 2, record.addon_author);
        sqlite3_bind_int(statement, first_index + 3, record.addon_version);
        sqlite3_bind_int64(statement, first_index + 4,
                           static_cast<sqlite3_int64>(record.steamid64));
        sqlite3_bind_int64(statement, first_index + 5, record.timestamp);
        return;
    }
    for (int offset = 0; offset < 6; ++offset)
    {
        sqlite3_bind_null(statement, first_index + offset);
    }
}

bool load_archive_rows(sqlite3* database, std::unordered_map<std::string, StoredArchive>& rows,
                       std::string& error)
{
    sqlite::Statement statement = sqlite::prepare(database, sql::kSelectArchives, error);
    if (!statement)
    {
        return false;
    }

    int step = SQLITE_ROW;
    while ((step = sqlite3_step(statement.get())) == SQLITE_ROW)
    {
        StoredArchive row;
        row.id = sqlite3_column_int64(statement.get(), 0);
        row.game_id = sqlite3_column_int64(statement.get(), 1);
        row.path = sqlite::column_text(statement.get(), 2);
        row.mount_path = sqlite::column_text(statement.get(), 3);
        row.stem = sqlite::column_text(statement.get(), 4);
        row.archive_type_id = sqlite3_column_int64(statement.get(), 5);
        row.hash = sqlite::column_text(statement.get(), 6);
        row.hash_value = parse_hash_hex(row.hash);
        row.size = static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 7));
        row.modified = sqlite3_column_int64(statement.get(), 8);
        row.addon_name = sqlite::column_text(statement.get(), 9);
        row.addon_description = sqlite::column_text(statement.get(), 10);
        row.addon_author = sqlite::column_text(statement.get(), 11);
        row.addon_version =
            sqlite::column_is_null(statement.get(), 12)
                ? 0
                : static_cast<std::int32_t>(sqlite3_column_int(statement.get(), 12));
        row.steamid64 = static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 13));
        row.timestamp = sqlite3_column_int64(statement.get(), 14);
        rows[row.path] = std::move(row);
    }
    if (step != SQLITE_DONE)
    {
        error = sqlite3_errmsg(database);
        return false;
    }
    return true;
}
void split_entry_path(const std::string& path, std::string_view& parent, std::string_view& name)
{
    const std::size_t slash = path.rfind('/');
    if (slash == std::string::npos)
    {
        parent = {};
        name = path;
        return;
    }
    parent = std::string_view(path).substr(0, slash);
    name = std::string_view(path).substr(slash + 1);
}
bool insert_content_rows(sqlite3* database, sqlite3_int64 archive_id,
                         const std::vector<ArchiveContent>& contents, std::string& error)
{
    sqlite::Statement file = sqlite::prepare(database, sql::kInsertEntry, error);
    sqlite::Statement directory = sqlite::prepare(database, sql::kInsertDirectory, error);
    if (!file || !directory)
    {
        return false;
    }

    auto step = [&](sqlite3_stmt* statement) { return sqlite::step_done(statement, error); };

    // Files first: a path that appears both as a file and as a generated
    // directory must keep its file row. Directories then use OR IGNORE.
    for (const ArchiveContent& content : contents)
    {
        if (content.directory)
        {
            continue;
        }
        std::string_view parent;
        std::string_view name;
        split_entry_path(content.path, parent, name);
        sqlite3_bind_int64(file.get(), 1, archive_id);
        sqlite::bind_text(file.get(), 2, content.path);
        sqlite::bind_text(file.get(), 3, parent);
        sqlite::bind_text(file.get(), 4, name);
        sqlite3_bind_int64(file.get(), 5, static_cast<sqlite3_int64>(content.offset));
        if (content.chunk < 0)
        {
            sqlite3_bind_null(file.get(), 6);
        }
        else
        {
            sqlite3_bind_int(file.get(), 6, content.chunk);
        }
        sqlite3_bind_int64(file.get(), 7, static_cast<sqlite3_int64>(content.length));
        sqlite3_bind_int64(file.get(), 8, static_cast<sqlite3_int64>(content.size));
        sqlite3_bind_int(file.get(), 9, static_cast<int>(content.method));
        sqlite3_bind_int64(file.get(), 10, static_cast<sqlite3_int64>(content.preload_offset));
        sqlite3_bind_int(file.get(), 11, static_cast<int>(content.preload_length));
        sqlite::bind_text(file.get(), 12, asset_type_name(content.asset_type));
        if (!step(file.get()))
        {
            return false;
        }
    }

    for (const ArchiveContent& content : contents)
    {
        if (!content.directory)
        {
            continue;
        }
        std::string_view parent;
        std::string_view name;
        split_entry_path(content.path, parent, name);
        sqlite3_bind_int64(directory.get(), 1, archive_id);
        sqlite::bind_text(directory.get(), 2, content.path);
        sqlite::bind_text(directory.get(), 3, parent);
        sqlite::bind_text(directory.get(), 4, name);
        if (!step(directory.get()))
        {
            return false;
        }
    }
    return true;
}
} // namespace

bool write_database(const fs::path& database, const std::vector<GameRecord>& games,
                    const std::vector<ArchiveRecord>& archives,
                    const std::vector<ArchiveContent>& contents, std::string& error)
{
    sqlite::Database db = sqlite::open(database, sqlite::OpenMode::ReadWrite, error);
    if (!db)
    {
        return false;
    }

    // `archive_entries` is the mount namespace. Directories are rows of their
    // own so readdir() is an equality search on (archive_id, parent, name);
    // `path` is denormalized for O(1) lookup and subtree prefix scans. There is
    // no inode table: the tree is read-only and archives cannot express hard
    // links or renames.
    if (!sqlite::exec(db.get(), sql::kCreateSchema, error))
    {
        sqlite::rollback(db.get());
        return false;
    }

    sqlite::Statement game;
    sqlite::Statement archive;
    sqlite::Statement file;
    sqlite::Statement directory;

    auto prepare = [&](sqlite::Statement& statement, std::string_view sql_text)
    {
        statement = sqlite::prepare(db.get(), sql_text, error);
        return static_cast<bool>(statement);
    };

    if (!prepare(game, sql::kInsertGame) || !prepare(archive, sql::kInsertArchive) ||
        !prepare(file, sql::kInsertEntry) || !prepare(directory, sql::kInsertDirectory))
    {
        sqlite::rollback(db.get());
        return false;
    }

    auto step = [&](sqlite3_stmt* statement)
    {
        if (!sqlite::step_done(statement, error))
        {
            sqlite::rollback(db.get());
            return false;
        }
        return true;
    };

    auto split_path = [](const std::string& path, std::string_view& parent, std::string_view& name)
    {
        const std::size_t slash = path.rfind('/');
        if (slash == std::string::npos)
        {
            parent = {};
            name = path;
            return;
        }
        parent = std::string_view(path).substr(0, slash);
        name = std::string_view(path).substr(slash + 1);
    };

    std::unordered_map<std::string, sqlite3_int64> game_ids;
    game_ids.reserve(games.size());
    for (const GameRecord& record : games)
    {
        sqlite::bind_text(game.get(), 1, record.slug);
        sqlite::bind_text(game.get(), 2, record.title);
        sqlite::bind_text(game.get(), 3, record.game_path);
        if (!step(game.get()))
        {
            return false;
        }
        game_ids.emplace(record.game_path, sqlite3_last_insert_rowid(db.get()));
    }

    std::unordered_map<std::string, sqlite3_int64> archive_ids;
    archive_ids.reserve(archives.size());
    for (const ArchiveRecord& record : archives)
    {
        const auto game_row = game_ids.find(record.game_path);
        if (game_row == game_ids.end())
        {
            error = "archive has unknown game: " + record.game_path;
            sqlite::rollback(db.get());
            return false;
        }
        const std::string name = fs::path(record.path).filename().string();
        const std::string hash = hash_hex(record.hash);
        sqlite3_bind_int64(archive.get(), 1, game_row->second);
        sqlite::bind_text(archive.get(), 2, record.path);
        sqlite::bind_text(archive.get(), 3, record.mount_path);
        sqlite::bind_text(archive.get(), 4, name);
        sqlite::bind_text(archive.get(), 5, record.stem);
        sqlite3_bind_int64(archive.get(), 6, archive_type_id(record.variant));
        sqlite::bind_text(archive.get(), 7, hash);
        sqlite3_bind_int64(archive.get(), 8, static_cast<sqlite3_int64>(record.size));
        sqlite3_bind_int64(archive.get(), 9, static_cast<sqlite3_int64>(record.modified));
        bind_addon_metadata(archive.get(), 10, record);
        if (!step(archive.get()))
        {
            return false;
        }
        archive_ids.emplace(record.path, sqlite3_last_insert_rowid(db.get()));
    }

    auto archive_id_for = [&](const std::string& path, sqlite3_int64& id)
    {
        const auto found = archive_ids.find(path);
        if (found == archive_ids.end())
        {
            error = "entry references unknown archive: " + path;
            sqlite::rollback(db.get());
            return false;
        }
        id = found->second;
        return true;
    };

    // Files first: a path that appears both as a file and as a generated
    // directory must keep its file row. Directories then use OR IGNORE.
    for (const ArchiveContent& content : contents)
    {
        if (content.directory)
        {
            continue;
        }
        sqlite3_int64 archive_id = 0;
        if (!archive_id_for(content.archive_path, archive_id))
        {
            return false;
        }
        std::string_view parent;
        std::string_view name;
        split_path(content.path, parent, name);
        sqlite3_bind_int64(file.get(), 1, archive_id);
        sqlite::bind_text(file.get(), 2, content.path);
        sqlite::bind_text(file.get(), 3, parent);
        sqlite::bind_text(file.get(), 4, name);
        sqlite3_bind_int64(file.get(), 5, static_cast<sqlite3_int64>(content.offset));
        if (content.chunk < 0)
        {
            sqlite3_bind_null(file.get(), 6);
        }
        else
        {
            sqlite3_bind_int(file.get(), 6, content.chunk);
        }
        sqlite3_bind_int64(file.get(), 7, static_cast<sqlite3_int64>(content.length));
        sqlite3_bind_int64(file.get(), 8, static_cast<sqlite3_int64>(content.size));
        sqlite3_bind_int(file.get(), 9, static_cast<int>(content.method));
        sqlite3_bind_int64(file.get(), 10, static_cast<sqlite3_int64>(content.preload_offset));
        sqlite3_bind_int(file.get(), 11, static_cast<int>(content.preload_length));
        sqlite::bind_text(file.get(), 12, asset_type_name(content.asset_type));
        if (!step(file.get()))
        {
            return false;
        }
    }

    for (const ArchiveContent& content : contents)
    {
        if (!content.directory)
        {
            continue;
        }
        sqlite3_int64 archive_id = 0;
        if (!archive_id_for(content.archive_path, archive_id))
        {
            return false;
        }
        std::string_view parent;
        std::string_view name;
        split_path(content.path, parent, name);
        sqlite3_bind_int64(directory.get(), 1, archive_id);
        sqlite::bind_text(directory.get(), 2, content.path);
        sqlite::bind_text(directory.get(), 3, parent);
        sqlite::bind_text(directory.get(), 4, name);
        if (!step(directory.get()))
        {
            return false;
        }
    }

    if (!sqlite::commit(db.get(), error))
    {
        sqlite::rollback(db.get());
        return false;
    }
    return true;
}

bool read_archive_rows(const fs::path& database,
                       std::unordered_map<std::string, StoredArchive>& rows, std::string& error)
{
    sqlite::Database db = sqlite::open(database, sqlite::OpenMode::ReadWrite, error);
    if (!db)
    {
        return false;
    }
    return load_archive_rows(db.get(), rows, error);
}
bool apply_database_delta(const fs::path& database, const DatabaseDelta& delta, std::string& error)
{
    sqlite::Database db = sqlite::open(database, sqlite::OpenMode::ReadWrite, error);
    if (!db)
    {
        return false;
    }

    if (!sqlite::begin(db.get(), error, true))
    {
        return false;
    }

    sqlite::Statement find_game;
    sqlite::Statement insert_game;
    sqlite::Statement update_game;
    sqlite::Statement delete_entries;
    sqlite::Statement delete_archive;
    sqlite::Statement stage_mount;
    sqlite::Statement update_archive;
    sqlite::Statement insert_archive;

    auto prepare = [&](sqlite::Statement& statement, std::string_view sql_text)
    {
        statement = sqlite::prepare(db.get(), sql_text, error);
        return static_cast<bool>(statement);
    };

    const bool prepared =
        prepare(find_game, sql::kFindGame) && prepare(insert_game, sql::kInsertGame) &&
        prepare(update_game, sql::kUpdateGame) && prepare(delete_entries, sql::kDeleteEntries) &&
        prepare(delete_archive, sql::kDeleteArchive) &&
        prepare(stage_mount, sql::kStageArchiveMount) &&
        prepare(update_archive, sql::kUpdateArchive) &&
        prepare(insert_archive, sql::kInsertArchive);
    if (!prepared)
    {
        sqlite::rollback(db.get());
        return false;
    }

    auto step = [&](sqlite3_stmt* statement) { return sqlite::step_done(statement, error); };

    std::unordered_map<std::string, sqlite3_int64> game_ids;
    for (const GameRecord& game : delta.games)
    {
        sqlite::bind_text(find_game.get(), 1, game.game_path);
        const int result = sqlite3_step(find_game.get());
        sqlite3_int64 id = 0;
        if (result == SQLITE_ROW)
        {
            id = sqlite3_column_int64(find_game.get(), 0);
            sqlite::reset(find_game.get());
            // Bind order: slug, title, id.
            sqlite::bind_text(update_game.get(), 1, game.slug);
            sqlite::bind_text(update_game.get(), 2, game.title);
            sqlite3_bind_int64(update_game.get(), 3, id);
            if (!step(update_game.get()))
            {
                sqlite::rollback(db.get());
                return false;
            }
        }
        else if (result == SQLITE_DONE)
        {
            sqlite::reset(find_game.get());
            sqlite::bind_text(insert_game.get(), 1, game.slug);
            sqlite::bind_text(insert_game.get(), 2, game.title);
            sqlite::bind_text(insert_game.get(), 3, game.game_path);
            if (!step(insert_game.get()))
            {
                sqlite::rollback(db.get());
                return false;
            }
            id = sqlite3_last_insert_rowid(db.get());
        }
        else
        {
            error = sqlite3_errmsg(db.get());
            sqlite::reset(find_game.get());
            sqlite::rollback(db.get());
            return false;
        }
        game_ids.emplace(game.game_path, id);
    }

    std::unordered_map<std::string, StoredArchive> existing;
    if (!load_archive_rows(db.get(), existing, error))
    {
        sqlite::rollback(db.get());
        return false;
    }

    // Deletions happen first so their mount paths are free for new archives.
    for (const std::string& path : delta.removed)
    {
        const auto found = existing.find(path);
        if (found == existing.end())
        {
            continue;
        }
        sqlite3_bind_int64(delete_entries.get(), 1, found->second.id);
        if (!step(delete_entries.get()))
        {
            sqlite::rollback(db.get());
            return false;
        }
        sqlite3_bind_int64(delete_archive.get(), 1, found->second.id);
        if (!step(delete_archive.get()))
        {
            sqlite::rollback(db.get());
            return false;
        }
        existing.erase(found);
    }

    // A candidate can move to a mount path still held by another existing row
    // (collision suffixes shift when an archive is added). Park every changed
    // row under a temporary name before assigning final paths.
    for (const ArchiveRecord& record : delta.archives)
    {
        const auto found = existing.find(record.path);
        if (found == existing.end() || found->second.mount_path == record.mount_path)
        {
            continue;
        }
        const std::string staged = std::string("\x1f", 1) + std::to_string(found->second.id);
        sqlite::bind_text(stage_mount.get(), 1, staged);
        sqlite3_bind_int64(stage_mount.get(), 2, found->second.id);
        if (!step(stage_mount.get()))
        {
            sqlite::rollback(db.get());
            return false;
        }
    }

    for (const ArchiveRecord& record : delta.archives)
    {
        const auto game = game_ids.find(record.game_path);
        if (game == game_ids.end())
        {
            error = "archive has unknown game: " + record.game_path;
            sqlite::rollback(db.get());
            return false;
        }
        const auto found = existing.find(record.path);
        const auto replaced = delta.replaced.find(record.path);
        const bool replace_entries = replaced != delta.replaced.end();

        if (found == existing.end())
        {
            sqlite3_bind_int64(insert_archive.get(), 1, game->second);
            sqlite::bind_text(insert_archive.get(), 2, record.path);
            sqlite::bind_text(insert_archive.get(), 3, record.mount_path);
            sqlite::bind_text(insert_archive.get(), 4, fs::path(record.path).filename().string());
            sqlite::bind_text(insert_archive.get(), 5, record.stem);
            sqlite3_bind_int64(insert_archive.get(), 6, archive_type_id(record.variant));
            sqlite::bind_text(insert_archive.get(), 7, hash_hex(record.hash));
            sqlite3_bind_int64(insert_archive.get(), 8, static_cast<sqlite3_int64>(record.size));
            sqlite3_bind_int64(insert_archive.get(), 9,
                               static_cast<sqlite3_int64>(record.modified));
            bind_addon_metadata(insert_archive.get(), 10, record);
            if (!step(insert_archive.get()))
            {
                sqlite::rollback(db.get());
                return false;
            }
            const sqlite3_int64 archive_id = sqlite3_last_insert_rowid(db.get());
            if (replace_entries &&
                !insert_content_rows(db.get(), archive_id, replaced->second, error))
            {
                sqlite::rollback(db.get());
                return false;
            }
            continue;
        }

        if (replace_entries)
        {
            sqlite3_bind_int64(delete_entries.get(), 1, found->second.id);
            if (!step(delete_entries.get()))
            {
                sqlite::rollback(db.get());
                return false;
            }
        }

        sqlite3_bind_int64(update_archive.get(), 1, game->second);
        sqlite::bind_text(update_archive.get(), 2, record.mount_path);
        sqlite::bind_text(update_archive.get(), 3, record.stem);
        sqlite3_bind_int64(update_archive.get(), 4, archive_type_id(record.variant));
        sqlite::bind_text(update_archive.get(), 5, hash_hex(record.hash));
        sqlite3_bind_int64(update_archive.get(), 6, static_cast<sqlite3_int64>(record.size));
        sqlite3_bind_int64(update_archive.get(), 7, static_cast<sqlite3_int64>(record.modified));
        bind_addon_metadata(update_archive.get(), 8, record);
        sqlite3_bind_int64(update_archive.get(), 14, found->second.id);
        if (!step(update_archive.get()))
        {
            sqlite::rollback(db.get());
            return false;
        }
        if (replace_entries &&
            !insert_content_rows(db.get(), found->second.id, replaced->second, error))
        {
            sqlite::rollback(db.get());
            return false;
        }
    }

    if (!sqlite::commit(db.get(), error))
    {
        sqlite::rollback(db.get());
        return false;
    }
    return true;
}
} // namespace sauce::fs::core
