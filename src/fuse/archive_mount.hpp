// SPDX-License-Identifier: MIT

/// \file
/// \brief Read-only FUSE3 frontend over the index providers.
///
/// The mount layer is Linux-only and is not part of the installed `core`
/// package. It consumes an index database through the provider API and exposes
/// the per-archive tree at a mountpoint.

#pragma once

#include <filesystem>

#include <sauce/core/log.hpp>

namespace sauce::fs::fuse
{

/// \brief Options for one read-only mount.
struct ArchiveMountOptions
{
    /// \brief SQLite database produced by sauce-fs-index or the index watcher.
    std::filesystem::path database = "test";

    /// \brief Permission bits reported for regular files (octal).
    ///
    /// The mount itself is read-only; these bits only decide what a copy
    /// inherits from `cp`.
    unsigned file_mode = 0444;

    /// \brief Permission bits reported for directories (octal).
    unsigned dir_mode = 0555;

    /// \brief Stay in the foreground.
    ///
    /// Useful for tests and service managers that expect a supervised process.
    bool foreground = false;

    /// \brief Ask libfuse for debug logging.
    bool debug = false;

    /// \brief Diagnostics sink.
    ///
    /// Empty by default, so mounting embedded in another program stays silent
    /// unless a callback is installed.
    core::LogOptions log;
};

/// \brief Mount an index database read-only.
///
/// \details The mount root corresponds to `sourcegames`: games are directories
/// below it, then one directory per archive, then the archive's internal tree.
/// Archive data is read on demand from the original files.
///
/// \param options Database path, permission bits, and diagnostics sink.
/// \param mountpoint Existing directory that receives the mount.
/// \return The libfuse exit status; zero on a clean unmount.
int run_archive_mount(const ArchiveMountOptions& options, const std::filesystem::path& mountpoint);

} // namespace sauce::fs::fuse
