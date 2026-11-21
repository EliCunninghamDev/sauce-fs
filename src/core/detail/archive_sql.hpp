// SPDX-License-Identifier: MIT

#pragma once

#include <string_view>

/// SQL text used by the index writer and the runtime reader. Raw strings keep
/// the statements readable and avoid line-by-line literal concatenation.
namespace sauce::fs::core::sql
{

inline constexpr std::string_view kForeignKeysOn = R"sql(PRAGMA foreign_keys = ON;)sql";
inline constexpr std::string_view kJournalModeWal = R"sql(PRAGMA journal_mode = WAL;)sql";
inline constexpr std::string_view kDataVersion = R"sql(PRAGMA data_version)sql";

inline constexpr std::string_view kBegin = R"sql(BEGIN;)sql";
inline constexpr std::string_view kBeginImmediate = R"sql(BEGIN IMMEDIATE;)sql";
inline constexpr std::string_view kCommit = R"sql(COMMIT;)sql";
inline constexpr std::string_view kRollback = R"sql(ROLLBACK;)sql";

/// Full index schema. The transaction is committed by the caller after all
/// rows have been inserted.
inline constexpr std::string_view kCreateSchema = R"sql(
BEGIN;
DROP TABLE IF EXISTS loose_entries;
DROP TABLE IF EXISTS game_mounts;
DROP TABLE IF EXISTS game_search_paths;
DROP TABLE IF EXISTS archive_entries;
DROP TABLE IF EXISTS archives;
DROP TABLE IF EXISTS games;
DROP TABLE IF EXISTS archive_type;
CREATE TABLE games(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    slug TEXT NOT NULL UNIQUE,
    title TEXT NOT NULL,
    game_path TEXT NOT NULL UNIQUE,
    gameinfo TEXT NOT NULL DEFAULT '',
    garrys_mod INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE archive_type(
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL UNIQUE
);
INSERT INTO archive_type(id, name) VALUES
    (1, 'vpk'),
    (2, 'bsp-pak'),
    (3, 'gma'),
    (4, 'gma-bin');
CREATE TABLE game_search_paths(
    game_id INTEGER NOT NULL REFERENCES games(id),
    ordinal INTEGER NOT NULL,
    types TEXT NOT NULL,
    raw TEXT NOT NULL,
    resolved TEXT NOT NULL,
    kind TEXT NOT NULL,
    PRIMARY KEY(game_id, ordinal)
) WITHOUT ROWID;
CREATE TABLE game_mounts(
    game_id INTEGER NOT NULL REFERENCES games(id),
    ordinal INTEGER NOT NULL,
    name TEXT NOT NULL,
    path TEXT NOT NULL,
    source TEXT NOT NULL,
    PRIMARY KEY(game_id, ordinal)
) WITHOUT ROWID;
CREATE TABLE archives(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    game_id INTEGER NOT NULL REFERENCES games(id),
    path TEXT NOT NULL UNIQUE,
    mount_path TEXT NOT NULL,
    name TEXT NOT NULL,
    stem TEXT NOT NULL,
    archive_type_id INTEGER NOT NULL REFERENCES archive_type(id),
    hash TEXT NOT NULL,
    size INTEGER NOT NULL DEFAULT 0,
    modified INTEGER NOT NULL DEFAULT 0,
    addon_name TEXT,
    addon_description TEXT,
    addon_author TEXT,
    addon_version INTEGER,
    steamid64 INTEGER,
    timestamp INTEGER,
    UNIQUE(game_id, mount_path)
);
CREATE TABLE archive_entries(
    archive_id INTEGER NOT NULL REFERENCES archives(id),
    path TEXT NOT NULL,
    parent TEXT NOT NULL,
    name TEXT NOT NULL,
    kind TEXT NOT NULL CHECK(kind IN ('dir','file')),
    offset INTEGER,
    chunk INTEGER,
    length INTEGER,
    size INTEGER,
    method INTEGER,
    preload_offset INTEGER NOT NULL DEFAULT 0,
    preload_length INTEGER NOT NULL DEFAULT 0,
    assettype TEXT,
    PRIMARY KEY(archive_id, path),
    CHECK((kind = 'dir' AND offset IS NULL AND assettype IS NULL) OR
          (kind = 'file' AND offset IS NOT NULL AND assettype IS NOT NULL))
) WITHOUT ROWID;
CREATE TABLE loose_entries(
    root TEXT NOT NULL,
    key TEXT NOT NULL,
    path TEXT NOT NULL,
    parent_key TEXT NOT NULL,
    name TEXT NOT NULL,
    kind TEXT NOT NULL CHECK(kind IN ('dir','file')),
    size INTEGER NOT NULL DEFAULT 0,
    modified INTEGER NOT NULL DEFAULT 0,
    assettype TEXT,
    PRIMARY KEY(root, key),
    CHECK((kind = 'dir' AND assettype IS NULL) OR
          (kind = 'file' AND assettype IS NOT NULL))
) WITHOUT ROWID;
CREATE INDEX archive_entries_dir ON archive_entries(archive_id, parent, name);
CREATE INDEX loose_entries_dir ON loose_entries(root, parent_key, name);
CREATE INDEX archives_game ON archives(game_id);
CREATE INDEX archives_type ON archives(archive_type_id);
CREATE INDEX game_search_paths_game ON game_search_paths(game_id, ordinal);
CREATE INDEX game_mounts_game ON game_mounts(game_id, ordinal);
)sql";

inline constexpr std::string_view kInsertGame = R"sql(
INSERT INTO games(slug, title, game_path, gameinfo, garrys_mod) VALUES(?, ?, ?, ?, ?)
)sql";

