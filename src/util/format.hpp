// SPDX-License-Identifier: MIT

#pragma once

#include <cstdio>
#include <utility>

#include <fmt/format.h>

namespace sauce::fs
{

/// Write formatted text to `stream`. fmt performs the type-safe conversion and
/// writes the result directly, so call sites do not need printf format strings.
template <typename... Args>
void print_to(std::FILE* stream, fmt::format_string<Args...> format, Args&&... args)
{
    fmt::print(stream, format, std::forward<Args>(args)...);
}

/// Formatted write to stdout.
template <typename... Args> void print(fmt::format_string<Args...> format, Args&&... args)
{
    fmt::print(format, std::forward<Args>(args)...);
}

/// Formatted write to stderr.
template <typename... Args> void print_error(fmt::format_string<Args...> format, Args&&... args)
{
    fmt::print(stderr, format, std::forward<Args>(args)...);
}

} // namespace sauce::fs
