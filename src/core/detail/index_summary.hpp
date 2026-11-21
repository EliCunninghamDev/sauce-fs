// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>

#include <sauce/core/archive_index.hpp>
#include <sauce/core/log.hpp>

namespace sauce::fs::core::detail
{

/// Report a completed scan through the caller's log callback. Shared by the
/// one-shot build and the watcher's initial pass.
void log_index_summary(const LogOptions& log, const ArchiveIndexResult& result,
                       const std::filesystem::path& database);

} // namespace sauce::fs::core::detail
