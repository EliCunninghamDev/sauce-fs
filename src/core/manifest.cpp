#include "core/manifest.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include <efsw/efsw.hpp>
#include <lzma.h>
#include <sqlite3.h>
#include <xxhash.h>
#include <zlib.h>

namespace sauce::fs
{
namespace
{

std::string hex(std::uint64_t value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "0x%llx", static_cast<unsigned long long>(value));
    return buffer;
}

std::uint64_t digest_crc32(const std::string& text)
{
    return crc32(0, reinterpret_cast<const Bytef*>(text.data()), static_cast<uInt>(text.size()));
}

std::uint64_t digest_xxh3(const std::string& text)
{
    return XXH3_64bits(text.data(), text.size());
}

Check check_lzma()
{
    const std::string source = "sauce-fs: lzma roundtrip";
    std::vector<std::uint8_t> encoded(lzma_stream_buffer_bound(source.size()));
    std::size_t encoded_size = 0;

    lzma_ret result = lzma_easy_buffer_encode(
        6, LZMA_CHECK_CRC64, nullptr, reinterpret_cast<const std::uint8_t*>(source.data()),
        source.size(), encoded.data(), &encoded_size, encoded.size());
    if (result != LZMA_OK)
    {
        return {"lzma", false, "encode failed (" + std::to_string(result) + ")"};
    }

    std::vector<std::uint8_t> decoded(source.size());
    std::size_t in_position = 0;
    std::size_t decoded_size = 0;
    std::uint64_t memory_limit = UINT64_MAX;

    result = lzma_stream_buffer_decode(&memory_limit, 0, nullptr, encoded.data(), &in_position,
                                       encoded_size, decoded.data(), &decoded_size, decoded.size());
    if (result != LZMA_OK || decoded_size != source.size() ||
        std::memcmp(decoded.data(), source.data(), source.size()) != 0)
    {
        return {"lzma", false, "decode mismatch"};
    }

    return {"lzma", true, std::to_string(encoded_size) + " bytes on the wire"};
}

Check check_zlib()
{
    const std::string source = "sauce-fs: zlib roundtrip";
    uLongf encoded_size = compressBound(static_cast<uLong>(source.size()));
    std::vector<Bytef> encoded(encoded_size);

    if (compress2(encoded.data(), &encoded_size, reinterpret_cast<const Bytef*>(source.data()),
                  static_cast<uLong>(source.size()), Z_DEFAULT_COMPRESSION) != Z_OK)
    {
        return {"zlib", false, "compress failed"};
    }

    std::vector<Bytef> decoded(source.size());
    uLongf decoded_size = static_cast<uLongf>(decoded.size());
    if (uncompress(decoded.data(), &decoded_size, encoded.data(), encoded_size) != Z_OK ||
        decoded_size != source.size() ||
        std::memcmp(decoded.data(), source.data(), source.size()) != 0)
    {
        return {"zlib", false, "decompress mismatch"};
    }

    return {"zlib", true, std::to_string(encoded_size) + " bytes on the wire"};
}

Check check_crc32()
{
    // Standard check value from the zlib documentation.
    const std::uint64_t value = digest_crc32("123456789");
    const bool ok = value == 0xCBF43926u;
    return {"crc32", ok, ok ? "0xcbf43926 matches" : "unexpected " + hex(value)};
}

Check check_xxhash()
{
    // Documented XXH64 test vector for an empty input.
    const std::uint64_t value = XXH64("", 0, 0);
    const bool ok = value == 0xEF46DB3751D8E999ULL;
    return {"xxhash", ok, ok ? "xxh64 vector matches" : "unexpected " + hex(value)};
}

Check check_sqlite()
{
    sqlite3* database = nullptr;
    if (sqlite3_open(":memory:", &database) != SQLITE_OK)
    {
        const std::string detail = database ? sqlite3_errmsg(database) : "open failed";
        sqlite3_close(database);
        return {"sqlite", false, detail};
    }

    Check check{"sqlite", false, "query failed"};
    if (sqlite3_exec(database, "CREATE TABLE t(v TEXT); INSERT INTO t VALUES('sauce');", nullptr,
                     nullptr, nullptr) == SQLITE_OK)
    {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(database, "SELECT v FROM t", -1, &statement, nullptr) == SQLITE_OK)
        {
            if (sqlite3_step(statement) == SQLITE_ROW)
            {
                const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
                check = {"sqlite", text != nullptr && std::strcmp(text, "sauce") == 0,
                         "in-memory roundtrip"};
            }
            sqlite3_finalize(statement);
        }
    }

    sqlite3_close(database);
    return check;
}

Check check_efsw()
{
    struct Listener : efsw::FileWatchListener
    {
        void handleFileAction(efsw::WatchID, const std::string&, const std::string&, efsw::Action,
                              const std::string&) override
        {
        }
    };

    efsw::FileWatcher watcher;
    Listener listener;
    const efsw::WatchID id = watcher.addWatch(".", &listener, false);
    if (id < 0)
    {
        return {"efsw", false, "addWatch failed"};
    }
    watcher.removeWatch(id);
    return {"efsw", true, "watcher registered and removed"};
}

} // namespace

std::string manifest()
{
    std::ostringstream body;
    body << "sauce-fs " SAUCE_FS_VERSION "\n";
    body << "liblzma " << lzma_version_string() << "\n";
    body << "zlib " << zlibVersion() << "\n";
    body << "sqlite " << sqlite3_libversion() << "\n";
    body << "xxhash " << XXH_VERSION_MAJOR << '.' << XXH_VERSION_MINOR << '.' << XXH_VERSION_RELEASE
         << "\n";
    body << "efsw linked\n";

    const std::string text = body.str();
    std::ostringstream report;
    report << text;
    report << "crc32 " << hex(digest_crc32(text)) << "\n";
    report << "xxh3 " << hex(digest_xxh3(text)) << "\n";
    return report.str();
}

std::vector<Check> self_check()
{
    return {
        check_lzma(), check_zlib(), check_crc32(), check_sqlite(), check_xxhash(), check_efsw(),
    };
}

} // namespace sauce::fs
