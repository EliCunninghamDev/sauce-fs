// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

struct sqlite3;
struct sqlite3_stmt;

/// Thin RAII helpers over the SQLite C API. They keep handle ownership and the
/// prepare/bind/step/reset cycle in one place; SQL text lives in archive_sql.hpp.
namespace sauce::fs::sqlite
{

struct DatabaseCloser
{
    void operator()(sqlite3* database) const noexcept;
};

struct StatementFinalizer
{
    void operator()(sqlite3_stmt* statement) const noexcept;
};

using Database = std::unique_ptr<sqlite3, DatabaseCloser>;
using Statement = std::unique_ptr<sqlite3_stmt, StatementFinalizer>;

enum class OpenMode
{
    ReadOnly,
    ReadWrite,
};

/// Open a database and apply the pragmas needed by the index: a busy timeout,
/// plus foreign keys and WAL for read/write connections.
Database open(const std::filesystem::path& path, OpenMode mode, std::string& error);

bool exec(sqlite3* database, std::string_view sql_text, std::string& error);

Statement prepare(sqlite3* database, std::string_view sql_text, std::string& error);

/// Bind a string as SQLITE_TRANSIENT. Empty strings bind as "" rather than NULL.
bool bind_text(sqlite3_stmt* statement, int index, std::string_view value) noexcept;

/// Step a statement that must produce SQLITE_DONE, then reset it.
bool step_done(sqlite3_stmt* statement, std::string& error);

void reset(sqlite3_stmt* statement) noexcept;

std::string column_text(sqlite3_stmt* statement, int column);
bool column_is_null(sqlite3_stmt* statement, int column) noexcept;

bool begin(sqlite3* database, std::string& error, bool immediate = false);
bool commit(sqlite3* database, std::string& error);
void rollback(sqlite3* database);

} // namespace sauce::fs::sqlite
