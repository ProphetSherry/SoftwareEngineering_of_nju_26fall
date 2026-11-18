#ifndef EXP3_INCLUDE_CAMPUS_DATABASE_H_
#define EXP3_INCLUDE_CAMPUS_DATABASE_H_

#include <sqlite3.h>

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace campus {

using Row = std::map<std::string, std::string>;
using Rows = std::vector<Row>;
using Params = std::vector<std::string>;

class Error : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// A connection belongs to one service instance; concurrent workers use separate
// connections. BEGIN IMMEDIATE serializes all mutating domain operations.
class Database {
 public:
  explicit Database(const std::string& path);
  ~Database();
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;
  void Execute(const std::string& sql, const Params& parameters = {});
  Rows Query(const std::string& sql, const Params& parameters = {});
  Row One(const std::string& sql, const Params& parameters = {});
  std::string Scalar(const std::string& sql, const Params& parameters = {});
  std::string LastId() const;
  void Backup(const std::string& destination);
  void CheckIntegrity();

 private:
  sqlite3* handle_ = nullptr;
  void Initialize();
};

class Transaction {
 public:
  explicit Transaction(Database& database);
  ~Transaction();
  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;
  void Commit();

 private:
  Database& database_;
  bool committed_ = false;
};

std::string Value(const Row& row, const std::string& key);
std::int64_t Number(const Row& row, const std::string& key);
std::int64_t Now();
std::string TimeText(std::int64_t timestamp);
std::int64_t ParseTime(const std::string& text);
std::string RequestId();
std::string CsvCell(const std::string& text);
std::string RoleText(const std::string& role);
std::string StatusText(const std::string& status);

}  // namespace campus
#endif  // EXP3_INCLUDE_CAMPUS_DATABASE_H_
