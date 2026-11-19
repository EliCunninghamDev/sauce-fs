// SPDX-License-Identifier: MIT

#include <cstdio>
#include <string>

#include "mount/archive_mount.hpp"

namespace
{

void usage(const char* program)
{
    std::fprintf(stderr,
                 "usage: %s [options] <mountpoint>\n"
                 "\n"
                 "  --database PATH   SQLite index to mount (default: test)\n"
                 "  -f, --foreground  Stay in the foreground\n"
                 "  -d, --debug       Enable libfuse debug logging\n"
                 "  -h, --help        Show this message\n"
                 "\n"
                 "The mount is read-only. Unmount with: fusermount3 -u <mountpoint>\n",
                 program);
}

} // namespace

int main(int argc, char** argv)
{
    sauce::fs::ArchiveMountOptions options;
    std::string mountpoint;

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
        if (argument == "-f" || argument == "--foreground")
        {
            options.foreground = true;
            continue;
        }
        if (argument == "-d" || argument == "--debug")
        {
            options.debug = true;
            options.foreground = true;
            continue;
        }
        if (!argument.empty() && argument[0] == '-')
        {
            std::fprintf(stderr, "%s: unknown option: %s\n", argv[0], argument.c_str());
            usage(argv[0]);
            return 2;
        }
        if (!mountpoint.empty())
        {
            std::fprintf(stderr, "%s: only one mountpoint is supported\n", argv[0]);
            return 2;
        }
        mountpoint = argument;
    }

    if (mountpoint.empty())
    {
        usage(argv[0]);
        return 2;
    }
    return sauce::fs::run_archive_mount(options, mountpoint);
}
