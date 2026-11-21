// SPDX-License-Identifier: MIT

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "index/archive_access.hpp"
#include "index/archive_index.hpp"
#include "index/sqlite_util.hpp"
#include "util/format.hpp"

namespace fs = std::filesystem;

namespace
{

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8)
    {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
}

void append_u64(std::vector<std::uint8_t>& bytes, std::uint64_t value)
{
    for (int shift = 0; shift < 64; shift += 8)
    {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
}

void append_string(std::vector<std::uint8_t>& bytes, const std::string& text)
{
    bytes.insert(bytes.end(), text.begin(), text.end());
    bytes.push_back(0);
}

/// Minimal GMAD v3 archive with one file. `required_content` is written as a
/// list of strings terminated by a NUL, as the real header does.
std::vector<std::uint8_t> make_gma(const std::string& name, const std::string& description,
                                   const std::string& author, const std::string& file_path,
                                   const std::string& contents,
                                   const std::vector<std::string>& required_content = {})
{
    std::vector<std::uint8_t> file;
    file.push_back('G');
    file.push_back('M');
    file.push_back('A');
    file.push_back('D');
    file.push_back(3);
    append_u64(file, 76561198000000000ull);
    append_u64(file, 1700000000ull);
    for (const std::string& required : required_content)
    {
        append_string(file, required);
    }
    file.push_back(0);
    append_string(file, name);
    append_string(file, description);
    append_string(file, author);
    append_u32(file, 7);

    append_u32(file, 1);
    append_string(file, file_path);
    append_u64(file, static_cast<std::uint64_t>(contents.size()));
    append_u32(file, 0);
    append_u32(file, 0);
    file.insert(file.end(), contents.begin(), contents.end());
    append_u32(file, 0);
    return file;
}

bool write_file(const fs::path& path, const std::vector<std::uint8_t>& bytes)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream)
    {
        return false;
    }
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(stream);
}

int fail(const char* message)
{
    sauce::fs::print_error("gma metadata test: {}\n", message);
    return 1;
}

const sauce::fs::IndexDatabase::ArchiveInfo*
find_archive(const std::vector<sauce::fs::IndexDatabase::ArchiveInfo>& archives,
             const std::string& name)
{
    for (const auto& archive : archives)
    {
        if (archive.addon_name == name)
        {
            return &archive;
        }
    }
    return nullptr;
}

} // namespace

int main()
{
    const fs::path root =
        fs::temp_directory_path() /
        ("sauce-fs-gma-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path game = root / "game";
    const fs::path addons = game / "garrysmod" / "addons";
    const fs::path database = root / "index.db";

    std::error_code ec;
    fs::create_directories(addons, ec);
    const std::string description =
        R"({"description":"A test addon","type":"tool","tags":["fun","build"]})";
    if (ec ||
        !write_file(addons / "test.gma", make_gma("Test Addon", description, "Author Name",
                                                  "lua/autorun/test.lua", "print(1)\n")) ||
        !write_file(addons / "required.gma", make_gma("Required Content", "{}", "",
                                                      "materials/req.vmt", "req", {"hl2", "ep2"})))
    {
        return fail("cannot create fixture");
    }

    sauce::fs::ArchiveIndexOptions options;
    options.database = database;
    options.game_folders.push_back(game);

    sauce::fs::ArchiveIndexResult result;
    if (!sauce::fs::build_archive_index(options, result))
    {
        return fail(result.error.empty() ? "index build failed" : result.error.c_str());
    }

    std::string database_error;
    sauce::fs::sqlite::Database raw =
        sauce::fs::sqlite::open(database, sauce::fs::sqlite::OpenMode::ReadOnly, database_error);
    if (!raw)
    {
        return fail(database_error.empty() ? "cannot open the generated database"
                                           : database_error.c_str());
    }
    sauce::fs::sqlite::Statement statement = sauce::fs::sqlite::prepare(
        raw.get(),
        "SELECT COUNT(*) FROM archive_type WHERE name IN ('vpk','bsp-pak','gma','gma-bin')",
        database_error);
    if (!statement)
    {
        return fail("cannot query the archive_type lookup table");
    }
    const bool type_query =
        sqlite3_step(statement.get()) == SQLITE_ROW && sqlite3_column_int(statement.get(), 0) == 4;
    if (!type_query)
    {
        return fail("archive_type lookup table is incomplete");
    }

    auto opened = sauce::fs::IndexDatabase::open(database);
    if (!opened)
    {
        return fail("cannot open the generated database for reading");
    }
    auto archives = (*opened)->archives();
    if (!archives || archives->size() != 2)
    {
        return fail("expected two indexed GMA archives");
    }

    const auto* addon = find_archive(*archives, "Test Addon");
    const auto* required = find_archive(*archives, "Required Content");
    if (addon == nullptr || required == nullptr)
    {
        return fail("GMA addon names were not indexed");
    }
    if (addon->variant != "gma" || addon->addon_description != description ||
        addon->addon_author != "Author Name" || addon->addon_version != 7 ||
        addon->steamid64 != 76561198000000000ull || addon->timestamp != 1700000000)
    {
        return fail("GMA metadata does not match the fixture");
    }
    if (required->variant != "gma" || required->addon_author != "" ||
        required->addon_description != "{}")
    {
        return fail("GMA required-content header was not parsed");
    }

    auto provider = (*opened)->game_provider("game");
    if (!provider)
    {
        return fail("cannot build the game provider");
    }
    auto blob = (*provider)->get("lua/autorun/test.lua");
    if (!blob || std::string(reinterpret_cast<const char*>((*blob)->data()), (*blob)->size()) !=
                     "print(1)\n")
    {
        return fail("GMA entries are not readable through the index");
    }

    fs::remove_all(root, ec);
    return 0;
}
