// SPDX-License-Identifier: MIT

#pragma once

#include <cstdio>
#include <mutex>
#include <string_view>

#include <fmt/format.h>

#include <sauce/core/log.hpp>

namespace sauce::fs
{

/// LogOptions that write Info/Debug to stdout and warnings/errors to stderr,
/// prefixed by level. The callback serializes writes so messages from worker
/// threads do not interleave.
inline core::LogOptions console_log(core::LogLevel level = core::LogLevel::Info)
{
    core::LogOptions options;
    options.level = level;
    options.callback = [](core::LogLevel message_level, std::string_view message)
    {
        static std::mutex mutex;
        const std::lock_guard lock(mutex);
        std::FILE* const stream = message_level >= core::LogLevel::Warning ? stderr : stdout;
        switch (message_level)
        {
        case core::LogLevel::Debug:
            fmt::print(stream, "debug: {}\n", message);
            break;
        case core::LogLevel::Info:
            fmt::print(stream, "{}\n", message);
            break;
        case core::LogLevel::Warning:
            fmt::print(stream, "warning: {}\n", message);
            break;
        case core::LogLevel::Error:
            fmt::print(stream, "error: {}\n", message);
            break;
        }
        std::fflush(stream);
    };
    return options;
}

} // namespace sauce::fs