inline constexpr std::string_view kInsertSearchPath = R"sql(
INSERT INTO game_search_paths(game_id, ordinal, types, raw, resolved, kind)
VALUES(?, ?, ?, ?, ?, ?)
)sql";

inline constexpr std::string_view kInsertMount = R"sql(
INSERT INTO game_mounts(game_id, ordinal, name, path, source) VALUES(?, ?, ?, ?, ?)
)sql";

inline constexpr std::string_view kInsertArchive = R"sql(
INSERT INTO archives(
    game_id, path, mount_path, name, stem, archive_type_id, hash, size, modified,
    addon_name, addon_description, addon_author, addon_version, steamid64, timestamp
) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
)sql";

inline constexpr std::string_view kInsertEntry = R"sql(
INSERT INTO archive_entries(
    archive_id, path, parent, name, kind, offset, chunk, length, size, method,
    preload_offset, preload_length, assettype
) VALUES(?, ?, ?, ?, 'file', ?, ?, ?, ?, ?, ?, ?, ?)
)sql";

inline constexpr std::string_view kInsertDirectory = R"sql(
INSERT OR IGNORE INTO archive_entries(archive_id, path, parent, name, kind)
VALUES(?, ?, ?, ?, 'dir')
)sql";

inline constexpr std::string_view kUpsertLooseFile = R"sql(
INSERT INTO loose_entries(root, key, path, parent_key, name, kind, size, modified, assettype)
VALUES(?, ?, ?, ?, ?, 'file', ?, ?, ?)
ON CONFLICT(root, key) DO UPDATE SET
    path = excluded.path,
    parent_key = excluded.parent_key,
    name = excluded.name,
    kind = 'file',
    size = excluded.size,
    modified = excluded.modified,
    assettype = excluded.assettype
)sql";

inline constexpr std::string_view kInsertLooseDirectory = R"sql(
INSERT OR IGNORE INTO loose_entries(root, key, path, parent_key, name, kind)
VALUES(?, ?, ?, ?, ?, 'dir')
)sql";

inline constexpr std::string_view kDeleteLoose = R"sql(
DELETE FROM loose_entries WHERE root = ? AND key = ?
)sql";

