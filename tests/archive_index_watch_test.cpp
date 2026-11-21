// SPDX-License-Identifier: MIT

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "util/format.hpp"
#include <sauce/core/archive_access.hpp>
#include <sauce/core/archive_index.hpp>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

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

void append_string(std::vector<std::uint8_t>& bytes, const char* text)
{
    while (*text != '\0')
    {
        bytes.push_back(static_cast<std::uint8_t>(*text++));
    }
    bytes.push_back(0);
}

/// Minimal VPK v1 with one entry: materials/foo.vmt. Embedded entries store
/// their bytes after the tree; side entries put them in <stem>_000.vpk.
std::vector<std::uint8_t> make_vpk(const std::string& contents, bool embedded = true)
{
    std::vector<std::uint8_t> tree;
    append_string(tree, "vmt");
    append_string(tree, "materials");
    append_string(tree, "foo");
    append_u32(tree, 0);                     // crc
    append_u16(tree, 0);                     // preload bytes
    append_u16(tree, embedded ? 0x7FFF : 0); // archive index
    append_u32(tree, 0);                     // offset
    append_u32(tree, static_cast<std::uint32_t>(contents.size()));
    append_u16(tree, 0xFFFF); // entry terminator
    tree.push_back(0);        // end of names
    tree.push_back(0);        // end of directories
    tree.push_back(0);        // end of extensions

    std::vector<std::uint8_t> file;
    append_u32(file, 0x55AA1234u);
    append_u32(file, 1);
    append_u32(file, static_cast<std::uint32_t>(tree.size()));
    file.insert(file.end(), tree.begin(), tree.end());
    if (embedded)
    {
        file.insert(file.end(), contents.begin(), contents.end());
    }
    return file;
}

std::vector<std::uint8_t> make_bytes(const std::string& text)
{
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

bool write_file(const fs::path& path, const std::vector<std::uint8_t>& bytes)
{
    const fs::path temporary = path.string() + ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            return false;
        }
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        if (!stream)
        {
            return false;
        }
    }
    // Windows can briefly hold the old file open while the watcher reads it.
    for (int attempt = 0; attempt < 40; ++attempt)
    {
        std::error_code ec;
        fs::remove(path, ec);
        ec.clear();
        fs::rename(temporary, path, ec);
        if (!ec)
        {
            return true;
        }
        std::this_thread::sleep_for(50ms);
    }
    return false;
}

bool remove_file(const fs::path& path)
{
    for (int attempt = 0; attempt < 40; ++attempt)
    {
        std::error_code ec;
        fs::remove(path, ec);
        if (!ec || !fs::exists(path))
        {
            return true;
        }
        std::this_thread::sleep_for(50ms);
    }
    return false;
}

bool wait_for(const std::function<bool()>& predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
        {
            return true;
        }
        std::this_thread::sleep_for(50ms);
    }
    return predicate();
}

int fail(const char* message)
{
    sauce::fs::print_error("index watch test: {}\n", message);
    return 1;
}

} // namespace

