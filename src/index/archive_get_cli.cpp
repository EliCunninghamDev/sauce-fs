// SPDX-License-Identifier: MIT

#include <cstdio>
#include <memory>
#include <string>

#include "index/archive_access.hpp"

namespace
{

void usage(const char* program)
{
    std::fprintf(stderr,
                 "usage: %s [options] [path]\n"
                 "\n"
                 "  --database PATH   SQLite index produced by sauce-fs-index\n"
                 "  --game SLUG       Query a merged game view instead of the per-archive view\n"
                 "  --list            List a directory\n"
                 "  --stat            Show entry kind and size\n"
                 "  --resolve         Use Provider::resolve (raw pointer) instead of get\n"
                 "  -h, --help        Show this message\n"
                 "\n"
                 "Without --list or --stat, writes the file bytes to stdout.\n"
                 "Paths in the default view are: game/archive-mount-path/internal/path\n"
                 "With --game, paths are relative to that game: internal/path\n",
                 program);
}

int fail(const sauce::fs::Error& error)
{
    std::fprintf(stderr, "error: %s\n", error.message.c_str());
    return 1;
}

} // namespace

int main(int argc, char** argv)
{
    std::string database = "test";
    std::string game;
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
                std::fprintf(stderr, "%s: --database needs a path\n", argv[0]);
                return 2;
            }
            database = argv[++index];
            continue;
        }
        if (argument == "--game")
        {
            if (index + 1 >= argc)
            {
                std::fprintf(stderr, "%s: --game needs a slug\n", argv[0]);
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
            std::fprintf(stderr, "%s: unknown option: %s\n", argv[0], argument.c_str());
            usage(argv[0]);
            return 2;
        }
        if (!path.empty())
        {
            std::fprintf(stderr, "%s: only one path is supported\n", argv[0]);
            return 2;
        }
        path = argument;
    }

    if ((list && stat) || (resolve_raw && (list || stat)))
    {
        std::fprintf(stderr, "%s: --list, --stat, and --resolve are mutually exclusive\n", argv[0]);
        return 2;
    }
    if (path.empty())
    {
        path = "/";
    }

    auto opened = sauce::fs::IndexDatabase::open(database);
    if (!opened)
    {
        return fail(opened.error());
    }
    const std::shared_ptr<sauce::fs::IndexDatabase> index = *opened;

    auto provider = game.empty() ? index->mount_provider() : index->game_provider(game);
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
        for (const sauce::fs::DirectoryEntry& entry : *entries)
        {
            const bool directory = entry.info.kind == sauce::fs::NodeInfo::Kind::Directory;
            if (directory)
            {
                std::printf("d %s/\n", entry.name.c_str());
            }
            else
            {
                std::printf("f %s\t%llu\n", entry.name.c_str(),
                            static_cast<unsigned long long>(entry.info.size));
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
        if (info->kind == sauce::fs::NodeInfo::Kind::Directory)
        {
            std::printf("d %s\n", path.c_str());
        }
        else
        {
            std::printf("f %s\t%llu\n", path.c_str(), static_cast<unsigned long long>(info->size));
        }
        return 0;
    }

    if (resolve_raw)
    {
        sauce::fs::ResolvedEntry entry;
        if (!(*provider)->resolve(path, entry))
        {
            std::fprintf(stderr, "error: cannot resolve %s\n", path.c_str());
            return 1;
        }
        if (entry.info.kind == sauce::fs::NodeInfo::Kind::Directory)
        {
            std::printf("d %s\n", path.c_str());
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
