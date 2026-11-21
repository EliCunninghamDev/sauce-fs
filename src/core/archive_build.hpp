// SPDX-License-Identifier: MIT

#pragma once

#include "core/archive_discovery.hpp"
#include "core/archive_index.hpp"

namespace sauce::fs::core
{

/// Hash and parse every candidate in `plan`, then replace the database with
/// the result. Moves the plan's games and candidates into `result`.
bool build_from_plan(const ArchiveIndexOptions& options, ScanPlan& plan,
                     ArchiveIndexResult& result);

} // namespace sauce::fs::core