inline constexpr std::string_view kSelectLooseRows = R"sql(
SELECT root, key, path, parent_key, name, kind, size, modified, assettype
FROM loose_entries
)sql";

inline constexpr std::string_view kSelectSearchPaths = R"sql(
SELECT game_id, types, raw, resolved, kind FROM game_search_paths
ORDER BY game_id, ordinal
)sql";

inline constexpr std::string_view kSelectMounts = R"sql(
SELECT game_id, name, path, source FROM game_mounts ORDER BY game_id, ordinal
)sql";

inline constexpr std::string_view kDeleteSearchPaths =
    R"sql(DELETE FROM game_search_paths WHERE game_id = ?)sql";

inline constexpr std::string_view kDeleteMounts =
    R"sql(DELETE FROM game_mounts WHERE game_id = ?)sql";

inline constexpr std::string_view kSelectArchives = R"sql(
SELECT id, game_id, path, mount_path, stem, archive_type_id, hash, size, modified,
       addon_name, addon_description, addon_author, addon_version, steamid64, timestamp
FROM archives
)sql";

inline constexpr std::string_view kSelectGames =
    R"sql(SELECT id, slug, title, game_path, gameinfo, garrys_mod FROM games ORDER BY slug)sql";

inline constexpr std::string_view kSelectArchiveSummaries = R"sql(
SELECT a.id, a.game_id, a.path, a.mount_path, a.stem, t.name, a.hash,
       a.addon_name, a.addon_description, a.addon_author, a.addon_version,
       a.steamid64, a.timestamp
FROM archives a
JOIN archive_type t ON t.id = a.archive_type_id
ORDER BY a.game_id, a.mount_path
)sql";

inline constexpr std::string_view kSelectEntryTotals = R"sql(
SELECT
    (SELECT COUNT(*) FROM archive_entries WHERE kind = 'file') +
    (SELECT COUNT(*) FROM loose_entries WHERE kind = 'file'),
    (SELECT COALESCE(SUM(size), 0) FROM archive_entries WHERE kind = 'file') +
    (SELECT COALESCE(SUM(size), 0) FROM loose_entries WHERE kind = 'file')
)sql";

inline constexpr std::string_view kSelectEntry = R"sql(
SELECT kind, size, offset, chunk, length, method, preload_offset, preload_length
FROM archive_entries
WHERE archive_id=? AND path=?
)sql";

inline constexpr std::string_view kSelectDirectory = R"sql(
SELECT name, kind, size FROM archive_entries
WHERE archive_id=? AND parent=? ORDER BY name
)sql";

inline constexpr std::string_view kSelectLooseEntry = R"sql(
SELECT kind, size, modified, path FROM loose_entries WHERE root=? AND key=?
)sql";

inline constexpr std::string_view kSelectLooseDirectory = R"sql(
SELECT name, kind, size FROM loose_entries
WHERE root=? AND parent_key=? ORDER BY name
)sql";

inline constexpr std::string_view kFindGame = R"sql(SELECT id FROM games WHERE game_path = ?)sql";

inline constexpr std::string_view kUpdateGame = R"sql(
UPDATE games SET slug = ?, title = ?, gameinfo = ?, garrys_mod = ? WHERE id = ?
)sql";

inline constexpr std::string_view kDeleteEntries =
    R"sql(DELETE FROM archive_entries WHERE archive_id = ?)sql";

inline constexpr std::string_view kDeleteArchive = R"sql(DELETE FROM archives WHERE id = ?)sql";

inline constexpr std::string_view kStageArchiveMount =
    R"sql(UPDATE archives SET mount_path = ? WHERE id = ?)sql";

inline constexpr std::string_view kUpdateArchive = R"sql(
UPDATE archives SET game_id = ?, mount_path = ?, stem = ?, archive_type_id = ?, hash = ?,
    size = ?, modified = ?, addon_name = ?, addon_description = ?, addon_author = ?,
    addon_version = ?, steamid64 = ?, timestamp = ?
WHERE id = ?
)sql";

} // namespace sauce::fs::core::sql
