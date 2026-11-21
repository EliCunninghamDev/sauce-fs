// Ported from seeass-index's seeass_index.h.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace sauce::fs::parsers
{

// ---------------------------------------------------------------------------
// Small utilities
// ---------------------------------------------------------------------------

char ascii_lower(char c) noexcept;

bool ascii_iequals(std::string_view a, std::string_view b) noexcept;

bool starts_with(std::string_view text, std::string_view prefix) noexcept;

bool ends_with_ci(std::string_view text, std::string_view suffix) noexcept;

bool safe_add(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept;

bool safe_mul(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept;

std::string trim(std::string_view text);

/// Lowercase, convert '\' to '/', collapse separators, and strip leading
/// slashes and "./" prefixes.
std::string normalize_path(std::string_view path);

// ---------------------------------------------------------------------------
// CRC-32 (IEEE, same polynomial used by VPK, GMA, and ZIP)
// ---------------------------------------------------------------------------

std::uint32_t crc32_update(std::uint32_t crc, const void* data, std::size_t length);

// ---------------------------------------------------------------------------
// Little-endian loads
// ---------------------------------------------------------------------------

std::uint16_t load_le16(const std::uint8_t* data) noexcept;

std::uint32_t load_le32(const std::uint8_t* data) noexcept;

std::uint64_t load_le64(const std::uint8_t* data) noexcept;

// ---------------------------------------------------------------------------
// File access
// ---------------------------------------------------------------------------

class FileHandle
{
public:
    FileHandle() = default;
    ~FileHandle()
    {
        close();
    }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    bool open(const std::filesystem::path& path, std::string* error);

    void close();

    bool valid() const
    {
        return file_ != nullptr;
    }
    std::uint64_t size() const
    {
        return size_;
    }

    bool seek(std::uint64_t offset);

    bool read(void* destination, std::size_t length);

    bool read_at(std::uint64_t offset, void* destination, std::size_t length);

    /// Current absolute position, or UINT64_MAX when unavailable.
    std::uint64_t tell() const;

    /// Read a NUL-terminated string one byte at a time.  GMA and gameinfo
    /// strings are small; avoiding a large fgets buffer keeps the cursor exact.
    bool read_string(std::string& out, std::size_t max_length, std::string* error);

private:
    bool size_of(std::uint64_t& result, std::string* error);

    std::FILE* file_ = nullptr;
    std::filesystem::path path_;
    std::uint64_t size_ = 0;
};

/// A read-only cursor over an in-memory archive.  Mirrors the subset of
/// FileHandle used by the GMA parser so legacy `.bin` payloads can be parsed
/// after decompression without spilling them to disk.
class MemoryReader
{
public:
    MemoryReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    std::uint64_t size() const noexcept
    {
        return size_;
    }
    std::uint64_t tell() const noexcept
    {
        return position_;
    }

    bool seek(std::uint64_t offset);

    bool read(void* destination, std::size_t length);

    bool read_at(std::uint64_t offset, void* destination, std::size_t length);

    bool read_string(std::string& out, std::size_t max_length, std::string* error);

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t position_ = 0;
};

bool read_file_range(const std::filesystem::path& path, std::uint64_t offset, std::uint64_t length,
                     std::vector<std::uint8_t>& out, std::string* error);

// ---------------------------------------------------------------------------
// Byte cursors and little-endian I/O
// ---------------------------------------------------------------------------

class Cursor
{
public:
    Cursor(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    std::size_t position() const
    {
        return position_;
    }
    std::size_t remaining() const
    {
        return size_ - position_;
    }
    bool ok() const
    {
        return ok_;
    }

    bool skip(std::size_t count);

    bool bytes(std::size_t count, const std::uint8_t*& out);

    bool u8(std::uint8_t& value);

    bool u16(std::uint16_t& value);

    bool u32(std::uint32_t& value);

    bool u64(std::uint64_t& value);

    bool i32(std::int32_t& value);

    bool i64(std::int64_t& value);

    bool string(std::string& out, std::size_t max_length = 1u << 20);

    /// Length-prefixed string used by the snapshot format.
    bool sized_string(std::string& out, std::size_t max_length = 1u << 20);

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t position_ = 0;
    bool ok_ = true;
};

class Writer
{
public:
    void u8(std::uint8_t value)
    {
        data_.push_back(value);
    }

    void u16(std::uint16_t value);

    void u32(std::uint32_t value);

    void u64(std::uint64_t value);

    void bytes(const void* data, std::size_t length);

    void string(std::string_view value);

    std::vector<std::uint8_t>& data()
    {
        return data_;
    }
    const std::vector<std::uint8_t>& data() const
    {
        return data_;
    }

private:
    std::vector<std::uint8_t> data_;
};

} // namespace sauce::fs::parsers
