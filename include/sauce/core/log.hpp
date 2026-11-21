// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

/// Compile-time switch for library diagnostics. The build sets this from the
/// SAUCE_FS_LOGGING CMake option; callers may also define it themselves.
/// A disabled build keeps the LogOptions API but drops every message before it
/// is formatted.
#ifndef SAUCE_FS_LOGGING
#define SAUCE_FS_LOGGING 1
#endif

namespace sauce::fs::core
{

/// Severity ordering: Debug < Info < Warning < Error.
enum class LogLevel : std::uint8_t
{
    Debug = 0,
    Info = 1,
    Warning = 2,
    Error = 3,
};

/// Receives one formatted message without a trailing newline. The library may
/// call it from worker threads, so a callback that touches shared state must
/// synchronize.
using LogCallback = std::function<void(LogLevel level, std::string_view message)>;

/// Diagnostics configuration embedded in the library option structs.
///
/// The library never writes to stdout or stderr on its own. Embedders either
/// leave the callback empty (silent) or install one; the bundled command-line
/// tools install a callback that prints to the terminal.
struct LogOptions
{
    LogCallback callback;
    /// Messages below this level are dropped before formatting.
    LogLevel level = LogLevel::Info;

    bool enabled(LogLevel message_level) const noexcept
    {
#if SAUCE_FS_LOGGING
        return callback &&
               static_cast<std::uint8_t>(message_level) >= static_cast<std::uint8_t>(level);
#else
        (void)message_level;
        return false;
#endif
    }
};

} // namespace sauce::fs::core
