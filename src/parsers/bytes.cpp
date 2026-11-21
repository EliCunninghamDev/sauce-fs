// Ported from seeass-index's seeass_index.h.
// SPDX-License-Identifier: MIT

#include "bytes.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace sauce::fs::parsers
{

char ascii_lower(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool ascii_iequals(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        if (ascii_lower(a[i]) != ascii_lower(b[i]))
        {
            return false;
        }
    }
    return true;
}

bool starts_with(std::string_view text, std::string_view prefix) noexcept
{
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

bool ends_with_ci(std::string_view text, std::string_view suffix) noexcept
{
    if (text.size() < suffix.size())
    {
        return false;
    }
    return ascii_iequals(text.substr(text.size() - suffix.size()), suffix);
}

bool safe_add(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept
{
    if (a > UINT64_MAX - b)
    {
        return false;
    }
    out = a + b;
    return true;
}

bool safe_mul(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept
{
    if (a != 0 && b > UINT64_MAX / a)
    {
        return false;
    }
    out = a * b;
    return true;
}

std::string trim(std::string_view text)
{
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r' ||
                           text[begin] == '\n'))
    {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' ||
                           text[end - 1] == '\n'))
    {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

std::string normalize_path(std::string_view path)
{
    std::string out;
    out.reserve(path.size());
    bool first = true;
    std::size_t start = 0;
    while (start <= path.size())
    {
        const std::size_t slash = path.find_first_of("/\\", start);
        const std::size_t end = slash == std::string_view::npos ? path.size() : slash;
        const std::string_view part = path.substr(start, end - start);
        if (!part.empty() && part != ".")
        {
            if (!first)
            {
                out.push_back('/');
            }
            for (char c : part)
            {
                out.push_back(ascii_lower(c));
            }
            first = false;
        }
        if (slash == std::string_view::npos)
        {
            break;
        }
        start = slash + 1;
    }
    return out;
}

static const std::array<std::uint32_t, 256>& crc_table()
{
    static const std::array<std::uint32_t, 256> table = []
    {
        std::array<std::uint32_t, 256> result{};
        for (std::uint32_t i = 0; i < 256; ++i)
        {
            std::uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit)
            {
                value = (value & 1u) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
            }
            result[i] = value;
        }
        return result;
    }();
    return table;
}

std::uint32_t crc32_update(std::uint32_t crc, const void* data, std::size_t length)
{
    const auto& table = crc_table();
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    crc = ~crc;
    for (std::size_t i = 0; i < length; ++i)
    {
        crc = table[(crc ^ bytes[i]) & 0xFFu] ^ (crc >> 8);
    }
    return ~crc;
}

std::uint16_t load_le16(const std::uint8_t* data) noexcept
{
    return static_cast<std::uint16_t>(static_cast<std::uint32_t>(data[0]) |
                                      (static_cast<std::uint32_t>(data[1]) << 8));
}

std::uint32_t load_le32(const std::uint8_t* data) noexcept
{
    return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8) |
           (static_cast<std::uint32_t>(data[2]) << 16) |
           (static_cast<std::uint32_t>(data[3]) << 24);
}

std::uint64_t load_le64(const std::uint8_t* data) noexcept
{
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i)
    {
        value = (value << 8) | static_cast<std::uint64_t>(data[i]);
    }
    return value;
}

bool FileHandle::open(const std::filesystem::path& path, std::string* error)
{
    close();
    path_ = path;
#if defined(_WIN32)
    file_ = _wfopen(path.wstring().c_str(), L"rb");
#else
    file_ = std::fopen(path.c_str(), "rb");
#endif
    if (!file_)
    {
        if (error)
        {
            *error = "cannot open " + path.string() + ": " + std::strerror(errno);
        }
        return false;
    }
    if (!size_of(size_, error))
    {
        close();
        return false;
    }
    return true;
}

void FileHandle::close()
{
    if (file_)
    {
        std::fclose(file_);
        file_ = nullptr;
    }
    size_ = 0;
}

