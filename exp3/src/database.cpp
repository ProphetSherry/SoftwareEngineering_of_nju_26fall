#include "campus/database.h"

#include <atomic>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace campus {
namespace {
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

Statement Prepare(sqlite3* handle, const std::string& sql, const Params& parameters) {
  sqlite3_stmt* raw = nullptr;
  if (sqlite3_prepare_v2(handle, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK) {
    throw Error(sqlite3_errmsg(handle));
  }
  Statement statement(raw, sqlite3_finalize);
  if (sqlite3_bind_parameter_count(raw) != static_cast<int>(parameters.size())) {
    throw Error("SQL parameter count mismatch");
  }
  for (std::size_t i = 0; i < parameters.size(); ++i) {
    if (sqlite3_bind_text(raw, static_cast<int>(i + 1), parameters[i].c_str(), -1,
                          SQLITE_TRANSIENT) != SQLITE_OK) {
      throw Error(sqlite3_errmsg(handle));
    }
  }
  return statement;
}
}  // namespace

Database::Database(const std::string& path) {
  if (sqlite3_open_v2(path.c_str(), &handle_,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                      nullptr) != SQLITE_OK) {
    const std::string message = handle_ ? sqlite3_errmsg(handle_) : "Open failed";
    if (handle_) {
      sqlite3_close(handle_);
    }
    throw Error(message);
  }
  try {
    sqlite3_busy_timeout(handle_, 10000);
    Execute("PRAGMA foreign_keys=ON");
    Execute("PRAGMA journal_mode=WAL");
    Initialize();
  } catch (...) {
    sqlite3_close(handle_);
    throw;
  }
}

Database::~Database() { sqlite3_close(handle_); }

void Database::Execute(const std::string& sql, const Params& parameters) {
  auto statement = Prepare(handle_, sql, parameters);
  int result;
  do {
    result = sqlite3_step(statement.get());
  } while (result == SQLITE_ROW);
  if (result != SQLITE_DONE) {
    throw Error(sqlite3_errmsg(handle_));
  }
}

Rows Database::Query(const std::string& sql, const Params& parameters) {
  auto statement = Prepare(handle_, sql, parameters);
  Rows rows;
  int result;
  while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
    Row row;
    for (int i = 0; i < sqlite3_column_count(statement.get()); ++i) {
      const auto* text = sqlite3_column_text(statement.get(), i);
      row[sqlite3_column_name(statement.get(), i)] =
          text ? reinterpret_cast<const char*>(text) : "";
    }
    rows.push_back(std::move(row));
  }
  if (result != SQLITE_DONE) {
    throw Error(sqlite3_errmsg(handle_));
  }
  return rows;
}

Row Database::One(const std::string& sql, const Params& parameters) {
  auto rows = Query(sql, parameters);
  if (rows.size() != 1) {
    throw Error("记录不存在或不唯一");
  }
  return rows.front();
}

std::string Database::Scalar(const std::string& sql, const Params& parameters) {
  auto rows = Query(sql, parameters);
  if (rows.empty() || rows.front().empty()) {
    return "";
  }
  return rows.front().begin()->second;
}

std::string Database::LastId() const { return std::to_string(sqlite3_last_insert_rowid(handle_)); }

