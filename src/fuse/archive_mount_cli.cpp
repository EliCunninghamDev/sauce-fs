// SPDX-License-Identifier: MIT

#include <charconv>
#include <string>
#include <string_view>
#include <system_error>

#include "fuse/archive_mount.hpp"
#include "util/format.hpp"

namespace
{

/// Parse an octal permission mask such as 644 or 0755.
bool parse_mode(std::string_view text, unsigned& mode)
{
    if (text.empty())
    {
        return false;
    }
    unsigned value = 0;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const std::from_chars_result parsed = std::from_chars(begin, end, value, 8);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value > 0777)
    {
        return false;
    }
    mode = value;
    return true;
}

void usage(const char* program)
{
    sauce::fs::print_error("usage: {} [options] <mountpoint>\n"
                           "\n"
                           "  --database PATH    SQLite index to mount (default: test)\n"
                           "  --file-mode OCTAL  Permission bits for files (default: 0444)\n"
                           "  --dir-mode OCTAL   Permission bits for directories (default: 0555)\n"
                           "  -f, --foreground   Stay in the foreground\n"
                           "  -d, --debug        Enable libfuse debug logging\n"
                           "  -h, --help         Show this message\n"
                           "\n"
                           "The mount is read-only. Unmount with: fusermount3 -u <mountpoint>\n",
                           program);
}

} // namespace

int main(int argc, char** argv)
{
    sauce::fs::fuse::ArchiveMountOptions options;
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
        if (argument == "--file-mode" || argument == "--dir-mode")
        {
            if (index + 1 >= argc)
            {
                sauce::fs::print_error("{}: {} needs an octal mode\n", argv[0], argument);
                return 2;
            }
            const std::string value = argv[++index];
            unsigned* const mode =
                argument == "--file-mode" ? &options.file_mode : &options.dir_mode;
            if (!parse_mode(value, *mode))
            {
                sauce::fs::print_error("{}: invalid octal mode for {}: {}\n", argv[0], argument,
                                       value);
                return 2;
            }
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
    return sauce::fs::fuse::run_archive_mount(options, mountpoint);
}
