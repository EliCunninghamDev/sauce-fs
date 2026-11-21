// SPDX-License-Identifier: MIT

#include <cstdio>
#include <memory>
#include <string>

#include "util/format.hpp"
#include <sauce/core/archive_access.hpp>

namespace
{

void usage(const char* program)
{
    sauce::fs::print_error(
        "usage: {} [options] [path]\n"
        "\n"
        "  --database PATH   SQLite index produced by sauce-fs-index\n"
        "  --game SLUG       Query a merged game view instead of the per-archive view\n"
        "  --order MODE      auto, engine, garrysmod, or index (default: auto)\n"
        "  --list            List a directory\n"
        "  --stat            Show entry kind and size\n"
        "  --resolve         Use Provider::resolve (raw pointer) instead of get\n"
        "  -h, --help        Show this message\n"
        "\n"
        "Without --list or --stat, writes the file bytes to stdout.\n"
        "Paths in the default view are: game/path (archives and loose files)\n"
        "With --game, paths are relative to that game: internal/path\n",
        program);
}

int fail(const sauce::fs::core::Error& error)
{
    sauce::fs::print_error("error: {}\n", error.message);
    return 1;
}

} // namespace

int main(int argc, char** argv)
{
    std::string database = "test";
    std::string game;
    std::string order;
    std::string path;
    bool list = false;
    bool stat = false;
    bool resolve_raw = false;

    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        if (argument == "-h" || argument == "--help")
        {
            usage(argv[0]);
            return 0;
        }
        if (argument == "--database")
        {
            if (index + 1 >= argc)
            {
                sauce::fs::print_error("{}: --database needs a path\n", argv[0]);
                return 2;
            }
            database = argv[++index];
            continue;
        }
        if (argument == "--game")
        {
            if (index + 1 >= argc)
            {
                sauce::fs::print_error("{}: --game needs a slug\n", argv[0]);
                return 2;
            }
            game = argv[++index];
            continue;
        }
        if (argument == "--list")
        {
            list = true;
            continue;
        }
        if (argument == "--order")
        {
            if (index + 1 >= argc)
            {
                sauce::fs::print_error("{}: --order needs a mode\n", argv[0]);
                return 2;
            }
            order = argv[++index];
            continue;
        }
        if (argument == "--stat")
        {
            stat = true;
            continue;
        }
        if (argument == "--resolve")
        {
            resolve_raw = true;
            continue;
        }
        if (!argument.empty() && argument[0] == '-')
        {
            sauce::fs::print_error("{}: unknown option: {}\n", argv[0], argument);
            usage(argv[0]);
            return 2;
        }
        if (!path.empty())
        {
            sauce::fs::print_error("{}: only one path is supported\n", argv[0]);
            return 2;
        }
        path = argument;
    }

    if ((list && stat) || (resolve_raw && (list || stat)))
    {
        sauce::fs::print_error("{}: --list, --stat, and --resolve are mutually exclusive\n",
                               argv[0]);
        return 2;
    }

    sauce::fs::core::GameOrder policy = sauce::fs::core::GameOrder::Auto;
    if (!order.empty())
    {
        if (order == "auto")
        {
            policy = sauce::fs::core::GameOrder::Auto;
        }
        else if (order == "engine")
        {
            policy = sauce::fs::core::GameOrder::Engine;
        }
        else if (order == "garrysmod" || order == "gmod")
        {
            policy = sauce::fs::core::GameOrder::GarrysMod;
        }
        else if (order == "index")
        {
            policy = sauce::fs::core::GameOrder::Index;
        }
        else
        {
            sauce::fs::print_error("{}: unknown order: {}\n", argv[0], order);
            return 2;
        }
    }

    if (path.empty())
    {
        path = "/";
    }

    auto opened = sauce::fs::core::IndexDatabase::open(database);
    if (!opened)
    {
        return fail(opened.error());
    }
    const std::shared_ptr<sauce::fs::core::IndexDatabase> index = *opened;

    auto provider =
        game.empty() ? index->mount_provider() : index->ordered_game_provider(game, policy);
    if (!provider)
    {
        return fail(provider.error());
    }

    if (list)
    {
        auto entries = (*provider)->list(path);
        if (!entries)
        {
            return fail(entries.error());
        }
        for (const sauce::fs::core::DirectoryEntry& entry : *entries)
        {
            const bool directory = entry.info.kind == sauce::fs::core::NodeInfo::Kind::Directory;
            if (directory)
            {
                sauce::fs::print("d {}/\n", entry.name);
            }
            else
            {
                sauce::fs::print("f {}\t{}\n", entry.name, entry.info.size);
            }
        }
        return 0;
    }

    if (stat)
    {
        auto info = (*provider)->stat(path);
        if (!info)
        {
            return fail(info.error());
        }
        if (info->kind == sauce::fs::core::NodeInfo::Kind::Directory)
        {
            sauce::fs::print("d {}\n", path);
        }
        else
        {
            sauce::fs::print("f {}\t{}\n", path, info->size);
        }
        return 0;
    }

    if (resolve_raw)
    {
        sauce::fs::core::ResolvedEntry entry;
        if (!(*provider)->resolve(path, entry))
        {
            sauce::fs::print_error("error: cannot resolve {}\n", path);
            return 1;
        }
        if (entry.info.kind == sauce::fs::core::NodeInfo::Kind::Directory)
        {
            sauce::fs::print("d {}\n", path);
            return 0;
        }
        if (entry.size != 0)
        {
            std::fwrite(entry.data, 1, entry.size, stdout);
        }
        return 0;
    }

    auto blob = (*provider)->get(path);
    if (!blob)
    {
        return fail(blob.error());
    }
    if (!(*blob)->empty())
    {
        std::fwrite((*blob)->data(), 1, (*blob)->size(), stdout);
    }
    return 0;
}
