#define FUSE_USE_VERSION 34

#include "backends/backend.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fuse3/fuse.h>

#include "core/volume.h"

namespace sauce::fs
{
namespace
{

std::string_view relative_path(const char* path)
{
    std::string_view name = path;
    if (!name.empty() && name.front() == '/')
    {
        name.remove_prefix(1);
    }
    return name;
}

bool lookup(const char* path, Entry& out)
{
    const std::string_view name = relative_path(path);
    if (name.empty())
    {
        return false;
    }

    const std::vector<Entry> entries = snapshot();
    const Entry* entry = find_entry(entries, name);
    if (entry == nullptr)
    {
        return false;
    }
    out = *entry;
    return true;
}

int do_getattr(const char* path, struct stat* info, struct fuse_file_info*)
{
    std::memset(info, 0, sizeof *info);
    info->st_uid = getuid();
    info->st_gid = getgid();
    info->st_atim.tv_sec = info->st_mtim.tv_sec = info->st_ctim.tv_sec = time(nullptr);

    if (relative_path(path).empty())
    {
        info->st_mode = S_IFDIR | 0555;
        info->st_nlink = 2;
        return 0;
    }

    Entry entry;
    if (!lookup(path, entry))
    {
        return -ENOENT;
    }

    info->st_mode = S_IFREG | 0444;
    info->st_nlink = 1;
    info->st_size = static_cast<off_t>(entry.data.size());
    return 0;
}

int do_readdir(const char* path, void* buffer, fuse_fill_dir_t fill, off_t, struct fuse_file_info*,
               enum fuse_readdir_flags)
{
    if (!relative_path(path).empty())
    {
        return -ENOENT;
    }

    fill(buffer, ".", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
    fill(buffer, "..", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
    for (const Entry& entry : snapshot())
    {
        if (fill(buffer, entry.name.c_str(), nullptr, 0, static_cast<fuse_fill_dir_flags>(0)) != 0)
        {
            break;
        }
    }
    return 0;
}

int do_open(const char* path, struct fuse_file_info* info)
{
    if ((info->flags & O_ACCMODE) != O_RDONLY)
    {
        return -EROFS;
    }

    Entry entry;
    return lookup(path, entry) ? 0 : -ENOENT;
}

int do_read(const char* path, char* buffer, size_t size, off_t offset, struct fuse_file_info*)
{
    Entry entry;
    if (!lookup(path, entry))
    {
        return -ENOENT;
    }
    if (static_cast<size_t>(offset) >= entry.data.size())
    {
        return 0;
    }

    const size_t available = entry.data.size() - static_cast<size_t>(offset);
    const size_t count = std::min(size, available);
    std::memcpy(buffer, entry.data.data() + offset, count);
    return static_cast<int>(count);
}

struct fuse_operations operations()
{
    struct fuse_operations ops{};
    ops.getattr = do_getattr;
    ops.readdir = do_readdir;
    ops.open = do_open;
    ops.read = do_read;
    return ops;
}

} // namespace

int mount(int argc, char** argv)
{
    if (argc < 1)
    {
        std::fprintf(stderr, "sauce-fs: mount point required\n");
        return 2;
    }

    // FUSE wants a mutable argv with the program name first.
    std::vector<std::string> arguments{"sauce-fs"};
    for (int i = 0; i < argc; ++i)
    {
        arguments.emplace_back(argv[i]);
    }
    arguments.emplace_back("-o");
    arguments.emplace_back("ro");

    std::vector<char*> raw;
    raw.reserve(arguments.size());
    for (std::string& argument : arguments)
    {
        raw.push_back(argument.data());
    }

    const struct fuse_operations ops = operations();
    return fuse_main(static_cast<int>(raw.size()), raw.data(), &ops, nullptr);
}

Check backend_check()
{
    const int runtime = fuse_version();
    std::string detail = "libfuse " + std::to_string(FUSE_MAJOR_VERSION) + "." +
                         std::to_string(FUSE_MINOR_VERSION) + " (runtime " +
                         std::to_string(runtime) + ")";

    if (access("/dev/fuse", R_OK | W_OK) != 0)
    {
        detail += ", /dev/fuse unavailable";
    }
    return {"fuse3", runtime >= 30, detail};
}

} // namespace sauce::fs
