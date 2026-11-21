// SPDX-License-Identifier: MIT

#pragma once

#include <utility>

#include <fmt/format.h>

#include <sauce/core/log.hpp>

#if SAUCE_FS_LOGGING
/// Emit one diagnostic through the LogOptions carried by the current object.
/// The macro keeps argument evaluation out of builds configured with
/// -DSAUCE_FS_LOGGING=OFF.
#define SAUCE_FS_LOG(options, level, ...)                                                          \
    ::sauce::fs::core::detail::log((options), ::sauce::fs::core::LogLevel::level, __VA_ARGS__)
#else
#define SAUCE_FS_LOG(options, level, ...) ((void)0)
#endif

namespace sauce::fs::core::detail
{

template <typename... Args>
void log(const LogOptions& options, LogLevel level, fmt::format_string<Args...> format,
         Args&&... args)
{
#if SAUCE_FS_LOGGING
    if (!options.enabled(level))
    {
        return;
    }
    options.callback(level, fmt::format(format, std::forward<Args>(args)...));
#else
    (void)options;
    (void)level;
    (void)format;
    ((void)args, ...);
#endif
}

} // namespace sauce::fs::core::detail
