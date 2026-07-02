// Database.cpp
// SQLite database wrapper implementation

#include "db/Database.hpp"
#include <sqlite3.h>
#include <iostream>

namespace gitreview {
namespace db {

// ============================================================================
// Database
// ============================================================================

Database::Database(const std::string& path) {
    int rc = sqlite3_open(path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to open database: " << sqlite3_errmsg(db_) << std::endl;
        sqlite3_close(db_);
        db_ = nullptr;
    }

    // Enable WAL mode for better concurrency
    if (db_) {
        execute("PRAGMA journal_mode=WAL");
        execute("PRAGMA foreign_keys=ON");
    }
}

Database::~Database() {
    if (db_) {
        sqlite3_close(db_);
    }
}

Database::Database(Database&& other) noexcept : db_(other.db_) {
    other.db_ = nullptr;
}

Database& Database::operator=(Database&& other) noexcept {
    if (this != &other) {
        if (db_) {
            sqlite3_close(db_);
        }
        db_ = other.db_;
        other.db_ = nullptr;
    }
    return *this;
}

bool Database::isOpen() const {
    return db_ != nullptr;
}

bool Database::execute(const std::string& sql) {
    if (!db_) return false;

    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errMsg);

    if (rc != SQLITE_OK) {
        std::cerr << "SQL error: " << (errMsg ? errMsg : "unknown") << std::endl;
        sqlite3_free(errMsg);
        return false;
    }

    return true;
}

std::unique_ptr<Statement> Database::prepare(const std::string& sql) {
    if (!db_) return nullptr;
    return std::make_unique<Statement>(db_, sql);
}

int64_t Database::lastInsertRowId() const {
    if (!db_) return 0;
    return sqlite3_last_insert_rowid(db_);
}

int Database::changesCount() const {
    if (!db_) return 0;
    return sqlite3_changes(db_);
}

std::string Database::lastError() const {
    if (!db_) return "Database not open";
    return sqlite3_errmsg(db_);
}

bool Database::beginTransaction() {
    return execute("BEGIN TRANSACTION");
}

bool Database::commit() {
    return execute("COMMIT");
}

bool Database::rollback() {
    return execute("ROLLBACK");
}

// ============================================================================
// Statement
// ============================================================================

Statement::Statement(sqlite3* db, const std::string& sql) {
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt_, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(db) << std::endl;
        stmt_ = nullptr;
    }
}

Statement::~Statement() {
    if (stmt_) {
        sqlite3_finalize(stmt_);
    }
}

bool Statement::bindInt(int index, int value) {
    if (!stmt_) return false;
    return sqlite3_bind_int(stmt_, index, value) == SQLITE_OK;
}

bool Statement::bindInt64(int index, int64_t value) {
    if (!stmt_) return false;
    return sqlite3_bind_int64(stmt_, index, value) == SQLITE_OK;
}

bool Statement::bindDouble(int index, double value) {
    if (!stmt_) return false;
    return sqlite3_bind_double(stmt_, index, value) == SQLITE_OK;
}

bool Statement::bindText(int index, const std::string& value) {
    if (!stmt_) return false;
    return sqlite3_bind_text(stmt_, index, value.c_str(),
                             static_cast<int>(value.size()), SQLITE_TRANSIENT) == SQLITE_OK;
}

bool Statement::bindBlob(int index, const void* data, int size) {
    if (!stmt_) return false;
    return sqlite3_bind_blob(stmt_, index, data, size, SQLITE_TRANSIENT) == SQLITE_OK;
}

bool Statement::bindNull(int index) {
    if (!stmt_) return false;
    return sqlite3_bind_null(stmt_, index) == SQLITE_OK;
}

bool Statement::step() {
    if (!stmt_) return false;
    int rc = sqlite3_step(stmt_);
    return rc == SQLITE_ROW;
}

bool Statement::reset() {
    if (!stmt_) return false;
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
    return true;
}

int Statement::columnInt(int index) {
    if (!stmt_) return 0;
    return sqlite3_column_int(stmt_, index);
}

int64_t Statement::columnInt64(int index) {
    if (!stmt_) return 0;
    return sqlite3_column_int64(stmt_, index);
}

double Statement::columnDouble(int index) {
    if (!stmt_) return 0.0;
    return sqlite3_column_double(stmt_, index);
}

std::string Statement::columnText(int index) {
    if (!stmt_) return "";
    const unsigned char* text = sqlite3_column_text(stmt_, index);
    if (!text) return "";
    return reinterpret_cast<const char*>(text);
}

std::vector<uint8_t> Statement::columnBlob(int index) {
    if (!stmt_) return {};
    const void* data = sqlite3_column_blob(stmt_, index);
    int size = sqlite3_column_bytes(stmt_, index);
    if (!data || size <= 0) return {};

    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    return std::vector<uint8_t>(bytes, bytes + size);
}

bool Statement::columnIsNull(int index) {
    if (!stmt_) return true;
    return sqlite3_column_type(stmt_, index) == SQLITE_NULL;
}

int Statement::columnCount() const {
    if (!stmt_) return 0;
    return sqlite3_column_count(stmt_);
}

std::string Statement::columnName(int index) const {
    if (!stmt_) return "";
    const char* name = sqlite3_column_name(stmt_, index);
    return name ? name : "";
}

} // namespace db
} // namespace gitreview