bool FileHandle::seek(std::uint64_t offset)
{
    if (!file_ || offset > static_cast<std::uint64_t>(INT64_MAX))
    {
        return false;
    }
#if defined(_WIN32)
    return _fseeki64(file_, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
    return fseeko(file_, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

bool FileHandle::read(void* destination, std::size_t length)
{
    if (!file_ || length == 0)
    {
        return true;
    }
    return std::fread(destination, 1, length, file_) == length;
}

bool FileHandle::read_at(std::uint64_t offset, void* destination, std::size_t length)
{
    return seek(offset) && read(destination, length);
}

std::uint64_t FileHandle::tell() const
{
    if (!file_)
    {
        return UINT64_MAX;
    }
#if defined(_WIN32)
    const __int64 position = _ftelli64(file_);
#else
    const off_t position = ftello(file_);
#endif
    return position < 0 ? UINT64_MAX : static_cast<std::uint64_t>(position);
}

bool FileHandle::read_string(std::string& out, std::size_t max_length, std::string* error)
{
    out.clear();
    for (;;)
    {
        const int value = std::fgetc(file_);
        if (value == EOF)
        {
            if (error)
            {
                *error = "unterminated string in " + path_.string();
            }
            return false;
        }
        if (value == 0)
        {
            return true;
        }
        if (out.size() >= max_length)
        {
            if (error)
            {
                *error = "string too long in " + path_.string();
            }
            return false;
        }
        out.push_back(static_cast<char>(value));
    }
}

bool FileHandle::size_of(std::uint64_t& result, std::string* error)
{
#if defined(_WIN32)
    if (_fseeki64(file_, 0, SEEK_END) != 0)
    {
        if (error)
        {
            *error = "cannot seek " + path_.string();
        }
        return false;
    }
    const __int64 end = _ftelli64(file_);
    if (end < 0)
    {
        if (error)
        {
            *error = "cannot size " + path_.string();
        }
        return false;
    }
    result = static_cast<std::uint64_t>(end);
    return seek(0);
#else
    if (fseeko(file_, 0, SEEK_END) != 0)
    {
        if (error)
        {
            *error = "cannot seek " + path_.string();
        }
        return false;
    }
    const off_t end = ftello(file_);
    if (end < 0)
    {
        if (error)
        {
            *error = "cannot size " + path_.string();
        }
        return false;
    }
    result = static_cast<std::uint64_t>(end);
    return seek(0);
#endif
}

bool MemoryReader::seek(std::uint64_t offset)
{
    if (offset > size_)
    {
        return false;
    }
    position_ = static_cast<std::size_t>(offset);
    return true;
}

bool MemoryReader::read(void* destination, std::size_t length)
{
    if (position_ > size_ || length > size_ - position_)
    {
        return false;
    }
    if (length != 0)
    {
        std::memcpy(destination, data_ + position_, length);
    }
    position_ += length;
    return true;
}

bool MemoryReader::read_at(std::uint64_t offset, void* destination, std::size_t length)
{
    return seek(offset) && read(destination, length);
}

bool MemoryReader::read_string(std::string& out, std::size_t max_length, std::string* error)
{
    out.clear();
    while (position_ < size_)
    {
        const std::uint8_t value = data_[position_++];
        if (value == 0)
        {
            return true;
        }
        if (out.size() >= max_length)
        {
            if (error)
            {
                *error = "string too long in archive";
            }
            return false;
        }
        out.push_back(static_cast<char>(value));
    }
    if (error)
    {
        *error = "unterminated string in archive";
    }
    return false;
}

bool read_file_range(const std::filesystem::path& path, std::uint64_t offset, std::uint64_t length,
                     std::vector<std::uint8_t>& out, std::string* error)
{
    out.clear();
    if (length > static_cast<std::uint64_t>(SIZE_MAX))
    {
        if (error)
        {
            *error = "file range is too large";
        }
        return false;
    }
    FileHandle file;
    if (!file.open(path, error))
    {
        return false;
    }
    std::uint64_t end = 0;
    if (!safe_add(offset, length, end) || end > file.size())
    {
        if (error)
        {
            *error = "file range " + std::to_string(offset) + "+" + std::to_string(length) +
                     " is outside " + path.string() + " (" + std::to_string(file.size()) + ")";
        }
        return false;
    }
    try
    {
        out.resize(static_cast<std::size_t>(length));
    }
    catch (const std::bad_alloc&)
    {
        if (error)
        {
            *error = "out of memory reading " + path.string();
        }
        return false;
    }
    if (length != 0 && !file.read_at(offset, out.data(), out.size()))
    {
        if (error)
        {
            *error = "short read from " + path.string();
        }
        out.clear();
        return false;
    }
    return true;
}

bool Cursor::skip(std::size_t count)
{
    if (!ok_ || count > remaining())
    {
        ok_ = false;
        return false;
    }
    position_ += count;
    return true;
}

bool Cursor::bytes(std::size_t count, const std::uint8_t*& out)
{
    if (!ok_ || count > remaining())
    {
        ok_ = false;
        return false;
    }
    out = data_ + position_;
    position_ += count;
    return true;
}

bool Cursor::u8(std::uint8_t& value)
{
    const std::uint8_t* p = nullptr;
    if (!bytes(1, p))
    {
        return false;
    }
    value = p[0];
    return true;
}

bool Cursor::u16(std::uint16_t& value)
{
    const std::uint8_t* p = nullptr;
    if (!bytes(2, p))
    {
        return false;
    }
    value = load_le16(p);
    return true;
}

bool Cursor::u32(std::uint32_t& value)
{
    const std::uint8_t* p = nullptr;
    if (!bytes(4, p))
    {
        return false;
    }
    value = load_le32(p);
    return true;
}

bool Cursor::u64(std::uint64_t& value)
{
    std::uint32_t lo = 0;
    std::uint32_t hi = 0;
    if (!u32(lo) || !u32(hi))
    {
        return false;
    }
    value = static_cast<std::uint64_t>(lo) | (static_cast<std::uint64_t>(hi) << 32);
    return true;
}

bool Cursor::i32(std::int32_t& value)
{
    std::uint32_t raw = 0;
    if (!u32(raw))
    {
        return false;
    }
    value = static_cast<std::int32_t>(raw);
    return true;
}

bool Cursor::i64(std::int64_t& value)
{
    std::uint64_t raw = 0;
    if (!u64(raw))
    {
        return false;
    }
    value = static_cast<std::int64_t>(raw);
    return true;
}

bool Cursor::string(std::string& out, std::size_t max_length)
{
    const std::size_t begin = position_;
    while (position_ < size_ && data_[position_] != 0)
    {
        ++position_;
    }
    if (position_ >= size_)
    {
        ok_ = false;
        return false;
    }
    if (position_ - begin > max_length)
    {
        ok_ = false;
        return false;
    }
    out.assign(reinterpret_cast<const char*>(data_ + begin), position_ - begin);
    ++position_;
    return true;
}

bool Cursor::sized_string(std::string& out, std::size_t max_length)
{
    std::uint32_t length = 0;
    if (!u32(length) || length > max_length)
    {
        ok_ = false;
        return false;
    }
    const std::uint8_t* p = nullptr;
    if (!bytes(length, p))
    {
        ok_ = false;
        return false;
    }
    out.assign(reinterpret_cast<const char*>(p), length);
    return true;
}

void Writer::u16(std::uint16_t value)
{
    data_.push_back(static_cast<std::uint8_t>(value & 0xFF));
    data_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void Writer::u32(std::uint32_t value)
{
    for (int i = 0; i < 4; ++i)
    {
        data_.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

void Writer::u64(std::uint64_t value)
{
    for (int i = 0; i < 8; ++i)
    {
        data_.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

void Writer::bytes(const void* data, std::size_t length)
{
    const auto* p = static_cast<const std::uint8_t*>(data);
    data_.insert(data_.end(), p, p + length);
}

void Writer::string(std::string_view value)
{
    u32(static_cast<std::uint32_t>(value.size()));
    bytes(value.data(), value.size());
}

} // namespace sauce::fs::parsers
