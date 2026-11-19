// SPDX-License-Identifier: MIT

#include <cstdio>
#include <string>

#include "index/archive_index.hpp"

namespace
{

void usage(const char* program)
{
    std::fprintf(stderr,
                 "usage: %s [options] [game-folder...]\n"
                 "\n"
                 "  --database PATH     SQLite database to write (default: test)\n"
                 "  --steam-root PATH   Steam root used for auto-discovery (repeatable)\n"
                 "  -h, --help          Show this message\n"
                 "\n"
                 "With no game folders, installed Source titles are discovered through Steam.\n"
                 "One worker scans each game folder; Garry's Mod also gets a GMA/.bin crawler.\n"
                 "Discovered archives are hashed in parallel.\n",
                 program);
}

} // namespace

int main(int argc, char** argv)
{
    sauce::fs::ArchiveIndexOptions options;

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
            options.database = argv[++index];
            continue;
        }
        if (argument == "--steam-root")
        {
            if (index + 1 >= argc)
            {
                std::fprintf(stderr, "%s: --steam-root needs a path\n", argv[0]);
                return 2;
            }
            options.steam_roots.emplace_back(argv[++index]);
            continue;
        }
        if (!argument.empty() && argument[0] == '-')
        {
            std::fprintf(stderr, "%s: unknown option: %s\n", argv[0], argument.c_str());
            usage(argv[0]);
            return 2;
        }
        options.game_folders.emplace_back(argument);
    }

    sauce::fs::ArchiveIndexResult result;
    if (!sauce::fs::build_archive_index(options, result))
    {
        return 1;
    }
    return 0;
}
