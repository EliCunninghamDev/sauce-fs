// SPDX-License-Identifier: MIT

#include <chrono>
#include <cstdlib>
#include <string>

#include <sauce/core/archive_index.hpp>

#include "util/console_log.hpp"
#include "util/format.hpp"

namespace
{

void usage(const char* program)
{
    sauce::fs::print_error(
        "usage: {} [options] [game-folder...]\n"
        "\n"
        "  --database PATH     SQLite database to write (default: test)\n"
        "  --steam-root PATH   Steam root used for auto-discovery (repeatable)\n"
        "  --watch             Keep running and update the index when archives change\n"
        "  --debounce-ms N     Watch quiet period before rescanning (default: 500)\n"
        "  -h, --help          Show this message\n"
        "\n"
        "With no game folders, installed Source titles are discovered through Steam.\n"
        "One worker scans each game folder; Garry's Mod also gets a GMA/.bin crawler.\n"
        "Discovered archives are hashed in parallel. --watch only rehashes archives\n"
        "whose size or modification time changed.\n",
        program);
}

} // namespace

int main(int argc, char** argv)
{
    sauce::fs::core::ArchiveIndexOptions options;
    sauce::fs::core::ArchiveWatchOptions watch;
    options.log = sauce::fs::console_log();
    bool watch_mode = false;

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
            options.database = argv[++index];
            continue;
        }
        if (argument == "--steam-root")
        {
            if (index + 1 >= argc)
            {
                sauce::fs::print_error("{}: --steam-root needs a path\n", argv[0]);
                return 2;
            }
            options.steam_roots.emplace_back(argv[++index]);
            continue;
        }
        if (argument == "--watch")
        {
            watch_mode = true;
            continue;
        }
        if (argument == "--debounce-ms")
        {
            if (index + 1 >= argc)
            {
                sauce::fs::print_error("{}: --debounce-ms needs a number\n", argv[0]);
                return 2;
            }
            char* end = nullptr;
            const char* text = argv[++index];
            const unsigned long value = std::strtoul(text, &end, 10);
            if (end == text || *end != '\0' || value > 3600000)
            {
                sauce::fs::print_error("{}: invalid --debounce-ms value: {}\n", argv[0], text);
                return 2;
            }
            watch.debounce = std::chrono::milliseconds(value);
            continue;
        }
        if (!argument.empty() && argument[0] == '-')
        {
            sauce::fs::print_error("{}: unknown option: {}\n", argv[0], argument);
            usage(argv[0]);
            return 2;
        }
        options.game_folders.emplace_back(argument);
    }

    if (watch_mode)
    {
        sauce::fs::core::ArchiveIndexResult result;
        sauce::fs::core::ArchiveIndexWatcher watcher(options, watch);
        return watcher.run(result) ? 0 : 1;
    }

    sauce::fs::core::ArchiveIndexResult result;
    if (!sauce::fs::core::build_archive_index(options, result))
    {
        return 1;
    }
    return 0;
}
