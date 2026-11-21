// SPDX-License-Identifier: MIT

#define FUSE_USE_VERSION 34

#include "fuse/archive_mount.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "util/format.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <fuse3/fuse.h>
#include <xxhash.h>

#include "core/archive_access.hpp"

namespace sauce::fs::fuse
{

namespace core = ::sauce::fs::core;

namespace
{

struct OpenFile
{
    core::Blob blob;
};

struct MountState
{
    std::shared_ptr<core::IndexDatabase> database;
    core::ProviderPtr root;
    std::time_t mount_time = 0;
    unsigned file_mode = 0444;
    unsigned dir_mode = 0555;
    mutable std::mutex root_mutex;

    /// Pick up an index update committed by sauce-fs-index --watch. The
    /// database object notices the new PRAGMA data_version and reloads its
    /// metadata; the tree is rebuilt only when that happens.
    core::ProviderPtr current_root()
    {
        std::lock_guard lock(root_mutex);
        auto changed = database->refresh();
        if (!changed || !*changed)
        {
            return root;
        }
        auto rebuilt = database->mount_provider();
        if (rebuilt)
        {
            root = std::move(*rebuilt);
        }
        return root;
    }
};

MountState* state_of()
{
    return static_cast<MountState*>(fuse_get_context()->private_data);
}

ino_t inode_for(const char* path)
{
    const std::uint64_t hash = XXH3_64bits(path, std::strlen(path));
    return static_cast<ino_t>(hash == 0 ? 1 : hash);
}

int errno_for(const core::Error& error)
{
    switch (error.code)
    {
    case core::ErrorCode::NotFound:
        return -ENOENT;
    case core::ErrorCode::InvalidPath:
        return -EINVAL;
    case core::ErrorCode::NotAFile:
        return -EISDIR;
    case core::ErrorCode::NotADirectory:
        return -ENOTDIR;
    case core::ErrorCode::Io:
    case core::ErrorCode::Unsupported:
    case core::ErrorCode::Database:
    case core::ErrorCode::NotSupported:
        return -EIO;
    }
    return -EIO;
}

void fill_stat(const MountState& state, const char* path, const core::NodeInfo& info,
               struct stat& attributes)
{
    std::memset(&attributes, 0, sizeof attributes);
    attributes.st_uid = getuid();
    attributes.st_gid = getgid();
    attributes.st_atim.tv_sec = attributes.st_mtim.tv_sec = attributes.st_ctim.tv_sec =
        state.mount_time;
    attributes.st_blksize = 4096;
    attributes.st_ino = std::strcmp(path, "/") == 0 ? 1 : inode_for(path);

    if (info.kind == core::NodeInfo::Kind::Directory)
    {
        attributes.st_mode = S_IFDIR | state.dir_mode;
        attributes.st_nlink = 2;
        return;
    }

