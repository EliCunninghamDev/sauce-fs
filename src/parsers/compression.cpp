// Ported from seeass-index's seeass_index.h.
// SPDX-License-Identifier: MIT

#include "compression.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <vector>

#include <lzma.h>
#include <zlib.h>

#include "bytes.hpp"

namespace sauce::fs::parsers
{

namespace
{

void set_error(std::string* error, const char* message)
{
    if (error != nullptr)
    {
        *error = message;
    }
}

const char* lzma_error_text(lzma_ret result, const char* fallback)
{
    switch (result)
    {
    case LZMA_MEM_ERROR:
        return "out of memory in the LZMA decoder";
    case LZMA_OPTIONS_ERROR:
        return "unsupported LZMA options";
    case LZMA_DATA_ERROR:
        return "corrupt LZMA stream";
    case LZMA_BUF_ERROR:
        return "truncated LZMA stream";
    default:
        return fallback;
    }
}

/// Releases the liblzma decoder on every return path.
struct LzmaStream
{
    lzma_stream stream = LZMA_STREAM_INIT;

    ~LzmaStream()
    {
        lzma_end(&stream);
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Raw deflate (RFC 1951) for ZIP entries in BSP pakfiles
// ---------------------------------------------------------------------------

bool inflate_raw(const std::uint8_t* data, std::size_t size, std::size_t expected_size,
                 std::vector<std::uint8_t>& out, std::string* error)
{
    if (expected_size == SIZE_MAX)
    {
        set_error(error, "deflate output is too large");
        return false;
    }

    out.clear();
    try
    {
        // Keep one byte of slack so a stream that decodes to more than the
        // declared size is detected instead of silently truncated.
        out.resize(expected_size + 1);
    }
    catch (const std::bad_alloc&)
    {
        set_error(error, "out of memory inflating deflate stream");
        return false;
    }

    z_stream stream{};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
    {
        set_error(error, "failed to initialize the deflate decoder");
        return false;
    }
    struct Guard
    {
        z_stream* stream;

        ~Guard()
        {
            inflateEnd(stream);
        }
    } guard{&stream};

    // zlib's counters are 32-bit, so feed input and drain output in chunks.
    std::uint8_t* const target = out.data();
    const std::size_t target_size = out.size();
    std::size_t input_position = 0;
    std::size_t output_capacity = 0;

    for (;;)
    {
        if (stream.avail_in == 0 && input_position < size)
        {
            const std::size_t chunk = std::min<std::size_t>(size - input_position, UINT_MAX);
            stream.next_in = const_cast<Bytef*>(data + input_position);
            stream.avail_in = static_cast<uInt>(chunk);
            input_position += chunk;
        }
        if (stream.avail_out == 0 && output_capacity < target_size)
        {
            const std::size_t chunk =
                std::min<std::size_t>(target_size - output_capacity, UINT_MAX);
            stream.next_out = target + output_capacity;
            stream.avail_out = static_cast<uInt>(chunk);
            output_capacity += chunk;
        }

        const int result = inflate(&stream, Z_NO_FLUSH);
        const std::size_t produced = output_capacity - stream.avail_out;
        if (result == Z_STREAM_END)
        {
            if (produced != expected_size)
            {
                set_error(error, "deflate output size mismatch");
                return false;
            }
            out.resize(expected_size);
            return true;
        }
        if (result != Z_OK)
        {
            set_error(error, result == Z_DATA_ERROR ? "invalid deflate stream"
                                                    : "truncated deflate stream");
            return false;
        }
        if (produced > expected_size)
        {
            set_error(error, "deflate output exceeds declared size");
            return false;
        }
        if (stream.avail_in == 0 && input_position == size)
        {
            set_error(error, "truncated deflate stream");
            return false;
        }
    }
}

// ---------------------------------------------------------------------------
// LZMA1 (LZMA-alone) for Garry's Mod *_legacy.bin Workshop containers
// ---------------------------------------------------------------------------

bool lzma_alone_decode(const std::uint8_t* data, std::size_t size, std::uint64_t declared_override,
                       std::vector<std::uint8_t>& out, std::string* error)
{
    static constexpr std::size_t kHeaderSize = 13;
    if (size < kHeaderSize)
    {
        set_error(error, "truncated LZMA header");
        return false;
    }
    const std::uint8_t properties = data[0];
    if (properties >= 9 * 5 * 5)
    {
        set_error(error, "invalid LZMA properties byte");
        return false;
    }
    const unsigned lc = properties % 9;
    const unsigned rest = properties / 9;
    const unsigned lp = rest % 5;
    const unsigned pb = rest / 5;
    if (lc + lp > LZMA_LCLP_MAX)
    {
        set_error(error, "LZMA literal context is too large");
        return false;
    }
    std::uint64_t declared = load_le64(data + 5);
    if (declared_override != UINT64_MAX)
    {
        declared = declared_override;
    }
    const bool known_size = declared != UINT64_MAX;
    if (known_size && declared > static_cast<std::uint64_t>(SIZE_MAX))
    {
        set_error(error, "LZMA output is too large");
        return false;
    }

    out.clear();
    if (known_size && declared == 0)
    {
        return true;
    }

    lzma_options_lzma options{};
    options.lc = lc;
    options.lp = lp;
    options.pb = pb;
    options.dict_size = load_le32(data + 1);
    if (known_size)
    {
        // Matches need a dictionary no larger than the output itself.
        const std::uint64_t limit =
            std::max<std::uint64_t>(declared, static_cast<std::uint64_t>(LZMA_DICT_SIZE_MIN));
        if (static_cast<std::uint64_t>(options.dict_size) > limit && limit <= UINT32_MAX)
        {
            options.dict_size = static_cast<std::uint32_t>(limit);
        }
        if (options.dict_size < LZMA_DICT_SIZE_MIN)
        {
            options.dict_size = LZMA_DICT_SIZE_MIN;
        }

        options.ext_flags = LZMA_LZMA1EXT_ALLOW_EOPM;
        options.ext_size_low = static_cast<std::uint32_t>(declared);
        options.ext_size_high = static_cast<std::uint32_t>(declared >> 32);
    }
    else
    {
        // 0 in an LZMA-alone header means "unspecified"; liblzma needs a real
        // dictionary, so fall back to a common encoder default.
        if (options.dict_size == 0)
        {
            options.dict_size = 8u << 20;
        }
        if (options.dict_size < LZMA_DICT_SIZE_MIN)
        {
            options.dict_size = LZMA_DICT_SIZE_MIN;
        }

        options.ext_flags = LZMA_LZMA1EXT_ALLOW_EOPM;
        options.ext_size_low = UINT32_MAX;
        options.ext_size_high = UINT32_MAX;
    }

    lzma_filter filters[2] = {
        {LZMA_FILTER_LZMA1EXT, &options},
        {LZMA_VLI_UNKNOWN, nullptr},
    };
    LzmaStream decoder;
    lzma_ret result = lzma_raw_decoder(&decoder.stream, filters);
    if (result != LZMA_OK)
    {
        set_error(error, lzma_error_text(result, "failed to initialize the LZMA decoder"));
        return false;
    }

    decoder.stream.next_in = const_cast<std::uint8_t*>(data + kHeaderSize);
    decoder.stream.avail_in = size - kHeaderSize;

    if (known_size)
    {
        const std::size_t output_size = static_cast<std::size_t>(declared);
        try
        {
            out.resize(output_size);
        }
        catch (const std::bad_alloc&)
        {
            set_error(error, "out of memory reserving LZMA output");
            return false;
        }

        decoder.stream.next_out = out.data();
        decoder.stream.avail_out = output_size;
        for (;;)
        {
            result = lzma_code(&decoder.stream, LZMA_RUN);
            if (result == LZMA_STREAM_END)
            {
                break;
            }
            if (result != LZMA_OK)
            {
                set_error(error, lzma_error_text(result, "LZMA decoder error"));
                return false;
            }
            if (decoder.stream.total_out != declared)
            {
                if (decoder.stream.avail_in == 0)
                {
                    set_error(error, "truncated LZMA stream");
                    return false;
                }
                continue;
            }
            break;
        }
        if (decoder.stream.total_out != declared)
        {
            set_error(error, "LZMA output size mismatch");
            return false;
        }
        return true;
    }

    // Unknown output size: grow in fixed chunks until the end marker.
    static constexpr std::size_t kChunkSize = 64 * 1024;
    static constexpr std::size_t kMaxOutput = std::size_t{8} << 30;
    std::vector<std::uint8_t> chunk(kChunkSize);
    for (;;)
    {
        decoder.stream.next_out = chunk.data();
        decoder.stream.avail_out = chunk.size();
        result = lzma_code(&decoder.stream, LZMA_RUN);
        const std::size_t produced = chunk.size() - decoder.stream.avail_out;
        out.insert(out.end(), chunk.begin(), chunk.begin() + produced);
        if (result == LZMA_STREAM_END)
        {
            return true;
        }
        if (result != LZMA_OK)
        {
            set_error(error, lzma_error_text(result, "LZMA decoder error"));
            return false;
        }
        if (out.size() > kMaxOutput)
        {
            set_error(error, "LZMA output is too large");
            return false;
        }
        if (produced == 0 && decoder.stream.avail_in == 0)
        {
            set_error(error, "truncated LZMA stream");
            return false;
        }
    }
}

// ---------------------------------------------------------------------------
// ZIP method 14 (LZMA) framing
// ---------------------------------------------------------------------------

/// Decode a ZIP method 14 (LZMA) payload.  ZIP framing is a 4-byte header
/// (`u16` encoder version, `u16` properties size) followed by the five LZMA
/// property bytes and a raw LZMA1 stream.  That is the LZMA-alone header minus
/// the 8-byte uncompressed size, which ZIP stores in its own directory, so
/// rebuild the 13-byte header and reuse the shared decoder.
///
/// The ZIP stream may or may not carry an end-of-stream marker (general
/// purpose flag bit 1); the size from the ZIP directory is authoritative.
bool lzma_zip_decode(const std::uint8_t* data, std::size_t size, std::uint64_t declared,
                     std::vector<std::uint8_t>& out, std::string* error)
{
    static constexpr std::size_t kZipHeaderSize = 4;
    static constexpr std::size_t kPropertiesSize = 5;
    static constexpr std::size_t kAloneHeaderSize = 13;
    if (size < kZipHeaderSize)
    {
        set_error(error, "truncated ZIP LZMA header");
        return false;
    }
    const std::uint16_t properties_size = load_le16(data + 2);
    if (properties_size != kPropertiesSize)
    {
        if (error != nullptr)
        {
            *error = "unsupported ZIP LZMA properties size " + std::to_string(properties_size);
        }
        return false;
    }
    if (size < kZipHeaderSize + kPropertiesSize)
    {
        set_error(error, "truncated ZIP LZMA properties");
        return false;
    }
    const std::size_t stream_offset = kZipHeaderSize + kPropertiesSize;
    if (declared == 0)
    {
        out.clear();
        return true;
    }
    std::vector<std::uint8_t> alone;
    alone.reserve(kAloneHeaderSize + (size - stream_offset));
    // Five property bytes: lc/lp/pb followed by the dictionary size.
    alone.insert(alone.end(), data + kZipHeaderSize, data + kZipHeaderSize + kPropertiesSize);
    for (unsigned shift = 0; shift < 64; shift += 8)
    {
        alone.push_back(static_cast<std::uint8_t>((declared >> shift) & 0xFFu));
    }
    alone.insert(alone.end(), data + stream_offset, data + size);
    return lzma_alone_decode(alone.data(), alone.size(), declared, out, error);
}

} // namespace sauce::fs::parsers
