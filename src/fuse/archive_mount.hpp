// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>

namespace sauce::fs::fuse
{

struct ArchiveMountOptions
{
    /// SQLite database produced by sauce-fs-index.
    std::filesystem::path database = "test";
    /// Permission bits reported for regular files (octal). The mount itself is
    /// read-only; these bits only decide what a copy inherits from `cp`.
    unsigned file_mode = 0444;
    /// Permission bits reported for directories (octal).
    unsigned dir_mode = 0555;
    /// Stay in the foreground (useful for tests and service managers).
    bool foreground = false;
    /// Ask libfuse for debug logging.
    bool debug = false;
};

/// Mount the index read-only. The mount root corresponds to `sourcegames`;
/// games are directories below it, then one directory per archive, then the
/// archive's internal tree.
///
/// Returns the libfuse exit status (0 on a clean unmount).
int run_archive_mount(const ArchiveMountOptions& options, const std::filesystem::path& mountpoint);

} // namespace sauce::fs::fuse