    attributes.st_mode = S_IFREG | state.file_mode;
    attributes.st_nlink = 1;
    attributes.st_size = static_cast<off_t>(info.size);
    attributes.st_blocks = static_cast<blkcnt_t>((info.size + 511) / 512);
}

int do_getattr(const char* path, struct stat* attributes, struct fuse_file_info*)
{
    try
    {
        MountState* state = state_of();
        core::ProviderPtr root = state->current_root();
        if (!root)
        {
            return -EIO;
        }
        auto info = root->stat(path);
        if (!info)
        {
            return errno_for(info.error());
        }
        fill_stat(*state, path, *info, *attributes);
        return 0;
    }
    catch (...)
    {
        return -EIO;
    }
}

int do_readdir(const char* path, void* buffer, fuse_fill_dir_t fill, off_t, struct fuse_file_info*,
               enum fuse_readdir_flags)
{
    try
    {
        MountState* state = state_of();
        core::ProviderPtr root = state->current_root();
        if (!root)
        {
            return -EIO;
        }
        auto entries = root->list(path);
        if (!entries)
        {
            return errno_for(entries.error());
        }
        const auto emit = [&](const std::string& name)
        { return fill(buffer, name.c_str(), nullptr, 0, static_cast<fuse_fill_dir_flags>(0)); };
        emit(".");
        emit("..");
        for (const core::DirectoryEntry& entry : *entries)
        {
            if (emit(entry.name) != 0)
            {
                break;
            }
        }
        return 0;
    }
    catch (...)
    {
        return -EIO;
    }
}

int do_open(const char* path, struct fuse_file_info* file)
{
    if ((file->flags & O_ACCMODE) != O_RDONLY)
    {
        return -EROFS;
    }
    try
    {
        MountState* state = state_of();
        core::ProviderPtr root = state->current_root();
        if (!root)
        {
            return -EIO;
        }
        auto info = root->stat(path);
        if (!info)
        {
            return errno_for(info.error());
        }
        if (info->kind == core::NodeInfo::Kind::Directory)
        {
            return -EISDIR;
        }
        auto blob = root->get(path);
        if (!blob)
        {
            return errno_for(blob.error());
        }
        auto* open = new OpenFile{std::move(*blob)};
        file->fh = reinterpret_cast<std::uint64_t>(open);
        // Keep_cache would serve stale bytes after --watch replaces an archive;
        // the index is always current and the provider cache is cheap.
        file->keep_cache = 0;
        return 0;
    }
    catch (const std::bad_alloc&)
    {
        return -ENOMEM;
    }
    catch (...)
    {
        return -EIO;
    }
}

int do_read(const char*, char* buffer, size_t size, off_t offset, struct fuse_file_info* file)
{
    if (offset < 0)
    {
        return -EINVAL;
    }
    const auto* open = reinterpret_cast<const OpenFile*>(file->fh);
    if (open == nullptr || !open->blob)
    {
        return -EIO;
    }
    const core::Bytes& data = *open->blob;
    if (static_cast<std::uint64_t>(offset) >= data.size())
    {
        return 0;
    }
    const std::size_t available =
        std::min<std::size_t>(data.size() - static_cast<std::size_t>(offset),
                              std::min<std::size_t>(size, static_cast<std::size_t>(INT_MAX)));
    std::memcpy(buffer, data.data() + offset, available);
    return static_cast<int>(available);
}

int do_release(const char*, struct fuse_file_info* file)
{
    delete reinterpret_cast<OpenFile*>(file->fh);
    file->fh = 0;
    return 0;
}

int do_statfs(const char*, struct statvfs* info)
{
    MountState* state = state_of();
    std::memset(info, 0, sizeof *info);
    info->f_bsize = 4096;
    info->f_frsize = 4096;
    info->f_blocks = state->database->logical_bytes() / 4096;
    info->f_bfree = 0;
    info->f_bavail = 0;
    info->f_files = state->database->file_count();
    info->f_ffree = 0;
    info->f_namemax = 255;
    return 0;
}

} // namespace

int run_archive_mount(const ArchiveMountOptions& options, const std::filesystem::path& mountpoint)
{
    auto database = core::IndexDatabase::open(options.database);
    if (!database)
    {
        sauce::fs::print_error("sauce-fs-mount: {}\n", database.error().message);
        return 1;
    }
    auto root = (*database)->mount_provider();
    if (!root)
    {
        sauce::fs::print_error("sauce-fs-mount: {}\n", root.error().message);
        return 1;
    }

    MountState state;
    state.database = std::move(*database);
    state.root = std::move(*root);
    state.mount_time = std::time(nullptr);
    state.file_mode = options.file_mode;
    state.dir_mode = options.dir_mode;

    struct fuse_operations operations = {};
    operations.getattr = do_getattr;
    operations.readdir = do_readdir;
    operations.open = do_open;
    operations.read = do_read;
    operations.release = do_release;
    operations.statfs = do_statfs;

    std::vector<std::string> raw_arguments;
    raw_arguments.emplace_back("sauce-fs-mount");
    raw_arguments.push_back(mountpoint.string());
    if (options.foreground)
    {
        raw_arguments.emplace_back("-f");
    }
    if (options.debug)
    {
        raw_arguments.emplace_back("-d");
    }
    raw_arguments.emplace_back("-o");
    raw_arguments.emplace_back("ro,default_permissions,fsname=sourcegames");

    std::vector<char*> arguments;
    arguments.reserve(raw_arguments.size());
    for (std::string& argument : raw_arguments)
    {
        arguments.push_back(argument.data());
    }
    return fuse_main(static_cast<int>(arguments.size()), arguments.data(), &operations, &state);
}

} // namespace sauce::fs::fuse