int main()
{
    const fs::path root =
        fs::temp_directory_path() /
        ("sauce-fs-watch-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path game = root / "game";
    const fs::path database = root / "index.db";
    const fs::path first = game / "test_dir.vpk";
    const fs::path second = game / "other_dir.vpk";
    const fs::path loose = game / "loose.txt";

    std::error_code ec;
    fs::create_directories(game, ec);
    if (ec || !write_file(first, make_vpk("hello v1")) ||
        !write_file(loose, make_bytes("loose v1")))
    {
        return fail("cannot create fixture");
    }

    sauce::fs::core::ArchiveIndexOptions options;
    options.database = database;
    options.game_folders.push_back(game);

    sauce::fs::core::ArchiveWatchOptions watch;
    watch.debounce = 50ms;
    watch.verbose = false;

    sauce::fs::core::ArchiveIndexResult result;
    sauce::fs::core::ArchiveIndexWatcher watcher(options, watch);
    bool watcher_result = false;
    std::thread thread([&] { watcher_result = watcher.run(result); });

    std::shared_ptr<sauce::fs::core::IndexDatabase> index;
    if (!wait_for(
            [&]
            {
                auto opened = sauce::fs::core::IndexDatabase::open(database);
                if (!opened)
                {
                    return false;
                }
                index = *opened;
                return true;
            },
            10s))
    {
        watcher.stop();
        thread.join();
        return fail("database was not created");
    }

    auto read_file = [&](const std::string& path, std::string& out)
    {
        auto provider = index->game_provider("game");
        if (!provider)
        {
            return false;
        }
        auto blob = (*provider)->get(path);
        if (!blob)
        {
            return false;
        }
        out.assign(reinterpret_cast<const char*>((*blob)->data()), (*blob)->size());
        return true;
    };

    auto archive_count = [&]
    {
        auto archives = index->archives();
        return archives ? archives->size() : std::size_t{0};
    };

    auto read_loose = [&](const std::string& path, std::string& out)
    {
        auto provider = index->ordered_game_provider("game", sauce::fs::core::GameOrder::Index);
        if (!provider)
        {
            return false;
        }
        auto blob = (*provider)->get(path);
        if (!blob)
        {
            return false;
        }
        out.assign(reinterpret_cast<const char*>((*blob)->data()), (*blob)->size());
        return true;
    };

    std::string contents;
    if (!wait_for([&] { return read_file("materials/foo.vmt", contents); }, 10s) ||
        contents != "hello v1")
    {
        watcher.stop();
        thread.join();
        return fail("initial index did not serve the fixture");
    }

    std::string loose_contents;
    if (!wait_for([&] { return read_loose("loose.txt", loose_contents); }, 10s) ||
        loose_contents != "loose v1")
    {
        watcher.stop();
        thread.join();
        return fail("initial index did not serve loose files");
    }

    // Give efsw a moment to install its watches after the initial build.
    std::this_thread::sleep_for(1000ms);
    if (!write_file(loose, make_bytes("loose v2")))
    {
        watcher.stop();
        thread.join();
        return fail("cannot update loose fixture");
    }
    if (!wait_for(
            [&] { return read_loose("loose.txt", loose_contents) && loose_contents == "loose v2"; },
            10s))
    {
        watcher.stop();
        thread.join();
        return fail("watcher did not pick up a changed loose file");
    }
    if (!remove_file(loose) ||
        !wait_for([&] { return !read_loose("loose.txt", loose_contents); }, 10s))
    {
        watcher.stop();
        thread.join();
        return fail("watcher did not pick up a removed loose file");
    }
    if (!write_file(first, make_vpk("hello v2")))
    {
        watcher.stop();
        thread.join();
        return fail("cannot replace fixture");
    }
    if (!wait_for([&]
                  { return read_file("materials/foo.vmt", contents) && contents == "hello v2"; },
                  10s))
    {
        watcher.stop();
        thread.join();
        return fail("watcher did not pick up a replaced archive");
    }

    if (!write_file(second, make_vpk("other v1")))
    {
        watcher.stop();
        thread.join();
        return fail("cannot add fixture");
    }
    if (!wait_for([&] { return archive_count() == 2; }, 10s))
    {
        watcher.stop();
        thread.join();
        return fail("watcher did not pick up an added archive");
    }

    auto mounted = index->mount_provider();
    if (!mounted)
    {
        watcher.stop();
        thread.join();
        return fail("cannot build mount provider after add");
    }
    auto other = (*mounted)->get("game/other/materials/foo.vmt");
    if (!other || std::string(reinterpret_cast<const char*>((*other)->data()), (*other)->size()) !=
                      "other v1")
    {
        watcher.stop();
        thread.join();
        return fail("added archive is not present in the mount view");
    }

    if (!remove_file(first))
    {
        watcher.stop();
        thread.join();
        return fail("cannot remove fixture");
    }
    if (!wait_for([&] { return archive_count() == 1; }, 10s))
    {
        watcher.stop();
        thread.join();
        return fail("watcher did not pick up a removed archive");
    }

    // A VPK side archive carries the bytes while the _dir file only carries
    // offsets. Replacing the side file leaves the directory hash untouched but
    // must still invalidate the runtime's cached entry blobs.
    const fs::path side_dir = game / "side_dir.vpk";
    const fs::path side_data = game / "side_000.vpk";
    if (!write_file(side_dir, make_vpk("side v1", false)) ||
        !write_file(side_data, make_bytes("side v1")))
    {
        watcher.stop();
        thread.join();
        return fail("cannot create side-archive fixture");
    }

    auto read_mounted = [&](const std::string& path, std::string& out)
    {
        auto mounted = index->mount_provider();
        if (!mounted)
        {
            return false;
        }
        auto blob = (*mounted)->get(path);
        if (!blob)
        {
            return false;
        }
        out.assign(reinterpret_cast<const char*>((*blob)->data()), (*blob)->size());
        return true;
    };

    if (!wait_for(
            [&]
            {
                return read_mounted("game/side/materials/foo.vmt", contents) &&
                       contents == "side v1";
            },
            10s))
    {
        watcher.stop();
        thread.join();
        return fail("side archive was not indexed");
    }

    if (!write_file(side_data, make_bytes("side v2")))
    {
        watcher.stop();
        thread.join();
        return fail("cannot replace side archive");
    }
    if (!wait_for(
            [&]
            {
                return read_mounted("game/side/materials/foo.vmt", contents) &&
                       contents == "side v2";
            },
            10s))
    {
        watcher.stop();
        thread.join();
        return fail("side archive change did not invalidate cached entry bytes");
    }

    // Removing and restoring the side payload must add and remove the entry
    // rows even though the _dir file bytes never change.
    if (!remove_file(side_data))
    {
        watcher.stop();
        thread.join();
        return fail("cannot remove side archive");
    }
    if (!wait_for([&] { return !read_mounted("game/side/materials/foo.vmt", contents); }, 10s))
    {
        watcher.stop();
        thread.join();
        return fail("removed side archive left its entry indexed");
    }
    if (!write_file(side_data, make_bytes("side v3")))
    {
        watcher.stop();
        thread.join();
        return fail("cannot restore side archive");
    }
    if (!wait_for(
            [&]
            {
                return read_mounted("game/side/materials/foo.vmt", contents) &&
                       contents == "side v3";
            },
            10s))
    {
        watcher.stop();
        thread.join();
        return fail("restored side archive was not reparsed");
    }

    watcher.stop();
    thread.join();
    if (!watcher_result)
    {
        return fail("watcher returned an error");
    }

    fs::remove_all(root, ec);
    return 0;
}
