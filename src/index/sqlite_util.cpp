// SPDX-License-Identifier: MIT

#include "index/sqlite_util.hpp"

#include <sqlite3.h>

#include "index/archive_sql.hpp"

namespace sauce::fs::sqlite
{

void DatabaseCloser::operator()(sqlite3* database) const noexcept
{
    sqlite3_close(database);
}

void StatementFinalizer::operator()(sqlite3_stmt* statement) const noexcept
{
    sqlite3_finalize(statement);
}

namespace
{

std::string error_message(sqlite3* database)
{
    return database != nullptr ? sqlite3_errmsg(database) : "sqlite error";
}

void exec_if_possible(sqlite3* database, std::string_view sql_text)
{
    std::string ignored;
    exec(database, sql_text, ignored);
}

} // namespace

Database open(const std::filesystem::path& path, OpenMode mode, std::string& error)
{
    sqlite3* raw = nullptr;
    const std::u8string utf8 = path.u8string();
    const int flags = mode == OpenMode::ReadOnly ? SQLITE_OPEN_READONLY
                                                 : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    if (sqlite3_open_v2(reinterpret_cast<const char*>(utf8.c_str()), &raw, flags, nullptr) !=
        SQLITE_OK)
    {
        error = raw != nullptr ? sqlite3_errmsg(raw) : "cannot open SQLite database";
        sqlite3_close(raw);
        return {};
    }

    Database database(raw);
    sqlite3_busy_timeout(raw, 5000);
    if (mode == OpenMode::ReadWrite)
    {
        // Enforced per connection and only outside a transaction.
        if (!exec(raw, sql::kForeignKeysOn, error))
        {
            return {};
        }
        // Failure is not fatal: some filesystems (notably network shares) only
        // support rollback journalling.
        exec_if_possible(raw, sql::kJournalModeWal);
    }
    return database;
}

bool exec(sqlite3* database, std::string_view sql_text, std::string& error)
{
    char* message = nullptr;
    const std::string text(sql_text);
    if (sqlite3_exec(database, text.c_str(), nullptr, nullptr, &message) != SQLITE_OK)
    {
        error = message != nullptr ? message : error_message(database);
        sqlite3_free(message);
        return false;
    }
    sqlite3_free(message);
    return true;
}

Statement prepare(sqlite3* database, std::string_view sql_text, std::string& error)
{
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(database, sql_text.data(), static_cast<int>(sql_text.size()), &raw,
                           nullptr) != SQLITE_OK)
    {
        error = error_message(database);
        sqlite3_finalize(raw);
        return {};
    }
    return Statement(raw);
}

bool bind_text(sqlite3_stmt* statement, int index, std::string_view value) noexcept
{
    return sqlite3_bind_text(statement, index, value.empty() ? "" : value.data(),
                             static_cast<int>(value.size()), SQLITE_TRANSIENT) == SQLITE_OK;
}

bool step_done(sqlite3_stmt* statement, std::string& error)
{
    const int result = sqlite3_step(statement);
    if (result != SQLITE_DONE)
    {
        error = error_message(sqlite3_db_handle(statement));
        reset(statement);
        return false;
    }
    reset(statement);
    return true;
}

void reset(sqlite3_stmt* statement) noexcept
{
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
}

std::string column_text(sqlite3_stmt* statement, int column)
{
    const unsigned char* value = sqlite3_column_text(statement, column);
    return value != nullptr ? reinterpret_cast<const char*>(value) : "";
}

bool column_is_null(sqlite3_stmt* statement, int column) noexcept
{
    return sqlite3_column_type(statement, column) == SQLITE_NULL;
}

bool begin(sqlite3* database, std::string& error, bool immediate)
{
    return exec(database, immediate ? sql::kBeginImmediate : sql::kBegin, error);
}

bool commit(sqlite3* database, std::string& error)
{
    return exec(database, sql::kCommit, error);
}

void rollback(sqlite3* database)
{
    exec_if_possible(database, sql::kRollback);
}

} // namespace sauce::fs::sqlite
