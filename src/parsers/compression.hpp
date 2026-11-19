// Ported from seeass-index's seeass_index.h.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sauce::fs::parsers
{

/// Inflate a raw RFC 1951 deflate stream into exactly `expected_size` bytes.
bool inflate_raw(const std::uint8_t* data, std::size_t size, std::size_t expected_size,
                 std::vector<std::uint8_t>& out, std::string* error);

/// Decode an LZMA-alone stream.  `data` points at the 13-byte properties /
/// dictionary / size header; `size` covers the header and the compressed
/// payload.  `declared_override` replaces the header size when it is unknown
/// (all-ones), which matters for `.bin` wrappers whose footer carries the
/// low 32 bits of the real length.
bool lzma_alone_decode(const std::uint8_t* data, std::size_t size, std::uint64_t declared_override,
                       std::vector<std::uint8_t>& out, std::string* error);

/// Decode a ZIP method 14 (LZMA) payload.  ZIP framing is a 4-byte header
/// (`u16` encoder version, `u16` properties size) followed by the five LZMA
/// property bytes and a raw LZMA1 stream.
bool lzma_zip_decode(const std::uint8_t* data, std::size_t size, std::uint64_t declared,
                     std::vector<std::uint8_t>& out, std::string* error);

} // namespace sauce::fs::parsers
