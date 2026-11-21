// SPDX-License-Identifier: MIT

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "util/format.hpp"
#include <sauce/core/archive_access.hpp>
#include <sauce/core/archive_index.hpp>

namespace fs = std::filesystem;
using namespace sauce::fs::core;

namespace
{

void append_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

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

struct VpkEntry
{
    std::string path;
    std::string contents;
};

/// Minimal VPK v1 with embedded entries.
std::vector<std::uint8_t> make_vpk(const std::vector<VpkEntry>& entries)
{
    std::map<std::string, std::map<std::string, std::vector<const VpkEntry*>>> tree;
    for (const VpkEntry& entry : entries)
    {
        const std::size_t slash = entry.path.rfind('/');
        const std::string name = entry.path.substr(slash + 1);
        const std::size_t dot = name.rfind('.');
        const std::string extension = name.substr(dot + 1);
        const std::string directory = slash == std::string::npos ? "" : entry.path.substr(0, slash);
        tree[extension][directory].push_back(&entry);
    }

    std::vector<std::uint8_t> body;
    std::vector<const VpkEntry*> ordered;
    std::uint32_t offset = 0;
    for (auto& [extension, directories] : tree)
    {
        append_string(body, extension);
        for (auto& [directory, files] : directories)
        {
            append_string(body, directory);
            for (const VpkEntry* entry : files)
            {
                const std::string name = entry->path.substr(entry->path.rfind('/') + 1);
                const std::size_t dot = name.rfind('.');
                append_string(body, name.substr(0, dot));
                append_u32(body, 0);
                append_u16(body, 0);
                append_u16(body, 0x7FFF);
                append_u32(body, offset);
                append_u32(body, static_cast<std::uint32_t>(entry->contents.size()));
                append_u16(body, 0xFFFF);
                offset += static_cast<std::uint32_t>(entry->contents.size());
                ordered.push_back(entry);
            }
            body.push_back(0);
        }
        body.push_back(0);
    }
    body.push_back(0);

    std::vector<std::uint8_t> file;
    append_u32(file, 0x55AA1234u);
    append_u32(file, 1);
    append_u32(file, static_cast<std::uint32_t>(body.size()));
    file.insert(file.end(), body.begin(), body.end());
    for (const VpkEntry* entry : ordered)
    {
        file.insert(file.end(), entry->contents.begin(), entry->contents.end());
    }
    return file;
}

/// Minimal single-entry GMAD v3 archive.
std::vector<std::uint8_t> make_gma(const std::string& file_path, const std::string& contents)
{
    std::vector<std::uint8_t> file = {'G', 'M', 'A', 'D', 3};
    append_u64(file, 0);
    append_u64(file, 0);
    file.push_back(0);
    append_string(file, "Test Addon");
    append_string(file, "{}");
    append_string(file, "");
    append_u32(file, 1);
    append_u32(file, 1);
    append_string(file, file_path);
    append_u64(file, static_cast<std::uint64_t>(contents.size()));
    append_u32(file, 0);
    append_u32(file, 0);
    file.insert(file.end(), contents.begin(), contents.end());
    append_u32(file, 0);
    return file;
}

bool write_bytes(const fs::path& path, const std::vector<std::uint8_t>& bytes)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(stream);
}

bool write_text(const fs::path& path, const std::string& text)
{
    return write_bytes(path, std::vector<std::uint8_t>(text.begin(), text.end()));
}

int fail(const std::string& message)
{
    sauce::fs::print_error("loose order test: {}\n", message);
    return 1;
}

bool expect(ProviderPtr provider, const std::string& path, const std::string& contents)
{
    auto blob = provider->get(path);
    if (!blob)
    {
        return false;
    }
    return std::string(reinterpret_cast<const char*>((*blob)->data()), (*blob)->size()) == contents;
}

} // namespace

