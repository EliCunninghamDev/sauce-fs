// SPDX-License-Identifier: MIT

/// \file
/// \brief Callback-based diagnostics configuration for the index library.
///
/// The library never writes to stdout or stderr. Callers opt into diagnostics
/// by installing a callback on one of the option structs that embed
/// LogOptions.

#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

/// \brief Compile-time switch for library diagnostics.
///
/// The build sets this from the SAUCE_FS_LOGGING CMake option; callers may also
/// define it themselves. A disabled build keeps the LogOptions API but drops
/// every message before it is formatted.
#ifndef SAUCE_FS_LOGGING
#define SAUCE_FS_LOGGING 1
#endif

namespace sauce::fs::core
{

/// \brief Severity of one diagnostic message.
///
/// The ordering is Debug < Info < Warning < Error, so a LogOptions level acts
/// as a minimum.
enum class LogLevel : std::uint8_t
{
    /// \brief Per-archive detail, useful when diagnosing a specific scan.
    Debug = 0,
    /// \brief Build progress, summaries, and watch-mode updates.
    Info = 1,
    /// \brief One archive or file was skipped; the rest of the index is valid.
    Warning = 2,
    /// \brief The index could not be built or the database could not be
    /// written.
    Error = 3,
};

/// \brief Receives one formatted message without a trailing newline.
///
/// The callback's first argument is the message severity and the second is the
/// formatted text. The library may call it from worker threads, so a callback
/// that touches shared state must synchronize.
using LogCallback = std::function<void(LogLevel level, std::string_view message)>;

/// \brief Diagnostics sink embedded in the library option structs.
///
/// \details Embedders either leave the callback empty (silent) or install one;
/// the bundled command-line tools install a callback that prints to the
/// terminal. Messages below \ref LogOptions::level are dropped before they are
/// formatted.
struct LogOptions
{
    /// \brief Destination for emitted messages.
    ///
    /// An empty callback disables all output.
    LogCallback callback;

    /// \brief Minimum severity that reaches \ref LogOptions::callback.
    LogLevel level = LogLevel::Info;

    /// \brief Check whether a message at \p message_level would be delivered.
    ///
    /// \param message_level Severity of the candidate message.
    /// \return \c true when a callback is installed, \p message_level is at or
    /// above \ref LogOptions::level, and diagnostics are compiled in.
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