void Database::Initialize() {
  const std::vector<std::string> statements = {
      R"(CREATE TABLE IF NOT EXISTS users(
        id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE,
        name TEXT NOT NULL, role TEXT NOT NULL CHECK(role IN
          ('participant','organizer','admin','checker')),
        salt TEXT NOT NULL, password_hash TEXT NOT NULL,
        enabled INTEGER NOT NULL DEFAULT 1 CHECK(enabled IN (0,1))))",
      R"(CREATE TABLE IF NOT EXISTS events(
        id INTEGER PRIMARY KEY, owner_id INTEGER NOT NULL REFERENCES users(id),
        name TEXT NOT NULL, category TEXT NOT NULL, description TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'draft', version INTEGER NOT NULL DEFAULT 0))",
      R"(CREATE TABLE IF NOT EXISTS sessions(
        id INTEGER PRIMARY KEY, event_id INTEGER NOT NULL REFERENCES events(id)
        ON DELETE CASCADE, name TEXT NOT NULL, location TEXT NOT NULL,
        deadline INTEGER NOT NULL, start INTEGER NOT NULL, end INTEGER NOT NULL,
        capacity INTEGER NOT NULL CHECK(capacity>0),
        cancelled INTEGER NOT NULL DEFAULT 0))",
      R"(CREATE TABLE IF NOT EXISTS tickets(
        id INTEGER PRIMARY KEY, session_id INTEGER NOT NULL REFERENCES sessions(id)
        ON DELETE CASCADE, name TEXT NOT NULL, price INTEGER NOT NULL CHECK(price>=0),
        capacity INTEGER NOT NULL CHECK(capacity>0),
        cancelled INTEGER NOT NULL DEFAULT 0))",
      R"(CREATE TABLE IF NOT EXISTS seats(
        id INTEGER PRIMARY KEY, session_id INTEGER NOT NULL REFERENCES sessions(id),
        ticket_id INTEGER NOT NULL REFERENCES tickets(id) ON DELETE CASCADE,
        label TEXT NOT NULL, UNIQUE(session_id,label)))",
      R"(CREATE TABLE IF NOT EXISTS registrations(
        id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id),
        event_id INTEGER NOT NULL REFERENCES events(id),
        session_id INTEGER NOT NULL REFERENCES sessions(id),
        ticket_id INTEGER NOT NULL REFERENCES tickets(id),
        status TEXT NOT NULL CHECK(status IN
          ('waiting','pending','registered','cancelled','checked','absent')),
        created INTEGER NOT NULL, confirmed INTEGER, payment_due INTEGER,
        checked_at INTEGER, seat_id INTEGER UNIQUE REFERENCES seats(id),
        UNIQUE(user_id,event_id)))",
      R"(CREATE TABLE IF NOT EXISTS reviews(
        id INTEGER PRIMARY KEY, event_id INTEGER NOT NULL REFERENCES events(id),
        kind TEXT NOT NULL, scope TEXT NOT NULL, target_id INTEGER NOT NULL,
        status TEXT NOT NULL DEFAULT 'pending', reason TEXT NOT NULL DEFAULT '',
        applicant INTEGER NOT NULL REFERENCES users(id), reviewer INTEGER
        REFERENCES users(id), created INTEGER NOT NULL))",
      R"(CREATE UNIQUE INDEX IF NOT EXISTS one_pending_review ON reviews(event_id)
        WHERE status='pending')",
      R"(CREATE TABLE IF NOT EXISTS proposals(
        review_id INTEGER PRIMARY KEY REFERENCES reviews(id),
        name TEXT NOT NULL, category TEXT NOT NULL, description TEXT NOT NULL))",
      R"(CREATE TABLE IF NOT EXISTS session_proposals(
        review_id INTEGER NOT NULL REFERENCES reviews(id),
        session_id INTEGER NOT NULL REFERENCES sessions(id),
        location TEXT NOT NULL, deadline INTEGER NOT NULL, start INTEGER NOT NULL,
        end INTEGER NOT NULL, capacity INTEGER NOT NULL,
        PRIMARY KEY(review_id,session_id)))",
      R"(CREATE TABLE IF NOT EXISTS ticket_proposals(
        id INTEGER PRIMARY KEY, review_id INTEGER NOT NULL REFERENCES reviews(id),
        ticket_id INTEGER REFERENCES tickets(id), session_id INTEGER NOT NULL REFERENCES sessions(id),
        name TEXT NOT NULL, price INTEGER NOT NULL CHECK(price>=0),
        labels TEXT NOT NULL, UNIQUE(review_id,ticket_id)))",
      R"(CREATE TABLE IF NOT EXISTS session_name_proposals(
        review_id INTEGER NOT NULL REFERENCES reviews(id),
        session_id INTEGER NOT NULL REFERENCES sessions(id), name TEXT NOT NULL,
        PRIMARY KEY(review_id,session_id)))",
      R"(CREATE TABLE IF NOT EXISTS new_session_proposals(
        id INTEGER PRIMARY KEY, review_id INTEGER NOT NULL REFERENCES reviews(id),
        name TEXT NOT NULL, location TEXT NOT NULL, deadline INTEGER NOT NULL,
        start INTEGER NOT NULL, end INTEGER NOT NULL, capacity INTEGER NOT NULL))",
      R"(CREATE TABLE IF NOT EXISTS new_session_tickets(
        id INTEGER PRIMARY KEY, proposal_id INTEGER NOT NULL REFERENCES new_session_proposals(id),
        name TEXT NOT NULL, price INTEGER NOT NULL, capacity INTEGER NOT NULL, labels TEXT NOT NULL))",
      R"(CREATE TABLE IF NOT EXISTS versions(
        id INTEGER PRIMARY KEY, event_id INTEGER NOT NULL REFERENCES events(id),
        number INTEGER NOT NULL, snapshot TEXT NOT NULL, effective INTEGER NOT NULL,
        UNIQUE(event_id,number)))",
      R"(CREATE TABLE IF NOT EXISTS receipts(
        id INTEGER PRIMARY KEY, registration_id INTEGER NOT NULL
        REFERENCES registrations(id), amount INTEGER NOT NULL CHECK(amount>0),
        confirmed INTEGER NOT NULL, actor INTEGER NOT NULL REFERENCES users(id),
        abnormal INTEGER NOT NULL DEFAULT 0))",
      R"(CREATE TABLE IF NOT EXISTS refunds(
        id INTEGER PRIMARY KEY, receipt_id INTEGER NOT NULL UNIQUE
        REFERENCES receipts(id), reason TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'pending', completed INTEGER,
        actor INTEGER REFERENCES users(id)))",
      R"(CREATE TABLE IF NOT EXISTS assignments(
        session_id INTEGER NOT NULL REFERENCES sessions(id),
        checker_id INTEGER NOT NULL REFERENCES users(id),
        PRIMARY KEY(session_id,checker_id)))",
      R"(CREATE TABLE IF NOT EXISTS logs(
        id INTEGER PRIMARY KEY, actor TEXT NOT NULL, event_id INTEGER,
        user_id INTEGER, object_type TEXT NOT NULL, object_id TEXT NOT NULL,
        action TEXT NOT NULL, before_state TEXT NOT NULL, after_state TEXT NOT NULL,
        result TEXT NOT NULL, request_id TEXT NOT NULL, time INTEGER NOT NULL))",
      R"(CREATE TABLE IF NOT EXISTS requests(
        id TEXT PRIMARY KEY, actor TEXT NOT NULL, result TEXT NOT NULL,
        time INTEGER NOT NULL))",
      R"(CREATE TABLE IF NOT EXISTS notifications(
        id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id),
        event_id INTEGER NOT NULL REFERENCES events(id), message TEXT NOT NULL,
        time INTEGER NOT NULL))",
      "CREATE UNIQUE INDEX IF NOT EXISTS one_normal_receipt ON receipts(registration_id) WHERE "
      "abnormal=0",
      "CREATE INDEX IF NOT EXISTS queue ON registrations(ticket_id,status,created,id)",
      "CREATE INDEX IF NOT EXISTS event_reg ON registrations(event_id,status)",
      "CREATE INDEX IF NOT EXISTS session_reg ON registrations(session_id,status)",
      "CREATE INDEX IF NOT EXISTS log_event ON logs(event_id,time)",
  };
  Transaction transaction(*this);
  for (const auto& statement : statements) {
    Execute(statement);
  }
  transaction.Commit();
}

