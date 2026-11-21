// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <set>
#include <string>
#include <vector>

#include <sauce/core/archive_index.hpp>

namespace sauce::fs::core::detail
{

/// \brief What one incremental rescan changed, for logging and event emission.
struct WatchRefreshSummary
{
    /// \brief Archive paths committed as new rows.
    std::vector<std::string> added;

    /// \brief Archive paths committed with new contents or metadata.
    std::vector<std::string> changed;

    /// \brief Archive paths whose rows were removed.
    std::vector<std::string> removed;

    /// \brief Archive paths that could not be read; their rows were kept.
    std::vector<std::string> unreadable;

    /// \brief Per-file parser diagnostics.
    std::vector<std::string> warnings;

    /// \brief Loose files added or updated by the commit.
    std::size_t loose_added = 0;

    /// \brief Loose files removed by the commit.
    std::size_t loose_removed = 0;

    /// \brief \c true when a database transaction was committed.
    bool committed = false;
};

/// \brief Reconcile the database with the current archives and loose files.
///
/// \param options Scan paths, database target, and diagnostics sink.
/// \param verbose Log a summary when archives or loose files changed.
/// \param dirty Absolute archive paths reported by efsw.
/// \param reparse Paths whose entry rows must be rebuilt even when the hash is
/// unchanged (for example a VPK side archive).
/// \param force_all Rehash and reparse every candidate.
/// \param[out] summary Receives the paths and counts that were committed.
/// \param[out] error Receives the database error on failure.
/// \return \c false when the database could not be read or written.
bool refresh_watch_index(const ArchiveIndexOptions& options, bool verbose,
                         const std::set<std::string>& dirty, const std::set<std::string>& reparse,
                         bool force_all, WatchRefreshSummary& summary, std::string& error);

} // namespace sauce::fs::core::detail
