// SPDX-License-Identifier: MIT

#include "index/archive_index.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sqlite3.h>
#include <xxhash.h>

#include "index/thread_pool.hpp"
#include "parsers/bsp.hpp"
#include "parsers/bytes.hpp"
#include "parsers/gma.hpp"
#include "parsers/steam.hpp"
#include "parsers/vpk.hpp"

namespace sauce::fs
{

namespace
{

namespace fs = std::filesystem;

constexpr std::size_t kReadChunk = 1u << 20;
constexpr std::size_t kPrintedWarnings = 20;

struct GameFolder
{
    fs::path path;
    std::string title;
    std::string slug;
    bool garrys_mod = false;
};

struct ScanJob
{
    fs::path root;
    /// Content root the mount paths are relative to.
    fs::path game_path;
    bool gmod = false;
    std::vector<fs::path> extra_roots;
};

struct Candidate
{
    fs::path path;
    std::string game_path;
    std::string mount_path;
    ArchiveVariant variant = ArchiveVariant::Vpk;
};

struct JobResult
{
    std::vector<Candidate> candidates;
    std::vector<std::string> warnings;
};

fs::path absolute_normalized(const fs::path& path)
{
    std::error_code ec;
    fs::path absolute = fs::absolute(path, ec);
    if (ec)
    {
        absolute = path;
    }
    return absolute.lexically_normal();
}

/// Lowercase ASCII slug used as the mount directory name under `sourcegames`.
/// Apostrophes are dropped so "Garry's Mod" becomes "garrys-mod".
std::string slugify(std::string_view text)
{
    std::string slug;
    slug.reserve(text.size());
    bool separator = false;
    for (const char character : text)
    {
        if (character >= 'a' && character <= 'z')
        {
            if (separator && !slug.empty())
            {
                slug.push_back('-');
            }
            separator = false;
            slug.push_back(character);
        }
        else if (character >= 'A' && character <= 'Z')
        {
            if (separator && !slug.empty())
            {
                slug.push_back('-');
            }
            separator = false;
            slug.push_back(static_cast<char>(character - 'A' + 'a'));
        }
        else if (character >= '0' && character <= '9')
        {
            if (separator && !slug.empty())
            {
                slug.push_back('-');
            }
            separator = false;
            slug.push_back(character);
        }
        else if (character != '\'')
        {
            separator = true;
        }
    }
    return slug.empty() ? "game" : slug;
}

std::string unique_slug(std::string base, std::set<std::string>& used)
{
    if (used.insert(base).second)
    {
        return base;
    }
    for (std::size_t suffix = 2;; ++suffix)
    {
        std::string candidate = base + '-' + std::to_string(suffix);
        if (used.insert(candidate).second)
        {
            return candidate;
        }
    }
}

bool is_gmod_folder(const fs::path& folder)
{
    if (parsers::ascii_iequals(folder.filename().string(), "garrysmod"))
    {
        return true;
    }
    std::error_code ec;
    return fs::is_directory(folder / "garrysmod", ec);
}

/// The game directory that owns Garry's Mod addons. Accepts either the game
/// directory itself or an install root containing `garrysmod`.
fs::path gmod_content_folder(const fs::path& folder)
{
    std::error_code ec;
    const fs::path nested = folder / "garrysmod";
    if (fs::is_directory(nested, ec))
    {
        return nested;
    }
    return folder;
}

bool contains_ci(std::string_view text, std::string_view needle)
{
    if (needle.empty())
    {
        return true;
    }
    for (std::size_t index = 0; index + needle.size() <= text.size(); ++index)
    {
        if (parsers::ascii_iequals(text.substr(index, needle.size()), needle))
        {
            return true;
        }
    }
    return false;
}

/// Walk up from a game directory until a Steam library's workshop content for
/// Garry's Mod (appid 4000) is found.
std::vector<fs::path> workshop_roots_for(const fs::path& game_dir)
{
    std::vector<fs::path> roots;
    std::error_code ec;
    for (fs::path ancestor = game_dir; !ancestor.empty(); ancestor = ancestor.parent_path())
    {
        const fs::path candidate = ancestor / "steamapps" / "workshop" / "content" / "4000";
        if (fs::is_directory(candidate, ec))
        {
            roots.push_back(candidate);
            break;
        }
        if (ancestor == ancestor.root_path())
        {
            break;
        }
    }
    return roots;
}

std::vector<GameFolder> collect_game_folders(const ArchiveIndexOptions& options,
                                             std::vector<std::string>& warnings)
{
    std::vector<GameFolder> folders;
    if (!options.game_folders.empty())
    {
        for (const fs::path& folder : options.game_folders)
        {
            if (folder.empty())
            {
                warnings.push_back("game folder path is empty");
                continue;
            }
            const fs::path path = absolute_normalized(folder);
            std::error_code ec;
            if (!fs::is_directory(path, ec))
            {
                warnings.push_back("game folder does not exist: " + path.string());
                continue;
            }
            std::string title = path.filename().string();
            if (title.empty())
            {
                title = path.generic_string();
            }
            folders.push_back({path, std::move(title), {}, is_gmod_folder(path)});
        }
    }
    else
    {
        parsers::DiscoveryOptions discovery;
        discovery.steam_roots = options.steam_roots;
        discovery.include_dedicated_servers = false;
        std::vector<std::string> discovery_warnings;
        const std::vector<parsers::InstalledTitle> titles =
            parsers::discover_source_titles(discovery, &discovery_warnings);
        warnings.insert(warnings.end(), discovery_warnings.begin(), discovery_warnings.end());

        for (const parsers::InstalledTitle& title : titles)
        {
            if (title.game_dir.empty())
            {
                continue;
            }
            const fs::path path = absolute_normalized(title.game_dir);
            const bool gmod = is_gmod_folder(path) || contains_ci(title.name, "garry");
            std::string display = title.name;
            if (display.empty())
            {
                display = path.filename().string();
            }
            folders.push_back({path, std::move(display), {}, gmod});
        }
    }

    std::vector<GameFolder> unique;
    std::set<std::string> seen;
    for (GameFolder& folder : folders)
    {
        const std::string key = folder.path.generic_string();
        if (!seen.insert(key).second)
        {
            continue;
        }
        unique.push_back(std::move(folder));
    }

    // Stable order also makes the generated slug suffixes reproducible.
    std::sort(unique.begin(), unique.end(), [](const GameFolder& left, const GameFolder& right)
              { return left.path.generic_string() < right.path.generic_string(); });
    std::set<std::string> slugs;
    for (GameFolder& folder : unique)
    {
        folder.slug = unique_slug(slugify(folder.title), slugs);
    }
    return unique;
}

bool compute_hash(const fs::path& path, std::uint64_t& hash, std::string& error)
{
    parsers::FileHandle file;
    if (!file.open(path, &error))
    {
        return false;
    }

    XXH3_state_t* state = XXH3_createState();
    if (state == nullptr)
    {
        error = "cannot allocate hash state for " + path.string();
        return false;
    }
    struct StateGuard
    {
        XXH3_state_t* state;

        ~StateGuard()
        {
            XXH3_freeState(state);
        }
    } state_guard{state};

    XXH3_64bits_reset(state);

    // One reusable buffer per worker thread: hashing is dispatched as many
    // small jobs, so per-file allocation would otherwise dominate.
    thread_local std::vector<std::uint8_t> buffer(kReadChunk);
    std::uint64_t remaining = file.size();
    while (remaining > 0)
    {
        const std::size_t chunk =
            static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        if (!file.read(buffer.data(), chunk))
        {
            error = "cannot read " + path.string();
            return false;
        }
        XXH3_64bits_update(state, buffer.data(), chunk);
        remaining -= chunk;
    }
    hash = XXH3_64bits_digest(state);
    return true;
}

std::string hash_hex(std::uint64_t hash)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string text(16, '0');
    for (std::size_t index = 0; index < text.size(); ++index)
    {
        text[text.size() - 1 - index] = digits[hash & 0xFu];
        hash >>= 4;
    }
    return text;
}

bool has_extension(std::string_view path, std::initializer_list<std::string_view> extensions)
{
    for (const std::string_view extension : extensions)
    {
        if (parsers::ends_with_ci(path, extension))
        {
            return true;
        }
    }
    return false;
}

/// Asset type is a coarse classification of the path's extension.
AssetType classify_asset_type(std::string_view path)
{
    if (has_extension(path, {".mdl", ".vvd", ".vtx", ".phy", ".ani", ".vta"}))
    {
        return AssetType::Model;
    }
    if (has_extension(path, {".vmt"}))
    {
        return AssetType::Material;
    }
    if (has_extension(path, {".vtf", ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".dds"}))
    {
        return AssetType::Texture;
    }
    if (has_extension(path, {".wav", ".mp3", ".ogg", ".flac"}))
    {
        return AssetType::Sound;
    }
    if (has_extension(path, {".bsp", ".vmf"}))
    {
        return AssetType::Map;
    }
    if (has_extension(path, {".lua", ".cfg", ".txt", ".vbs", ".js", ".nut", ".sq"}))
    {
        return AssetType::Script;
    }
    return AssetType::Unknown;
}

void append_entry_paths(std::string_view archive_path, ArchiveVariant variant,
                        std::vector<parsers::ScannedEntry> entries,
                        std::vector<ArchiveContent>& contents)
{
    // Case-normalized paths can collide inside one archive (VPK trees are not
    // guaranteed unique). The table keys on (archive, path), so collapse them
    // before deriving directories.
    std::sort(entries.begin(), entries.end(),
              [](const parsers::ScannedEntry& left, const parsers::ScannedEntry& right)
              { return left.path < right.path; });
    entries.erase(
        std::unique(entries.begin(), entries.end(),
                    [](const parsers::ScannedEntry& left, const parsers::ScannedEntry& right)
                    { return left.path == right.path; }),
        entries.end());

    std::set<std::string> directories;
    contents.reserve(contents.size() + entries.size() + 64);
    for (parsers::ScannedEntry& entry : entries)
    {
        if (entry.path.empty())
        {
            continue;
        }

        // Archive listings contain files only. Materialize every ancestor so
        // readdir() can use an equality lookup even for directories that hold
        // only subdirectories.
        for (std::size_t slash = entry.path.find('/'); slash != std::string::npos;
             slash = entry.path.find('/', slash + 1))
        {
            directories.insert(entry.path.substr(0, slash));
        }

        ArchiveContent content;
        content.archive_path = archive_path;
        content.path = std::move(entry.path);
        content.offset = entry.offset;
        content.chunk =
            variant == ArchiveVariant::Vpk ? static_cast<std::int32_t>(entry.archive) : -1;
        content.length = entry.length;
        content.size = entry.size;
        content.method = entry.method;
        content.preload_offset = entry.preload_offset;
        content.preload_length = entry.preload_length;
        content.asset_type = classify_asset_type(content.path);
        contents.push_back(std::move(content));
    }

    for (const std::string& directory : directories)
    {
        ArchiveContent content;
        content.archive_path = archive_path;
        content.path = directory;
        content.directory = true;
        contents.push_back(std::move(content));
    }
}

/// Parse only the container's directory/index; entry payloads are never read.
void scan_contents(const fs::path& path, std::string_view archive_path, ArchiveVariant variant,
                   std::vector<ArchiveContent>& contents, std::vector<std::string>& warnings)
{
    std::vector<parsers::ScannedEntry> entries;
    std::string error;
    std::vector<std::string> parse_warnings;
    bool ok = false;
    switch (variant)
    {
    case ArchiveVariant::Vpk:
        ok = parsers::parse_vpk(path, entries, &error, parse_warnings);
        break;
    case ArchiveVariant::Bsp:
    {
        std::uint64_t pak_offset = 0;
        std::uint64_t pak_length = 0;
        ok = parsers::parse_bsp_pakfile(path, pak_offset, pak_length, entries, &error,
                                        parse_warnings);
        if (ok)
        {
            // Parser offsets are pakfile-relative; store them relative to the
            // BSP itself so offset..offset+length is a real file range.
            for (parsers::ScannedEntry& entry : entries)
            {
                entry.offset += pak_offset;
            }
        }
        break;
    }
    case ArchiveVariant::Gma:
        ok = parsers::parse_gma(path, entries, &error);
        break;
    case ArchiveVariant::GmaBin:
    {
        std::vector<std::uint8_t> decoded;
        ok = parsers::decode_legacy_bin(path, decoded, &error) &&
             parsers::parse_gma_buffer(decoded.data(), decoded.size(), entries, &error);
        break;
    }
    }

    warnings.insert(warnings.end(), std::make_move_iterator(parse_warnings.begin()),
                    std::make_move_iterator(parse_warnings.end()));
    if (!ok)
    {
        warnings.push_back(error.empty() ? "cannot parse " + path.string() : error);
        return;
    }
    append_entry_paths(archive_path, variant, std::move(entries), contents);
}

/// Game-relative mount directory for one archive: the on-disk path with the
/// archive extension replaced by a sibling directory, e.g. `maps/foo.bsp` ->
/// `maps/foo`, `hl2_misc_dir.vpk` -> `hl2_misc`, `addons/foo.gma` ->
/// `addons/foo`. Keeping the parent path avoids collisions between archives
/// with the same stem in different folders.
std::string make_mount_path(const fs::path& path, const fs::path& root, const std::string& prefix,
                            ArchiveVariant variant)
{
    fs::path relative = path.lexically_relative(root);
    if (relative.empty() || relative.begin()->string() == "..")
    {
        relative = path.filename();
    }

    fs::path mount = fs::path(prefix) / relative;
    mount = mount.parent_path() / mount.stem();
    if (variant == ArchiveVariant::Vpk)
    {
        const std::string name = mount.filename().string();
        if (parsers::ends_with_ci(name, "_dir") && name.size() > 4)
        {
            mount = mount.parent_path() / name.substr(0, name.size() - 4);
        }
    }
    return mount.generic_string();
}

/// Prefix of `root` below `game_path`, or empty when they are the same or
/// unrelated.
std::string relative_mount_prefix(const fs::path& root, const fs::path& game_path)
{
    if (root == game_path)
    {
        return {};
    }
    const fs::path relative = root.lexically_relative(game_path);
    if (relative.empty() || relative == "." || relative.begin()->string() == "..")
    {
        return {};
    }
    return relative.generic_string();
}

/// Mount prefix for a Workshop content root:
/// `.../steamapps/workshop/content/4000` -> `workshop/content/4000`.
std::string workshop_mount_prefix(const fs::path& root)
{
    std::string prefix;
    bool in_workshop = false;
    for (const fs::path& part : root)
    {
        if (part == "workshop")
        {
            in_workshop = true;
        }
        if (in_workshop)
        {
            if (!prefix.empty())
            {
                prefix.push_back('/');
            }
            prefix += part.string();
        }
    }
    return prefix.empty() ? root.filename().string() : prefix;
}

void add_candidate(const fs::path& path, ArchiveVariant variant, const fs::path& game_path,
                   const fs::path& mount_root, const std::string& mount_prefix, JobResult& result)
{
    Candidate candidate;
    candidate.path = absolute_normalized(path);
    candidate.variant = variant;
    candidate.game_path = game_path.generic_string();
    candidate.mount_path = make_mount_path(candidate.path, mount_root, mount_prefix, variant);
    result.candidates.push_back(std::move(candidate));
}

bool is_vpk_directory_file(const fs::path& path, const std::string& name)
{
    if (!parsers::ends_with_ci(name, ".vpk") || parsers::is_vpk_chunk_name(name))
    {
        return false;
    }
    if (parsers::ends_with_ci(name, "_dir.vpk"))
    {
        return true;
    }

    // foo.vpk is the directory file only when foo_dir.vpk does not exist.
    const fs::path directory_file = path.parent_path() / (path.stem().string() + "_dir.vpk");
    std::error_code ec;
    return !fs::is_regular_file(directory_file, ec);
}

void scan_root(const fs::path& root, const fs::path& game_path, const std::string& mount_prefix,
               bool gmod, JobResult& result)
{
    using directory_options = fs::directory_options;
    std::error_code ec;
    fs::recursive_directory_iterator iterator(root, directory_options::skip_permission_denied, ec);
    if (ec)
    {
        result.warnings.push_back("cannot walk " + root.string() + ": " + ec.message());
        return;
    }

    const fs::recursive_directory_iterator end;
    for (; iterator != end; iterator.increment(ec))
    {
        if (ec)
        {
            result.warnings.push_back("directory walk stopped in " + root.string() + ": " +
                                      ec.message());
            break;
        }

        const fs::path path = iterator->path();
        const std::string name = path.filename().string();
        if (!name.empty() && name[0] == '.')
        {
            std::error_code type_error;
            if (iterator->is_directory(type_error))
            {
                iterator.disable_recursion_pending();
            }
            continue;
        }

        std::error_code type_error;
        if (!iterator->is_regular_file(type_error) || type_error)
        {
            continue;
        }

        if (gmod)
        {
            if (parsers::ends_with_ci(name, ".gma"))
            {
                add_candidate(path, ArchiveVariant::Gma, game_path, root, mount_prefix, result);
            }
            else if (parsers::is_legacy_bin_name(name))
            {
                add_candidate(path, ArchiveVariant::GmaBin, game_path, root, mount_prefix, result);
            }
            continue;
        }

        if (is_vpk_directory_file(path, name))
        {
            add_candidate(path, ArchiveVariant::Vpk, game_path, root, mount_prefix, result);
        }
        else if (parsers::ends_with_ci(name, ".bsp"))
        {
            add_candidate(path, ArchiveVariant::Bsp, game_path, root, mount_prefix, result);
        }
    }
}

void scan_job(const ScanJob& job, JobResult& result)
{
    scan_root(job.root, job.game_path, relative_mount_prefix(job.root, job.game_path), job.gmod,
              result);
    for (const fs::path& extra : job.extra_roots)
    {
        if (extra == job.root)
        {
            continue;
        }
        scan_root(extra, job.game_path, workshop_mount_prefix(extra), job.gmod, result);
    }
}

bool write_database(const fs::path& database, const std::vector<GameRecord>& games,
                    const std::vector<ArchiveRecord>& archives,
                    const std::vector<ArchiveContent>& contents, std::string& error)
{
    sqlite3* handle = nullptr;
    const std::u8string utf8 = database.u8string();
    const int open_result = sqlite3_open_v2(reinterpret_cast<const char*>(utf8.c_str()), &handle,
                                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (open_result != SQLITE_OK)
    {
        error = handle != nullptr ? sqlite3_errmsg(handle) : "cannot open SQLite database";
        sqlite3_close(handle);
        return false;
    }
    struct DatabaseGuard
    {
        sqlite3* handle;

        ~DatabaseGuard()
        {
            sqlite3_close(handle);
        }
    } database_guard{handle};

    // Enforced per connection and only outside a transaction.
    if (sqlite3_exec(handle, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr) != SQLITE_OK)
    {
        error = sqlite3_errmsg(handle);
        return false;
    }

    auto rollback = [handle] { sqlite3_exec(handle, "ROLLBACK;", nullptr, nullptr, nullptr); };

    char* message = nullptr;
    // `archive_entries` is the mount namespace. Directories are rows of their
    // own so readdir() is an equality search on (archive_id, parent, name);
    // `path` is denormalized for O(1) lookup and subtree prefix scans. There is
    // no inode table: the tree is read-only and archives cannot express hard
    // links or renames.
    const char* schema =
        "BEGIN;"
        "DROP TABLE IF EXISTS archive_entries;"
        "DROP TABLE IF EXISTS archives;"
        "DROP TABLE IF EXISTS games;"
        "CREATE TABLE games("
        "id INTEGER PRIMARY KEY,"
        "slug TEXT NOT NULL UNIQUE,"
        "title TEXT NOT NULL,"
        "game_path TEXT NOT NULL UNIQUE);"
        "CREATE TABLE archives("
        "id INTEGER PRIMARY KEY,"
        "game_id INTEGER NOT NULL REFERENCES games(id),"
        "path TEXT NOT NULL UNIQUE,"
        "mount_path TEXT NOT NULL,"
        "name TEXT NOT NULL,"
        "stem TEXT NOT NULL,"
        "variant TEXT NOT NULL,"
        "hash TEXT NOT NULL,"
        "UNIQUE(game_id, mount_path));"
        "CREATE TABLE archive_entries("
        "archive_id INTEGER NOT NULL REFERENCES archives(id),"
        "path TEXT NOT NULL,"
        "parent TEXT NOT NULL,"
        "name TEXT NOT NULL,"
        "kind TEXT NOT NULL CHECK(kind IN ('dir','file')),"
        "offset INTEGER,"
        "chunk INTEGER,"
        "length INTEGER,"
        "size INTEGER,"
        "method INTEGER,"
        "preload_offset INTEGER NOT NULL DEFAULT 0,"
        "preload_length INTEGER NOT NULL DEFAULT 0,"
        "assettype TEXT,"
        "PRIMARY KEY(archive_id, path),"
        "CHECK((kind = 'dir' AND offset IS NULL AND assettype IS NULL) OR "
        "(kind = 'file' AND offset IS NOT NULL AND assettype IS NOT NULL)))"
        " WITHOUT ROWID;"
        "CREATE INDEX archive_entries_dir ON archive_entries(archive_id, parent, name);"
        "CREATE INDEX archives_game ON archives(game_id);";
    if (sqlite3_exec(handle, schema, nullptr, nullptr, &message) != SQLITE_OK)
    {
        error = message != nullptr ? message : sqlite3_errmsg(handle);
        sqlite3_free(message);
        rollback();
        return false;
    }

    struct StatementGuard
    {
        sqlite3_stmt* game = nullptr;
        sqlite3_stmt* archive = nullptr;
        sqlite3_stmt* file = nullptr;
        sqlite3_stmt* directory = nullptr;

        ~StatementGuard()
        {
            sqlite3_finalize(game);
            sqlite3_finalize(archive);
            sqlite3_finalize(file);
            sqlite3_finalize(directory);
        }
    } statements;

    auto prepare = [&](sqlite3_stmt*& statement, const char* sql)
    {
        if (sqlite3_prepare_v2(handle, sql, -1, &statement, nullptr) != SQLITE_OK)
        {
            error = sqlite3_errmsg(handle);
            return false;
        }
        return true;
    };

    if (!prepare(statements.game, "INSERT INTO games(slug, title, game_path) VALUES(?, ?, ?)") ||
        !prepare(statements.archive,
                 "INSERT INTO archives(game_id, path, mount_path, name, stem, variant, hash)"
                 " VALUES(?, ?, ?, ?, ?, ?, ?)") ||
        !prepare(statements.file,
                 "INSERT INTO archive_entries("
                 "archive_id, path, parent, name, kind, offset, chunk, length, size, method,"
                 " preload_offset, preload_length, assettype)"
                 " VALUES(?, ?, ?, ?, 'file', ?, ?, ?, ?, ?, ?, ?, ?)") ||
        !prepare(statements.directory,
                 "INSERT OR IGNORE INTO archive_entries("
                 "archive_id, path, parent, name, kind) VALUES(?, ?, ?, ?, 'dir')"))
    {
        rollback();
        return false;
    }

    auto step = [&](sqlite3_stmt* statement)
    {
        if (sqlite3_step(statement) != SQLITE_DONE)
        {
            error = sqlite3_errmsg(handle);
            rollback();
            return false;
        }
        sqlite3_reset(statement);
        sqlite3_clear_bindings(statement);
        return true;
    };

    auto bind_text = [](sqlite3_stmt* statement, int index, std::string_view value)
    {
        sqlite3_bind_text(statement, index, value.empty() ? "" : value.data(),
                          static_cast<int>(value.size()), SQLITE_TRANSIENT);
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
    for (const GameRecord& game : games)
    {
        bind_text(statements.game, 1, game.slug);
        bind_text(statements.game, 2, game.title);
        bind_text(statements.game, 3, game.game_path);
        if (!step(statements.game))
        {
            return false;
        }
        game_ids.emplace(game.game_path, sqlite3_last_insert_rowid(handle));
    }

    std::unordered_map<std::string, sqlite3_int64> archive_ids;
    archive_ids.reserve(archives.size());
    for (const ArchiveRecord& record : archives)
    {
        const auto game = game_ids.find(record.game_path);
        if (game == game_ids.end())
        {
            error = "archive has unknown game: " + record.game_path;
            rollback();
            return false;
        }
        const std::string name = fs::path(record.path).filename().string();
        const std::string hash = hash_hex(record.hash);
        sqlite3_bind_int64(statements.archive, 1, game->second);
        bind_text(statements.archive, 2, record.path);
        bind_text(statements.archive, 3, record.mount_path);
        bind_text(statements.archive, 4, name);
        bind_text(statements.archive, 5, record.stem);
        bind_text(statements.archive, 6, archive_variant_name(record.variant));
        bind_text(statements.archive, 7, hash);
        if (!step(statements.archive))
        {
            return false;
        }
        archive_ids.emplace(record.path, sqlite3_last_insert_rowid(handle));
    }

    auto archive_id_for = [&](const std::string& path, sqlite3_int64& id)
    {
        const auto found = archive_ids.find(path);
        if (found == archive_ids.end())
        {
            error = "entry references unknown archive: " + path;
            rollback();
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
        sqlite3_bind_int64(statements.file, 1, archive_id);
        bind_text(statements.file, 2, content.path);
        bind_text(statements.file, 3, parent);
        bind_text(statements.file, 4, name);
        sqlite3_bind_int64(statements.file, 5, static_cast<sqlite3_int64>(content.offset));
        if (content.chunk < 0)
        {
            sqlite3_bind_null(statements.file, 6);
        }
        else
        {
            sqlite3_bind_int(statements.file, 6, content.chunk);
        }
        sqlite3_bind_int64(statements.file, 7, static_cast<sqlite3_int64>(content.length));
        sqlite3_bind_int64(statements.file, 8, static_cast<sqlite3_int64>(content.size));
        sqlite3_bind_int(statements.file, 9, static_cast<int>(content.method));
        sqlite3_bind_int64(statements.file, 10, static_cast<sqlite3_int64>(content.preload_offset));
        sqlite3_bind_int(statements.file, 11, static_cast<int>(content.preload_length));
        bind_text(statements.file, 12, asset_type_name(content.asset_type));
        if (!step(statements.file))
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
        sqlite3_bind_int64(statements.directory, 1, archive_id);
        bind_text(statements.directory, 2, content.path);
        bind_text(statements.directory, 3, parent);
        bind_text(statements.directory, 4, name);
        if (!step(statements.directory))
        {
            return false;
        }
    }

    if (sqlite3_exec(handle, "COMMIT;", nullptr, nullptr, &message) != SQLITE_OK)
    {
        error = message != nullptr ? message : sqlite3_errmsg(handle);
        sqlite3_free(message);
        rollback();
        return false;
    }
    return true;
}

} // namespace

const char* archive_variant_name(ArchiveVariant variant) noexcept
{
    switch (variant)
    {
    case ArchiveVariant::Vpk:
        return "vpk";
    case ArchiveVariant::Bsp:
        return "bsp-pak";
    case ArchiveVariant::Gma:
        return "gma";
    case ArchiveVariant::GmaBin:
        return "gma-bin";
    }
    return "unknown";
}

const char* asset_type_name(AssetType type) noexcept
{
    switch (type)
    {
    case AssetType::Unknown:
        return "Unknown";
    case AssetType::Model:
        return "Model";
    case AssetType::Material:
        return "Material";
    case AssetType::Texture:
        return "Texture";
    case AssetType::Sound:
        return "Sound";
    case AssetType::Map:
        return "Map";
    case AssetType::Script:
        return "Script";
    }
    return "Unknown";
}

bool build_archive_index(const ArchiveIndexOptions& options, ArchiveIndexResult& result)
{
    result = ArchiveIndexResult{};

    const std::vector<GameFolder> folders = collect_game_folders(options, result.summary.warnings);
    result.summary.game_folders = folders.size();
    for (const GameFolder& folder : folders)
    {
        result.games.push_back(GameRecord{folder.slug, folder.title, folder.path.generic_string()});
    }
    std::sort(result.games.begin(), result.games.end(),
              [](const GameRecord& left, const GameRecord& right)
              { return left.slug < right.slug; });

    std::vector<ScanJob> jobs;
    jobs.reserve(folders.size() + 1);
    for (const GameFolder& folder : folders)
    {
        jobs.push_back(ScanJob{folder.path, folder.path, false, {}});
    }
    for (const GameFolder& folder : folders)
    {
        if (!folder.garrys_mod)
        {
            continue;
        }
        ScanJob job;
        job.root = gmod_content_folder(folder.path);
        job.game_path = folder.path;
        job.gmod = true;
        job.extra_roots = workshop_roots_for(folder.path);
        jobs.push_back(std::move(job));
        ++result.summary.gmod_crawlers;
    }

    std::vector<JobResult> job_results(jobs.size());
    const std::size_t hash_workers = std::max<std::size_t>(1, std::thread::hardware_concurrency());
    if (!jobs.empty())
    {
        // Scanning keeps one worker per game folder (plus the Garry's Mod
        // crawler). The same pool then hashes files in parallel, one job per
        // archive, so a single large folder is not serialized.
        ThreadPool pool(std::max(jobs.size(), hash_workers));
        for (std::size_t index = 0; index < jobs.size(); ++index)
        {
            pool.submit([&jobs, &job_results, index]
                        { scan_job(jobs[index], job_results[index]); });
        }
        try
        {
            pool.wait();
        }
        catch (const std::exception& exception)
        {
            result.summary.warnings.push_back(std::string("worker failed: ") + exception.what());
        }

        std::vector<Candidate> candidates;
        for (JobResult& job_result : job_results)
        {
            candidates.insert(candidates.end(),
                              std::make_move_iterator(job_result.candidates.begin()),
                              std::make_move_iterator(job_result.candidates.end()));
            result.summary.warnings.insert(result.summary.warnings.end(),
                                           std::make_move_iterator(job_result.warnings.begin()),
                                           std::make_move_iterator(job_result.warnings.end()));
        }

        // Deduplicate before hashing: overlapping roots (an explicit game
        // folder plus the Garry's Mod crawler) must not read the same file
        // twice. Sorting first keeps which copy survives reproducible.
        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& left, const Candidate& right)
                  { return left.path.generic_string() < right.path.generic_string(); });
        std::vector<Candidate> unique;
        unique.reserve(candidates.size());
        std::set<std::string> seen;
        for (Candidate& candidate : candidates)
        {
            if (seen.insert(candidate.path.generic_string()).second)
            {
                unique.push_back(std::move(candidate));
            }
        }

        // Mount directory names must be unique per game. Two archives can
        // share a stem (`foo_dir.vpk` and `foo.bsp`), so disambiguate rather
        // than fail the UNIQUE(game_id, mount_path) constraint.
        std::set<std::string> mounts;
        for (Candidate& candidate : unique)
        {
            const std::string base = candidate.mount_path;
            std::string mount = base;
            for (std::size_t suffix = 2;; ++suffix)
            {
                if (mounts.insert(candidate.game_path + '\n' + mount).second)
                {
                    break;
                }
                mount = base + '-' + std::to_string(suffix);
            }
            candidate.mount_path = std::move(mount);
        }

        std::vector<ArchiveRecord> records(unique.size());
        std::vector<std::string> errors(unique.size());
        std::vector<std::uint8_t> hashed(unique.size(), 0);
        std::vector<std::vector<ArchiveContent>> content_lists(unique.size());
        std::vector<std::vector<std::string>> content_warnings(unique.size());
        for (std::size_t index = 0; index < unique.size(); ++index)
        {
            const Candidate& candidate = unique[index];
            ArchiveRecord& record = records[index];
            record.path = candidate.path.generic_string();
            record.game_path = candidate.game_path;
            record.mount_path = candidate.mount_path;
            record.stem = candidate.path.stem().string();
            record.variant = candidate.variant;
        }

        for (std::size_t index = 0; index < unique.size(); ++index)
        {
            pool.submit(
                [&unique, &records, &errors, &hashed, &content_lists, &content_warnings, index]
                {
                    if (!compute_hash(unique[index].path, records[index].hash, errors[index]))
                    {
                        return;
                    }
                    scan_contents(unique[index].path, records[index].path, unique[index].variant,
                                  content_lists[index], content_warnings[index]);
                    hashed[index] = 1;
                });
        }
        try
        {
            pool.wait();
        }
        catch (const std::exception& exception)
        {
            result.summary.warnings.push_back(std::string("worker failed: ") + exception.what());
        }

        for (std::size_t index = 0; index < unique.size(); ++index)
        {
            if (hashed[index] != 0)
            {
                result.archives.push_back(std::move(records[index]));
                for (const ArchiveContent& content : content_lists[index])
                {
                    if (content.directory)
                    {
                        ++result.summary.directories;
                    }
                }
                result.contents.insert(result.contents.end(),
                                       std::make_move_iterator(content_lists[index].begin()),
                                       std::make_move_iterator(content_lists[index].end()));
                result.summary.warnings.insert(
                    result.summary.warnings.end(),
                    std::make_move_iterator(content_warnings[index].begin()),
                    std::make_move_iterator(content_warnings[index].end()));
                continue;
            }
            ++result.summary.unreadable;
            if (!errors[index].empty())
            {
                result.summary.warnings.push_back(std::move(errors[index]));
            }
        }
    }

    std::sort(result.archives.begin(), result.archives.end(),
              [](const ArchiveRecord& left, const ArchiveRecord& right)
              { return left.path < right.path; });

    for (const ArchiveRecord& record : result.archives)
    {
        switch (record.variant)
        {
        case ArchiveVariant::Vpk:
            ++result.summary.vpk;
            break;
        case ArchiveVariant::Bsp:
            ++result.summary.bsp;
            break;
        case ArchiveVariant::Gma:
            ++result.summary.gma;
            break;
        case ArchiveVariant::GmaBin:
            ++result.summary.gma_bin;
            break;
        }
    }

    const bool written = write_database(options.database, result.games, result.archives,
                                        result.contents, result.error);
    print_archive_index_summary(result, options.database);
    return written;
}

void print_archive_index_summary(const ArchiveIndexResult& result,
                                 const std::filesystem::path& database)
{
    const ArchiveIndexSummary& summary = result.summary;
    std::printf("archives: %zu (vpk %zu, bsp-pak %zu, gma %zu, gma-bin %zu)\n",
                result.archives.size(), summary.vpk, summary.bsp, summary.gma, summary.gma_bin);
    std::printf("contents: %zu files, %zu directories\n",
                result.contents.size() - summary.directories, summary.directories);
    std::printf("games: %zu, gmod crawlers: %zu, unreadable: %zu\n", result.games.size(),
                summary.gmod_crawlers, summary.unreadable);
    std::printf("database: %s\n", database.string().c_str());

    if (!summary.warnings.empty())
    {
        std::printf("warnings (%zu):\n", summary.warnings.size());
        const std::size_t limit = std::min(summary.warnings.size(), kPrintedWarnings);
        for (std::size_t index = 0; index < limit; ++index)
        {
            std::printf("  %s\n", summary.warnings[index].c_str());
        }
        if (summary.warnings.size() > limit)
        {
            std::printf("  ... %zu more\n", summary.warnings.size() - limit);
        }
    }

    if (!result.error.empty())
    {
        std::printf("error: %s\n", result.error.c_str());
    }
}

} // namespace sauce::fs