void Database::Backup(const std::string& destination) {
  sqlite3* output = nullptr;
  if (sqlite3_open(destination.c_str(), &output) != SQLITE_OK) {
    if (output) {
      sqlite3_close(output);
    }
    throw Error("无法创建备份");
  }
  sqlite3_backup* backup = sqlite3_backup_init(output, "main", handle_, "main");
  int result = SQLITE_ERROR;
  if (backup) {
    result = sqlite3_backup_step(backup, -1);
    sqlite3_backup_finish(backup);
  }
  sqlite3_close(output);
  if (result != SQLITE_DONE) {
    throw Error("备份失败");
  }
}

void Database::CheckIntegrity() {
  if (Scalar("PRAGMA integrity_check") != "ok" || !Query("PRAGMA foreign_key_check").empty()) {
    throw Error("数据库完整性检查失败");
  }
  if (!Query(R"(SELECT r.id FROM registrations r
      JOIN tickets t ON t.id=r.ticket_id JOIN sessions s ON s.id=t.session_id
      LEFT JOIN seats z ON z.id=r.seat_id
      WHERE r.session_id<>s.id OR r.event_id<>s.event_id OR
      (r.seat_id IS NOT NULL AND z.ticket_id<>r.ticket_id))")
           .empty()) {
    throw Error("报名层级或座位关联异常");
  }
  if (!Query(R"(SELECT s.id FROM sessions s JOIN events e ON e.id=s.event_id
       WHERE e.status<>'draft' AND s.capacity <
       (SELECT coalesce(sum(t.capacity),0) FROM tickets t WHERE t.session_id=s.id))")
           .empty()) {
    throw Error("票种容量之和超过场次容量");
  }
  if (!Query(R"(SELECT t.id FROM tickets t JOIN sessions s ON s.id=t.session_id
       JOIN events e ON e.id=s.event_id WHERE e.status<>'draft' AND t.capacity <>
       (SELECT count(*) FROM seats z WHERE z.ticket_id=t.id))")
           .empty()) {
    throw Error("票种容量与座位数不一致");
  }
  for (const auto& table : {"sessions", "tickets"}) {
    const std::string foreign = std::string(table) == "sessions" ? "session_id" : "ticket_id";
    if (!Query("SELECT x.id FROM " + std::string(table) +
               " x WHERE x.capacity<(SELECT count(*) FROM registrations r WHERE r." + foreign +
               "=x.id AND r.status IN ('pending','registered','checked'))")
             .empty()) {
      throw Error("占用超过容量");
    }
  }
}

