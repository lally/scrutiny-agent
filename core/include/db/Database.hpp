// Database.hpp
// SQLite database wrapper for LSP caching

#ifndef DATABASE_HPP
#define DATABASE_HPP

#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <functional>

// Forward declare sqlite3 to avoid including the header here
struct sqlite3;
struct sqlite3_stmt;

namespace gitreview {
namespace db {

class Statement;

class Database {
public:
    explicit Database(const std::string& path);
    ~Database();

    // Non-copyable
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Movable
    Database(Database&&) noexcept;
    Database& operator=(Database&&) noexcept;

    bool isOpen() const;
    bool execute(const std::string& sql);
    std::unique_ptr<Statement> prepare(const std::string& sql);

    int64_t lastInsertRowId() const;
    int changesCount() const;
    std::string lastError() const;

    // Transaction support
    bool beginTransaction();
    bool commit();
    bool rollback();

private:
    sqlite3* db_ = nullptr;
};

class Statement {
public:
    Statement(sqlite3* db, const std::string& sql);
    ~Statement();

    // Non-copyable
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    bool isValid() const { return stmt_ != nullptr; }

    // Binding parameters (1-indexed)
    bool bindInt(int index, int value);
    bool bindInt64(int index, int64_t value);
    bool bindDouble(int index, double value);
    bool bindText(int index, const std::string& value);
    bool bindBlob(int index, const void* data, int size);
    bool bindNull(int index);

    // Execution
    bool step();  // Returns true if there's a row, false if done
    bool reset();

    // Getting column values (0-indexed)
    int columnInt(int index);
    int64_t columnInt64(int index);
    double columnDouble(int index);
    std::string columnText(int index);
    std::vector<uint8_t> columnBlob(int index);
    bool columnIsNull(int index);

    int columnCount() const;
    std::string columnName(int index) const;

private:
    sqlite3_stmt* stmt_ = nullptr;
};

} // namespace db
} // namespace gitreview

#endif // DATABASE_HPP
