// SPDX-License-Identifier: MIT

#include <string>

#include "mount/archive_mount.hpp"
#include "util/format.hpp"

namespace
{

void usage(const char* program)
{
    sauce::fs::print_error("usage: {} [options] <mountpoint>\n"
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
                sauce::fs::print_error("{}: --database needs a path\n", argv[0]);
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
            sauce::fs::print_error("{}: unknown option: {}\n", argv[0], argument);
            usage(argv[0]);
            return 2;
        }
        if (!mountpoint.empty())
        {
            sauce::fs::print_error("{}: only one mountpoint is supported\n", argv[0]);
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