Transaction::Transaction(Database& database) : database_(database) {
  database_.Execute("BEGIN IMMEDIATE");
}

Transaction::~Transaction() {
  if (!committed_) {
    try {
      database_.Execute("ROLLBACK");
    } catch (...) { /* Never throw in cleanup. */
    }
  }
}

void Transaction::Commit() {
  database_.Execute("COMMIT");
  committed_ = true;
}

std::string Value(const Row& row, const std::string& key) {
  const auto found = row.find(key);
  return found == row.end() ? "" : found->second;
}

std::int64_t Number(const Row& row, const std::string& key) { return std::stoll(Value(row, key)); }

std::int64_t Now() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string TimeText(std::int64_t timestamp) {
  const auto value = static_cast<std::time_t>(timestamp);
  std::tm local{};
  localtime_r(&value, &local);
  std::ostringstream output;
  output << std::put_time(&local, "%Y-%m-%d %H:%M");
  return output.str();
}

std::int64_t ParseTime(const std::string& text) {
  std::tm local{};
  local.tm_isdst = -1;
  std::istringstream input(text);
  input >> std::get_time(&local, "%Y-%m-%d %H:%M");
  if (input.fail()) {
    throw Error("日期格式应为 YYYY-MM-DD HH:MM");
  }
  const auto timestamp = std::mktime(&local);
  if (timestamp == -1 || TimeText(timestamp) != text) {
    throw Error("日期不存在");
  }
  return timestamp;
}

std::string RequestId() {
  static std::atomic<std::uint64_t> sequence{0};
  return std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
         std::to_string(++sequence);
}

std::string CsvCell(const std::string& text) {
  std::string safe = text;
  if (!safe.empty() && (safe.front() == '=' || safe.front() == '+' || safe.front() == '-' ||
                        safe.front() == '@' || safe.front() == '\t' || safe.front() == '\r')) {
    safe.insert(safe.begin(), '\'');
  }
  std::string result = "\"";
  for (char character : safe) {
    if (character == '"') {
      result += '"';
    }
    result += character;
  }
  return result + "\"";
}

std::string RoleText(const std::string& role) {
  const Row labels{{"participant", "参与者"},
                   {"organizer", "组织者"},
                   {"admin", "管理员"},
                   {"checker", "检票人员"}};
  return Value(labels, role);
}

std::string StatusText(const std::string& status) {
  const Row labels{{"waiting", "候补"},     {"pending", "待处理/待付款"}, {"registered", "已报名"},
                   {"cancelled", "已取消"}, {"checked", "已签到"},        {"absent", "缺席"},
                   {"draft", "草稿"},       {"published", "已发布"},      {"approved", "已通过"},
                   {"rejected", "已拒绝"},  {"withdrawn", "已撤回"},      {"expired", "已失效"},
                   {"completed", "已退款"}};
  const auto label = Value(labels, status);
  return label.empty() ? status : label;
}

}  // namespace campus