int main()
{
    const fs::path root =
        fs::temp_directory_path() /
        ("sauce-fs-order-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;

    // Engine fixture: custom loose content beats a VPK, and the VPK beats the
    // game root, matching gameinfo SearchPaths order.
    const fs::path hl2 = root / "steamapps/common/Half-Life 2/hl2";
    const std::vector<VpkEntry> pak_entries{{"materials/foo.vmt", "vpk"},
                                            {"materials/vpk-only.vmt", "vpk-only"}};
    if (!write_text(hl2 / "gameinfo.txt", R"("GameInfo"
{
    FileSystem
    {
        SearchPaths
        {
            game+mod hl2/custom/*
            game+mod hl2/hl2_pak.vpk
            mod+mod_write+default_write_path |gameinfo_path|.
        }
    }
}
)") || !write_text(hl2 / "custom/override/materials/foo.vmt", "custom") ||
        !write_text(hl2 / "materials/foo.vmt", "loose") ||
        !write_text(hl2 / "materials/loose-only.vmt", "loose-only") ||
        !write_bytes(hl2 / "hl2_pak_dir.vpk", make_vpk(pak_entries)))
    {
        return fail("cannot create the engine fixture");
    }

    ArchiveIndexOptions engine_options;
    engine_options.database = root / "engine.db";
    engine_options.game_folders.push_back(hl2);
    ArchiveIndexResult engine_result;
    if (!build_archive_index(engine_options, engine_result))
    {
        return fail(engine_result.error.empty() ? "engine build failed" : engine_result.error);
    }

    auto engine = IndexDatabase::open(engine_options.database);
    if (!engine)
    {
        return fail("cannot open the engine database");
    }
    auto engine_games = (*engine)->games();
    if (!engine_games || engine_games->size() != 1 || engine_games->front().search_paths.size() < 3)
    {
        return fail("engine search paths were not indexed");
    }
    auto engine_provider = (*engine)->engine_game_provider(engine_games->front().slug);
    if (!engine_provider)
    {
        return fail("cannot build the engine provider");
    }
    if (!expect(*engine_provider, "materials/foo.vmt", "custom") ||
        !expect(*engine_provider, "materials/loose-only.vmt", "loose-only") ||
        !expect(*engine_provider, "materials/vpk-only.vmt", "vpk-only"))
    {
        return fail("engine order is not custom > vpk > loose");
    }

    auto index_provider = (*engine)->game_provider(engine_games->front().slug);
    if (!index_provider || !expect(*index_provider, "materials/loose-only.vmt", "loose-only"))
    {
        return fail("game_provider does not include loose files");
    }

    // The mount exposes loose files next to the archive tree.
    auto mount = (*engine)->mount_provider();
    if (!mount ||
        !expect(*mount, engine_games->front().slug + "/materials/loose-only.vmt", "loose-only"))
    {
        return fail("mount provider does not expose loose files");
    }

    // Garry's Mod fixture: loose addons > workshop > local addons, later local
    // addon names win, and cfg/mount.cfg games sit below Garry's Mod content.
    const fs::path gmod = root / "steamapps/common/GarrysMod/garrysmod";
    const fs::path workshop = root / "steamapps/workshop/content/4000/123";
    const fs::path mounted = root / "steamapps/common/Mounted/mounted";
    const std::string mount_config =
        "\"mountcfg\"\n{\n    \"mounted\" \"" + mounted.generic_string() + "\"\n}\n";
    if (!write_text(gmod / "gameinfo.txt", R"("GameInfo"
{
    FileSystem
    {
        SearchPaths
        {
            game+mod garrysmod/addons/*
            game+mod garrysmod/garrysmod.vpk
            mod+mod_write+default_write_path |gameinfo_path|.
        }
    }
}
)") || !write_text(gmod / "cfg/mount.cfg", mount_config) ||
        !write_text(gmod / "addons/loose/lua/order.txt", "loose") ||
        !write_bytes(gmod / "addons/a.gma", make_gma("lua/local.txt", "a")) ||
        !write_bytes(gmod / "addons/b.gma", make_gma("lua/local.txt", "b")) ||
        !write_bytes(gmod / "addons/a2.gma", make_gma("lua/workshop.txt", "a")) ||
        !write_bytes(workshop / "w.gma", make_gma("lua/workshop.txt", "workshop")) ||
        !write_bytes(workshop / "w2.gma", make_gma("lua/order.txt", "workshop")) ||
        !write_text(mounted / "mounted-only.txt", "mounted"))
    {
        return fail("cannot create the Garry's Mod fixture");
    }

    ArchiveIndexOptions gmod_options;
    gmod_options.database = root / "gmod.db";
    gmod_options.game_folders.push_back(gmod);
    gmod_options.game_folders.push_back(mounted);
    ArchiveIndexResult gmod_result;
    if (!build_archive_index(gmod_options, gmod_result))
    {
        return fail(gmod_result.error.empty() ? "Garry's Mod build failed" : gmod_result.error);
    }

    auto gmod_index = IndexDatabase::open(gmod_options.database);
    if (!gmod_index)
    {
        return fail("cannot open the Garry's Mod database");
    }
    auto gmod_games = (*gmod_index)->games();
    if (!gmod_games || gmod_games->size() != 2 || !gmod_games->front().garrys_mod ||
        gmod_games->front().mounts.size() != 1)
    {
        return fail("Garry's Mod or its mount config was not indexed");
    }
    auto gmod_provider = (*gmod_index)->garrysmod_game_provider(gmod_games->front().slug);
    if (!gmod_provider)
    {
        return fail("cannot build the Garry's Mod provider");
    }
    if (!expect(*gmod_provider, "lua/order.txt", "loose"))
    {
        return fail("loose addons do not override workshop content");
    }
    if (!expect(*gmod_provider, "lua/workshop.txt", "workshop"))
    {
        return fail("workshop content does not override local addons");
    }
    if (!expect(*gmod_provider, "lua/local.txt", "b"))
    {
        return fail("later addon names do not win");
    }
    if (!expect(*gmod_provider, "mounted-only.txt", "mounted"))
    {
        return fail("cfg/mount.cfg games are not mounted");
    }

    fs::remove_all(root, ec);
    return 0;
}
