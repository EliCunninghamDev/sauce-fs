// SPDX-License-Identifier: MIT

/// \file
/// \brief Umbrella header for the sauce-fs indexing library.
///
/// Including this file pulls in the error and provider types, the index
/// construction and watch API, and the callback-based logging configuration.
/// The individual headers can also be included directly.

#pragma once

/// \brief Error, metadata, database, and provider interfaces.
#include "archive_access.hpp"

/// \brief Index construction, watch mode, and record types.
#include "archive_index.hpp"

/// \brief Callback-based diagnostics configuration.
#include "log.hpp"
