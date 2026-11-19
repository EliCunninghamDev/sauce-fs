// Ported from seeass-index's seeass_index.h.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string>

namespace sauce::fs::parsers
{

/// One file record produced by a container parser.
struct ScannedEntry
{
    std::string path;
    /// Offset relative to the container's base (BSP pakfile start, side VPK,
    /// or 0 for GMA/loose).
    std::uint64_t offset = 0;
    /// Stored bytes (excluding VPK preload).
    std::uint64_t length = 0;
    /// Logical file size.
    std::uint64_t size = 0;
    std::uint32_t crc = 0;
    bool crc_known = false;
    std::uint16_t archive = 0;
    std::uint32_t preload_length = 0;
    std::uint64_t preload_offset = 0;
    std::uint8_t method = 0;
    /// Actual-case path for loose files, when it differs from path.
    std::string locator;
};

} // namespace sauce::fs::parsers
