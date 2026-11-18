#include "campus/service.h"

#include <crypt.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace campus {
namespace {
void Ensure(bool condition, const std::string& message) {
  if (!condition) {
    throw Error(message);
  }
}

std::string Salt() {
  std::random_device random;
  constexpr char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789./";
  std::string salt = "$6$rounds=10000$";
  for (int i = 0; i < 16; ++i) {
    salt += alphabet[random() % 64];
  }
  return salt + "$";
}

std::string Hash(const std::string& password, const std::string& salt) {
  crypt_data state{};
  const char* result = crypt_r(password.c_str(), salt.c_str(), &state);
  Ensure(result && result[0] != '*', "密码散列失败");
  return result;
}

bool Equal(const std::string& left, const std::string& right) {
  if (left.size() != right.size()) {
    return false;
  }
  unsigned char difference = 0;
  for (std::size_t i = 0; i < left.size(); ++i) {
    difference |= left[i] ^ right[i];
  }
  return difference == 0;
}

void ValidateSession(const SessionInput& input) {
  Ensure(!input.name.empty() && !input.location.empty(), "场次名称和地点必填");
  Ensure(input.capacity > 0, "容量必须为正整数");
  Ensure(input.deadline <= input.start && input.start < input.end, "时间必须满足截止≤开始<结束");
}

const char* kRegistrationSelect = R"(
 SELECT r.*,u.name AS participant,e.name AS event_name,s.name AS session_name,
 s.location,s.start,s.end,s.deadline,t.name AS ticket_name,t.price,
 coalesce(z.label,'') AS seat,
 coalesce((SELECT sum(amount) FROM receipts p WHERE p.registration_id=r.id),0)
 AS received,
 coalesce((SELECT sum(p.amount) FROM refunds f JOIN receipts p ON p.id=f.receipt_id
 WHERE p.registration_id=r.id AND f.status='pending'),0) AS refund_pending,
 coalesce((SELECT sum(p.amount) FROM refunds f JOIN receipts p ON p.id=f.receipt_id
 WHERE p.registration_id=r.id AND f.status='completed'),0) AS refunded
 FROM registrations r JOIN users u ON u.id=r.user_id
 JOIN events e ON e.id=r.event_id JOIN sessions s ON s.id=r.session_id
 JOIN tickets t ON t.id=r.ticket_id LEFT JOIN seats z ON z.id=r.seat_id
 WHERE 1=1 )";  // NOLINT(whitespace/indent_namespace)
}  // namespace

Service::Service(const std::string& path, std::function<std::int64_t()> clock)
    : database_(path), path_(path), clock_(std::move(clock)) {}

void Service::Bootstrap(const std::string& username, const std::string& password) {
  Transaction transaction(database_);
  Ensure(database_.Scalar("SELECT count(*) FROM users") == "0", "系统已经初始化");
  Ensure(!username.empty() && password.size() >= 8, "账号必填，密码至少8位");
  const auto salt = Salt();
  database_.Execute(
      "INSERT INTO users(username,name,role,salt,password_hash) "
      "VALUES(?,?,'admin',?,?)",
      {username, "系统管理员", salt, Hash(password, salt)});
  transaction.Commit();
}

Row Service::Login(const std::string& username, const std::string& password) {
  Logout();
  const auto rows = database_.Query("SELECT * FROM users WHERE username=?", {username});
  Ensure(
      rows.size() == 1 && Value(rows.front(), "enabled") == "1" &&
          Equal(Hash(password, Value(rows.front(), "salt")), Value(rows.front(), "password_hash")),
      "账号或密码错误，或账号已停用");
  user_id_ = Value(rows.front(), "id");
  return CurrentUser();
}

void Service::Logout() { user_id_.clear(); }

Row Service::Require(const std::string& role) {
  Ensure(!user_id_.empty(), "请先登录");
  auto user =
      database_.One("SELECT id,username,name,role,enabled FROM users WHERE id=?", {user_id_});
  Ensure(Value(user, "enabled") == "1", "账号已停用");
  Ensure(role.empty() || Value(user, "role") == role, "无操作权限");
  return user;
}

Row Service::CurrentUser() { return Require(); }

Row Service::Event(const std::string& event) {
  return database_.One("SELECT * FROM events WHERE id=?", {event});
}

Row Service::Own(const std::string& event) {
  Require("organizer");
  auto row = Event(event);
  Ensure(Value(row, "owner_id") == user_id_, "无活动权限");
  return row;
}

Row Service::Session(const std::string& session) {
  return database_.One("SELECT * FROM sessions WHERE id=?", {session});
}

Row Service::Ticket(const std::string& ticket) {
  return database_.One(R"(SELECT t.*,s.event_id,s.deadline,s.start,s.end,
      s.cancelled AS session_cancelled FROM tickets t JOIN sessions s
      ON s.id=t.session_id WHERE t.id=?)",
                       {ticket});
}

Row Service::Registration(const std::string& registration) {
  return database_.One("SELECT * FROM registrations WHERE id=?", {registration});
}

void Service::BeginRequest(const std::string& request) {
  Ensure(!request.empty() && request.size() <= 200, "请求编号无效");
  Ensure(database_.Query("SELECT id FROM requests WHERE id=?", {request}).empty(), "请求失败");
}

void Service::EndRequest(const std::string& request, const std::string& result) {
  database_.Execute(
      "INSERT INTO requests VALUES(?,?,?,?)",
      {request, user_id_.empty() ? "system" : user_id_, result, std::to_string(clock_())});
}

void Service::Log(const std::string& event, const std::string& user, const std::string& type,
                  const std::string& object, const std::string& action, const std::string& before,
                  const std::string& after, const std::string& request, const std::string& result) {
  database_.Execute(R"(INSERT INTO logs(actor,event_id,user_id,object_type,object_id,
      action,before_state,after_state,result,request_id,time)
      VALUES(?,nullif(?,''),nullif(?,''),?,?,?,?,?,?,?,?))",
                    {user_id_.empty() ? "system" : user_id_, event, user, type, object, action,
                     before, after, result, request, std::to_string(clock_())});
}

std::string Service::CreateUser(const std::string& username, const std::string& name,
                                const std::string& role, const std::string& password,
                                const std::string& request) {
  Transaction transaction(database_);
  Require("admin");
  BeginRequest(request);
  Ensure(!username.empty() && !name.empty() && password.size() >= 8, "账号姓名必填，密码至少8位");
  Ensure(role == "participant" || role == "organizer" || role == "checker" || role == "admin",
         "角色无效");
  Ensure(database_.Query("SELECT id FROM users WHERE username=?", {username}).empty(),
         "此登录账号已存在，请使用其他账号名称");
  const auto salt = Salt();
  database_.Execute("INSERT INTO users(username,name,role,salt,password_hash) VALUES(?,?,?,?,?)",
                    {username, name, role, salt, Hash(password, salt)});
  const auto id = database_.LastId();
  Log("", id, "user", id, "create_user", "", role, request);
  EndRequest(request, id);
  transaction.Commit();
  return id;
}

void Service::EditUser(const std::string& user, const std::string& name, const std::string& role,
                       bool enabled, const std::string& request) {
  Transaction transaction(database_);
  Require("admin");
  BeginRequest(request);
  const auto target =
      database_.One("SELECT id,username,name,role,enabled FROM users WHERE id=?", {user});
  Ensure(!name.empty(), "姓名不能为空");
  Ensure(role == "participant" || role == "organizer" || role == "checker" || role == "admin",
         "角色无效");
  if (user == user_id_) {
    Ensure(role == Value(target, "role") && enabled,
           "不能修改当前管理员的角色或停用自己，请使用其他管理员账号操作");
  }
  if (role != Value(target, "role")) {
    Ensure(database_.Query("SELECT session_id FROM assignments WHERE checker_id=?", {user}).empty(),
           "该账号仍有检票分配，请先到检票分配页撤销全部授权，再修改角色");
    Ensure(database_.Query("SELECT id FROM events WHERE owner_id=?", {user}).empty(),
           "该账号拥有活动，不能直接变更角色；可以修改姓名或停用账号");
    Ensure(database_.Query("SELECT id FROM registrations WHERE user_id=?", {user}).empty(),
           "该账号已有报名记录，不能直接变更角色；可以修改姓名或停用账号");
    Ensure(database_.Query("SELECT id FROM reviews WHERE applicant=? OR reviewer=?", {user, user})
               .empty(),
           "该账号已有审核业务记录，不能直接变更角色；可以修改姓名或停用账号");
    Ensure(database_.Query("SELECT id FROM receipts WHERE actor=?", {user}).empty() &&
               database_.Query("SELECT id FROM refunds WHERE actor=?", {user}).empty(),
           "该账号已有资金确认记录，不能直接变更角色；可以修改姓名或停用账号");
  }
  database_.Execute("UPDATE users SET name=?,role=?,enabled=? WHERE id=?",
                    {name, role, enabled ? "1" : "0", user});
  const auto before = "name=" + CsvCell(Value(target, "name")) + ";role=" + Value(target, "role") +
                      ";enabled=" + Value(target, "enabled");
  const auto after =
      "name=" + CsvCell(name) + ";role=" + role + ";enabled=" + (enabled ? "1" : "0");
  Log("", user, "user", user, "edit_user", before, after, request);
  EndRequest(request, user);
  transaction.Commit();
}

void Service::SetEnabled(const std::string& user, bool enabled, const std::string& request) {
  Transaction transaction(database_);
  Require("admin");
  BeginRequest(request);
  const auto target = database_.One("SELECT * FROM users WHERE id=?", {user});
  Ensure(user != user_id_, "不能停用当前管理员");
  database_.Execute("UPDATE users SET enabled=? WHERE id=?", {enabled ? "1" : "0", user});
  Log("", user, "user", user, "set_enabled", Value(target, "enabled"), enabled ? "1" : "0",
      request);
  EndRequest(request, user);
  transaction.Commit();
}

Rows Service::Users(const std::string& keyword) {
  Require("admin");
  return database_.Query(
      "SELECT id,username,name,role,enabled FROM users "
      "WHERE instr(username,?)>0 OR instr(name,?)>0 ORDER BY id",
      {keyword, keyword});
}

std::string Service::CreateEvent(const std::string& name, const std::string& category,
                                 const std::string& description, const std::string& request) {
  Transaction transaction(database_);
  Require("organizer");
  BeginRequest(request);
  Ensure(!name.empty(), "活动名称必填");
  database_.Execute("INSERT INTO events(owner_id,name,category,description) VALUES(?,?,?,?)",
                    {user_id_, name, category, description});
  const auto id = database_.LastId();
  Log(id, "", "event", id, "create_event", "", "draft", request);
  EndRequest(request, id);
  transaction.Commit();
  return id;
}

std::vector<DraftSession> Service::EventConfiguration(const std::string& event) {
  const auto state = Value(Own(event), "status");
  Ensure(state == "draft" || state == "published", "活动已取消，不能编辑");
  std::vector<DraftSession> output;
  for (const auto& row :
       database_.Query("SELECT * FROM sessions WHERE event_id=? ORDER BY id", {event})) {
    DraftSession session;
    session.id = Value(row, "id");
    session.cancelled = Value(row, "cancelled") == "1";
    session.deadline_locked =
        clock_() >= Number(row, "deadline") ||
        !database_
             .Query("SELECT id FROM registrations WHERE session_id=? AND status='pending'",
                    {session.id})
             .empty();
    session.details = {Value(row, "name"),      Value(row, "location"),
                       Number(row, "deadline"), Number(row, "start"),
                       Number(row, "end"),      static_cast<int>(Number(row, "capacity"))};
    for (const auto& ticket :
         database_.Query("SELECT * FROM tickets WHERE session_id=? ORDER BY id", {session.id})) {
      DraftTicket input;
      input.id = Value(ticket, "id");
      input.name = Value(ticket, "name");
      input.cancelled = Value(ticket, "cancelled") == "1";
      input.price_locked =
          !database_.Query("SELECT id FROM registrations WHERE ticket_id=?", {input.id}).empty();
      input.seats_locked =
          !database_
               .Query("SELECT id FROM registrations WHERE ticket_id=? AND seat_id IS NOT NULL",
                      {input.id})
               .empty();
      input.price_cents = static_cast<int>(Number(ticket, "price"));
      input.capacity = static_cast<int>(Number(ticket, "capacity"));
      for (const auto& seat : database_.Query(
               "SELECT label FROM seats WHERE ticket_id=? ORDER BY label", {input.id})) {
        input.seats.push_back(Value(seat, "label"));
      }
      session.tickets.push_back(input);
    }
    output.push_back(session);
  }
  return output;
}

std::vector<DraftSession> Service::DraftSessions(const std::string& event) {
  Ensure(Value(Own(event), "status") == "draft", "只能读取本人草稿配置");
  return EventConfiguration(event);
}

std::string Service::SaveDraft(const std::string& event, const std::string& name,
                               const std::string& category, const std::string& description,
                               const std::vector<DraftSession>& sessions, bool submit,
                               const std::string& request) {
  Transaction transaction(database_);
  Require("organizer");
  BeginRequest(request);
  Ensure(!name.empty(), "活动名称必填");
  std::string id = event;
  if (id.empty()) {
    database_.Execute("INSERT INTO events(owner_id,name,category,description) VALUES(?,?,?,?)",
                      {user_id_, name, category, description});
    id = database_.LastId();
  } else {
    Ensure(Value(Own(id), "status") == "draft", "只能编辑草稿，已发布活动请提交修改审核");
    Ensure(database_.Query("SELECT id FROM reviews WHERE event_id=? AND status='pending'", {id})
               .empty(),
           "审核期间不能编辑，请先撤回申请");
    Ensure(database_.Query("SELECT id FROM registrations WHERE event_id=?", {id}).empty(),
           "草稿存在报名记录，不能覆盖");
    Ensure(database_
               .Query("SELECT a.session_id FROM assignments a JOIN sessions s ON s.id=a.session_id "
                      "WHERE s.event_id=?",
                      {id})
               .empty(),
           "草稿存在检票分配，不能覆盖");
    database_.Execute("UPDATE events SET name=?,category=?,description=? WHERE id=?",
                      {name, category, description, id});
  }
  std::set<std::string> keep_sessions;
  std::set<std::string> keep_tickets;
  for (const auto& session : sessions) {
    Ensure(session.details.capacity > 0, "场次容量必须为正整数");
    if (!session.id.empty()) {
      Ensure(
          Value(Session(session.id), "event_id") == id && keep_sessions.insert(session.id).second,
          "场次不属于草稿或重复出现");
    }
    std::set<std::string> seat_labels;
    for (const auto& ticket : session.tickets) {
      Ensure(ticket.price_cents >= 0 && ticket.capacity > 0, "票价不能为负，票种容量必须为正整数");
      for (const auto& seat : ticket.seats) {
        Ensure(!seat.empty() && seat_labels.insert(seat).second,
               "同一场次的座位编号为空或重复，请调整票种座位范围");
      }
      if (!ticket.id.empty()) {
        Ensure(!session.id.empty() && Value(Ticket(ticket.id), "session_id") == session.id &&
                   keep_tickets.insert(ticket.id).second,
               "票种不属于原场次或重复出现");
      }
    }
  }
  // Clear seat ranges first so swapping two draft ticket ranges remains atomic.
  database_.Execute(
      "DELETE FROM seats WHERE session_id IN(SELECT id FROM sessions WHERE event_id=?)", {id});
  for (const auto& row : database_.Query(
           "SELECT t.id FROM tickets t JOIN sessions s ON s.id=t.session_id WHERE s.event_id=?",
           {id})) {
    if (!keep_tickets.count(Value(row, "id"))) {
      database_.Execute("DELETE FROM tickets WHERE id=?", {Value(row, "id")});
      Log(id, "", "ticket", Value(row, "id"), "delete_draft_child", "draft", "deleted", request);
    }
  }
  for (const auto& row : database_.Query("SELECT id FROM sessions WHERE event_id=?", {id})) {
    if (!keep_sessions.count(Value(row, "id"))) {
      database_.Execute("DELETE FROM sessions WHERE id=?", {Value(row, "id")});
      Log(id, "", "session", Value(row, "id"), "delete_draft_child", "draft", "deleted", request);
    }
  }
  for (const auto& session : sessions) {
    const auto& value = session.details;
    std::string session_id = session.id;
    const Params fields{value.name,
                        value.location,
                        std::to_string(value.deadline),
                        std::to_string(value.start),
                        std::to_string(value.end),
                        std::to_string(value.capacity)};
    if (session_id.empty()) {
      auto parameters = fields;
      parameters.push_back(id);
      database_.Execute(
          "INSERT INTO sessions(name,location,deadline,start,end,capacity,event_id) "
          "VALUES(?,?,?,?,?,?,?)",
          parameters);
      session_id = database_.LastId();
    } else {
      auto parameters = fields;
      parameters.push_back(session_id);
      database_.Execute(
          "UPDATE sessions SET name=?,location=?,deadline=?,start=?,end=?,capacity=? WHERE id=?",
          parameters);
    }
    Log(id, "", "session", session_id, session.id.empty() ? "create_session" : "edit_session",
        session.id.empty() ? "" : "draft", "draft", request);
    for (const auto& ticket : session.tickets) {
      std::string ticket_id = ticket.id;
      const Params values{ticket.name, std::to_string(ticket.price_cents),
                          std::to_string(ticket.capacity)};
      if (ticket_id.empty()) {
        auto parameters = values;
        parameters.push_back(session_id);
        database_.Execute("INSERT INTO tickets(name,price,capacity,session_id) VALUES(?,?,?,?)",
                          parameters);
        ticket_id = database_.LastId();
      } else {
        auto parameters = values;
        parameters.push_back(ticket_id);
        database_.Execute("UPDATE tickets SET name=?,price=?,capacity=? WHERE id=?", parameters);
      }
      for (const auto& seat : ticket.seats) {
        Ensure(!seat.empty(), "座位编号不能为空");
        database_.Execute("INSERT INTO seats(session_id,ticket_id,label) VALUES(?,?,?)",
                          {session_id, ticket_id, seat});
      }
      Log(id, "", "ticket", ticket_id, ticket.id.empty() ? "create_ticket" : "edit_ticket",
          ticket.id.empty() ? "" : "draft", "draft", request);
    }
  }
  Log(id, "", "event", id, event.empty() ? "create_event" : "edit_draft",
      event.empty() ? "" : "draft", "draft", request);
  if (submit) {
    Validate(id);
    for (const auto& session : sessions) {
      Ensure(clock_() < session.details.deadline, "报名截止时间已到，请修改后再提交");
    }
    const auto review = NewReview(id, "publish", "event", id);
    Log(id, "", "review", review, "submit_review", "", "pending", request);
  }
  EndRequest(request, id);
  transaction.Commit();
  return id;
}

void Service::EditDraft(const std::string& event, const std::string& name,
                        const std::string& category, const std::string& description,
                        const std::string& request) {
  Transaction transaction(database_);
  const auto row = Own(event);
  BeginRequest(request);
  Ensure(Value(row, "status") == "draft", "仅可直接编辑草稿");
  Ensure(database_.Query("SELECT id FROM reviews WHERE event_id=? AND status='pending'", {event})
             .empty(),
         "审核期间不能编辑");
  Ensure(!name.empty(), "活动名称必填");
  database_.Execute("UPDATE events SET name=?,category=?,description=? WHERE id=?",
                    {name, category, description, event});
  Log(event, "", "event", event, "edit_draft", Value(row, "name"), name, request);
  EndRequest(request, event);
  transaction.Commit();
}

void Service::DeleteDraft(const std::string& event, const std::string& request) {
  Transaction transaction(database_);
  const auto row = Own(event);
  BeginRequest(request);
  Ensure(Value(row, "status") == "draft", "仅草稿可删除");
  Ensure(database_.Query("SELECT id FROM reviews WHERE event_id=?", {event}).empty(),
         "有审核历史的草稿保留以供追溯");
  database_.Execute(
      "DELETE FROM seats WHERE session_id IN "
      "(SELECT id FROM sessions WHERE event_id=?)",
      {event});
  database_.Execute("DELETE FROM events WHERE id=?", {event});
  Log(event, "", "event", event, "delete_draft", "draft", "deleted", request);
  EndRequest(request, event);
  transaction.Commit();
}

std::string Service::AddSession(const std::string& event, const SessionInput& input,
                                const std::string& request) {
  Transaction transaction(database_);
  const auto row = Own(event);
  BeginRequest(request);
  Ensure(Value(row, "status") == "draft", "只能给草稿添加场次");
  Ensure(database_.Query("SELECT id FROM reviews WHERE event_id=? AND status='pending'", {event})
             .empty(),
         "审核期间不能编辑");
  ValidateSession(input);
  database_.Execute(
      "INSERT INTO sessions(event_id,name,location,deadline,start,end,capacity) "
      "VALUES(?,?,?,?,?,?,?)",
      {event, input.name, input.location, std::to_string(input.deadline),
       std::to_string(input.start), std::to_string(input.end), std::to_string(input.capacity)});
  const auto id = database_.LastId();
  Log(event, "", "session", id, "create_session", "", input.name, request);
  EndRequest(request, id);
  transaction.Commit();
  return id;
}

std::string Service::AddTicket(const std::string& session, const std::string& name, int price_cents,
                               int capacity, const Params& seats, const std::string& request) {
  Transaction transaction(database_);
  const auto parent = Session(session);
  const auto event = Value(parent, "event_id");
  Ensure(Value(Own(event), "status") == "draft", "只能给草稿添加票种");
  BeginRequest(request);
  Ensure(database_.Query("SELECT id FROM reviews WHERE event_id=? AND status='pending'", {event})
             .empty(),
         "审核期间不能编辑");
  Ensure(!name.empty() && price_cents >= 0 && capacity > 0, "票种名称、价格或容量无效");
  Ensure(seats.size() == static_cast<std::size_t>(capacity), "座位数必须等于容量");
  database_.Execute("INSERT INTO tickets(session_id,name,price,capacity) VALUES(?,?,?,?)",
                    {session, name, std::to_string(price_cents), std::to_string(capacity)});
  const auto id = database_.LastId();
  for (const auto& seat : seats) {
    Ensure(!seat.empty(), "座位编号不能为空");
    Ensure(database_.Query("SELECT id FROM seats WHERE session_id=? AND label=?", {session, seat})
               .empty(),
           "同一场次的座位编号重复，请调整票种座位范围");
    database_.Execute("INSERT INTO seats(session_id,ticket_id,label) VALUES(?,?,?)",
                      {session, id, seat});
  }
  Log(event, "", "ticket", id, "create_ticket", "", name, request);
  EndRequest(request, id);
  transaction.Commit();
  return id;
}

void Service::EditSessionDraft(const std::string& session, const SessionInput& input,
                               const std::string& request) {
  Transaction transaction(database_);
  const auto original = Session(session);
  const auto event = Value(original, "event_id");
  Ensure(Value(Own(event), "status") == "draft", "只能直接编辑草稿场次");
  BeginRequest(request);
  Ensure(database_.Query("SELECT id FROM reviews WHERE event_id=? AND status='pending'", {event})
             .empty(),
         "审核期间不能编辑");
  ValidateSession(input);
  database_.Execute(
      "UPDATE sessions SET name=?,location=?,deadline=?,start=?,end=?,capacity=? WHERE id=?",
      {input.name, input.location, std::to_string(input.deadline), std::to_string(input.start),
       std::to_string(input.end), std::to_string(input.capacity), session});
  Log(event, "", "session", session, "edit_session", Value(original, "name"), input.name, request);
  EndRequest(request, session);
  transaction.Commit();
}

void Service::EditTicketDraft(const std::string& ticket, const std::string& name, int price_cents,
                              const Params& seats, const std::string& request) {
  Transaction transaction(database_);
  const auto original = Ticket(ticket);
  const auto event = Value(original, "event_id");
  Ensure(Value(Own(event), "status") == "draft", "只能直接编辑草稿票种");
  BeginRequest(request);
  Ensure(database_.Query("SELECT id FROM reviews WHERE event_id=? AND status='pending'", {event})
             .empty(),
         "审核期间不能编辑");
  Ensure(!name.empty() && price_cents >= 0 && !seats.empty(), "票种信息无效");
  database_.Execute("DELETE FROM seats WHERE ticket_id=?", {ticket});
  database_.Execute("UPDATE tickets SET name=?,price=?,capacity=? WHERE id=?",
                    {name, std::to_string(price_cents), std::to_string(seats.size()), ticket});
  for (const auto& seat : seats) {
    Ensure(!seat.empty(), "座位编号必填");
    Ensure(database_
               .Query("SELECT id FROM seats WHERE session_id=? AND label=?",
                      {Value(original, "session_id"), seat})
               .empty(),
           "同一场次的座位编号重复，请调整票种座位范围");
    database_.Execute("INSERT INTO seats(session_id,ticket_id,label) VALUES(?,?,?)",
                      {Value(original, "session_id"), ticket, seat});
  }
  Log(event, "", "ticket", ticket, "edit_ticket", Value(original, "name"), name, request);
  EndRequest(request, ticket);
  transaction.Commit();
}

void Service::DeleteDraftChild(const std::string& scope, const std::string& target,
                               const std::string& request) {
  Transaction transaction(database_);
  Ensure(scope == "session" || scope == "ticket", "对象类型无效");
  const auto original = scope == "session" ? Session(target) : Ticket(target);
  const auto event = Value(original, "event_id");
  Ensure(Value(Own(event), "status") == "draft", "只能删除草稿对象");
  BeginRequest(request);
  Ensure(database_.Query("SELECT id FROM reviews WHERE event_id=? AND status='pending'", {event})
             .empty(),
         "审核期间不能编辑");
  if (scope == "session") {
    database_.Execute("DELETE FROM seats WHERE session_id=?", {target});
    database_.Execute("DELETE FROM sessions WHERE id=?", {target});
  } else {
    database_.Execute("DELETE FROM tickets WHERE id=?", {target});
  }
  Log(event, "", scope, target, "delete_draft_child", Value(original, "name"), "deleted", request);
  EndRequest(request, target);
  transaction.Commit();
}

std::string Service::ReviewDetails(const std::string& review) {
  const auto visible = Reviews();
  Ensure(std::any_of(visible.begin(), visible.end(),
                     [&](const Row& row) { return Value(row, "id") == review; }),
         "无审核查看权限");
  const auto row = database_.One("SELECT * FROM reviews WHERE id=?", {review});
  std::ostringstream output;
  output << "申请类型: " << Value(row, "kind") << " / " << Value(row, "scope") << " / 目标 "
         << Value(row, "target_id") << "\n\n当前内容（原发布内容）\n"
         << Snapshot(Value(row, "event_id")) << "\n候选活动内容\n";
  for (const auto& proposal :
       database_.Query("SELECT * FROM proposals WHERE review_id=?", {review})) {
    for (const auto& field : proposal) {
      output << field.first << " = " << field.second << '\n';
    }
  }
  output << "\n候选场次内容（时间为Unix秒）\n";
  for (const auto& proposal :
       database_.Query("SELECT * FROM session_proposals WHERE review_id=?", {review})) {
    for (const auto& field : proposal) {
      output << field.first << " = " << field.second << '\n';
    }
  }
  output << "\n候选票种内容（价格单位为分）\n";
  for (const auto& proposal :
       database_.Query("SELECT * FROM ticket_proposals WHERE review_id=?", {review})) {
    for (const auto& field : proposal) {
      output << field.first << " = " << field.second << '\n';
    }
  }
  output << "\n候选场次名称与新增场次\n";
  for (const auto& change :
       database_.Query("SELECT * FROM session_name_proposals WHERE review_id=?", {review})) {
    for (const auto& field : change) {
      output << field.first << " = " << field.second << '\n';
    }
  }
  for (const auto& change : database_.Query(
           "SELECT * FROM new_session_proposals WHERE review_id=? ORDER BY id", {review})) {
    output << "NEW SESSION\n";
    for (const auto& field : change) {
      output << field.first << " = " << field.second << '\n';
    }
    for (const auto& ticket :
         database_.Query("SELECT * FROM new_session_tickets WHERE proposal_id=? ORDER BY id",
                         {Value(change, "id")})) {
      output << "NEW TICKET\n";
      for (const auto& field : ticket) {
        output << field.first << " = " << field.second << '\n';
      }
    }
  }
  return output.str();
}

void Service::Validate(const std::string& event) {
  const auto row = Event(event);
  Ensure(!Value(row, "name").empty() && !Value(row, "category").empty() &&
             !Value(row, "description").empty(),
         "活动必填信息不完整");
  const auto sessions = database_.Query("SELECT * FROM sessions WHERE event_id=?", {event});
  Ensure(!sessions.empty(), "至少需要一个场次");
  for (const auto& session : sessions) {
    const auto id = Value(session, "id");
    Ensure(!Value(session, "name").empty() && !Value(session, "location").empty(),
           "场次名称和地点必填");
    Ensure(Number(session, "deadline") <= Number(session, "start") &&
               Number(session, "start") < Number(session, "end"),
           "场次时间不合法");
    auto tickets = database_.Query("SELECT * FROM tickets WHERE session_id=?", {id});
    Ensure(!tickets.empty(), "每场次至少一个票种");
    std::int64_t total = 0;
    for (const auto& ticket : tickets) {
      Ensure(!Value(ticket, "name").empty(), "票种名称必填");
      total += Number(ticket, "capacity");
      Ensure(database_.Scalar("SELECT count(*) FROM seats WHERE ticket_id=?",
                              {Value(ticket, "id")}) == Value(ticket, "capacity"),
             "座位数量不符");
    }
    Ensure(total <= Number(session, "capacity"), "票种容量总和超过场次容量");
  }
  database_.CheckIntegrity();
}

std::string Service::NewReview(const std::string& event, const std::string& kind,
                               const std::string& scope, const std::string& target) {
  const auto pending =
      database_.Query("SELECT id,kind FROM reviews WHERE event_id=? AND status='pending'", {event});
  if (!pending.empty()) {
    const Row kinds{{"publish", "发布"}, {"edit", "修改"}, {"cancel", "取消"}};
    throw Error("此活动已有待审核的" + Value(kinds, Value(pending.front(), "kind")) +
                "申请（编号 " + Value(pending.front(), "id") +
                "），请到“我的申请”撤回后重新提交，或等待管理员审核");
  }
  database_.Execute(
      "INSERT INTO reviews(event_id,kind,scope,target_id,applicant,created) "
      "VALUES(?,?,?,?,?,?)",
      {event, kind, scope, target, user_id_, std::to_string(clock_())});
  return database_.LastId();
}

std::string Service::Submit(const std::string& event, const std::string& request) {
  Transaction transaction(database_);
  Ensure(Value(Own(event), "status") == "draft", "只能提交草稿");
  BeginRequest(request);
  Validate(event);
  for (const auto& session : database_.Query("SELECT * FROM sessions WHERE event_id=?", {event})) {
    Ensure(clock_() < Number(session, "deadline"), "提交时必须尚未截止");
  }
  const auto id = NewReview(event, "publish", "event", event);
  Log(event, "", "review", id, "submit_review", "", "pending", request);
  EndRequest(request, id);
  transaction.Commit();
  return id;
}

std::string Service::ProposeEdit(const std::string& event, const std::string& name,
                                 const std::string& category, const std::string& description,
                                 const std::string& request) {
  Transaction transaction(database_);
  Ensure(Value(Own(event), "status") == "published", "只能申请修改已发布活动");
  BeginRequest(request);
  Ensure(!name.empty() && !category.empty() && !description.empty(), "必填信息不完整");
  const auto id = NewReview(event, "edit", "event", event);
  database_.Execute("INSERT INTO proposals VALUES(?,?,?,?)", {id, name, category, description});
  Log(event, "", "review", id, "propose_edit", "", "pending", request);
  EndRequest(request, id);
  transaction.Commit();
  return id;
}

std::string Service::ProposeCompleteEdit(const std::string& event, const std::string& name,
                                         const std::string& category,
                                         const std::string& description,
                                         const std::vector<DraftSession>& sessions,
                                         const std::string& request) {
  Transaction transaction(database_);
  Ensure(Value(Own(event), "status") == "published", "只能修改已发布活动");
  BeginRequest(request);
  Ensure(!name.empty() && !category.empty() && !description.empty(), "活动基本信息必填");
  const auto review = NewReview(event, "edit", "event", event);
  database_.Execute("INSERT INTO proposals VALUES(?,?,?,?)", {review, name, category, description});
  const auto originals = EventConfiguration(event);
  std::set<std::string> included;
  for (const auto& session : sessions) {
    ValidateSession(session.details);
    Ensure(!session.tickets.empty(), "每个场次至少一个票种");
    std::int64_t total = 0;
    std::set<std::string> labels;
    for (const auto& ticket : session.tickets) {
      Ensure(!ticket.name.empty() && ticket.price_cents >= 0 && ticket.capacity > 0,
             "票种名称、价格或容量无效");
      Ensure(ticket.seats.size() == static_cast<std::size_t>(ticket.capacity),
             "票种座位数量必须等于容量");
      total += ticket.capacity;
      for (const auto& seat : ticket.seats) {
        Ensure(!seat.empty() && seat.find('\n') == std::string::npos && labels.insert(seat).second,
               "同一场次座位编号为空或重复，请调整票种座位范围");
      }
    }
    Ensure(total <= session.details.capacity, "票种容量总和超过场次容量");
    const DraftSession* original = nullptr;
    if (!session.id.empty()) {
      for (const auto& item : originals) {
        if (item.id == session.id) {
          original = &item;
        }
      }
      Ensure(original && included.insert(session.id).second, "场次不属于此活动或重复出现");
      const auto& before = original->details;
      const auto& after = session.details;
      const bool changed = before.name != after.name || before.location != after.location ||
                           before.deadline != after.deadline || before.start != after.start ||
                           before.end != after.end || before.capacity != after.capacity;
      if (changed) {
        Ensure(!original->cancelled, "已取消场次不能修改");
        if (before.deadline != after.deadline) {
          Ensure(!original->deadline_locked, "原截止已到或存在待付款报名，不能修改截止时间");
        }
        database_.Execute("INSERT INTO session_proposals VALUES(?,?,?,?,?,?,?)",
                          {review, session.id, after.location, std::to_string(after.deadline),
                           std::to_string(after.start), std::to_string(after.end),
                           std::to_string(after.capacity)});
        database_.Execute("INSERT INTO session_name_proposals VALUES(?,?,?)",
                          {review, session.id, after.name});
      }
    }
    std::string new_session;
    if (!original) {
      Ensure(clock_() < session.details.deadline, "新增场次必须尚未截止");
      const auto& input = session.details;
      database_.Execute(
          "INSERT INTO new_session_proposals(review_id,name,location,deadline,start,end,capacity) "
          "VALUES(?,?,?,?,?,?,?)",
          {review, input.name, input.location, std::to_string(input.deadline),
           std::to_string(input.start), std::to_string(input.end), std::to_string(input.capacity)});
      new_session = database_.LastId();
    }
    std::set<std::string> included_tickets;
    for (const auto& ticket : session.tickets) {
      std::string seat_text;
      for (const auto& seat : ticket.seats) {
        seat_text += seat + "\n";
      }
      const DraftTicket* previous = nullptr;
      if (!ticket.id.empty()) {
        Ensure(original, "新增场次不能引用原场次票种");
        for (const auto& item : original->tickets) {
          if (item.id == ticket.id) {
            previous = &item;
          }
        }
        Ensure(previous && included_tickets.insert(ticket.id).second, "票种不属于原场次或重复出现");
      }
      if (!original) {
        database_.Execute(
            "INSERT INTO new_session_tickets(proposal_id,name,price,capacity,labels) "
            "VALUES(?,?,?,?,?)",
            {new_session, ticket.name, std::to_string(ticket.price_cents),
             std::to_string(ticket.capacity), seat_text});
        continue;
      }
      if (previous && previous->name == ticket.name &&
          previous->price_cents == ticket.price_cents && previous->capacity == ticket.capacity &&
          std::set<std::string>(previous->seats.begin(), previous->seats.end()) ==
              std::set<std::string>(ticket.seats.begin(), ticket.seats.end())) {
        continue;
      }
      Ensure(!original->cancelled && (!previous || !previous->cancelled),
             "已取消场次或票种不能修改");
      database_.Execute(
          "INSERT INTO ticket_proposals(review_id,ticket_id,session_id,name,price,labels) "
          "VALUES(?,nullif(?,''),?,?,?,?)",
          {review, ticket.id, session.id, ticket.name, std::to_string(ticket.price_cents),
           seat_text});
    }
    if (original) {
      Ensure(included_tickets.size() == original->tickets.size(),
             "不能直接删除已发布票种，请另行提交取消申请");
    }
  }
  Ensure(included.size() == originals.size(), "不能直接删除已发布场次，请另行提交取消申请");
  // Validate the entire candidate against current registrations and assignments
  // without making any candidate content visible before administrator approval.
  database_.Execute("SAVEPOINT validate_edit_candidate");
  ApplyEventEdit(review, event, request);
  database_.Execute("ROLLBACK TO validate_edit_candidate");
  database_.Execute("RELEASE validate_edit_candidate");
  Log(event, "", "review", review, "propose_complete_edit", "", "pending", request);
  EndRequest(request, review);
  transaction.Commit();
  return review;
}

void Service::ProposeSession(const std::string& review, const std::string& session,
                             const SessionInput& input, const std::string& request) {
  Transaction transaction(database_);
  const auto row = database_.One("SELECT * FROM reviews WHERE id=?", {review});
  Own(Value(row, "event_id"));
  BeginRequest(request);
  Ensure(Value(row, "status") == "pending" && Value(row, "kind") == "edit", "申请状态无效");
  const auto current = Session(session);
  Ensure(Value(current, "event_id") == Value(row, "event_id"), "场次不属于活动");
  ValidateSession(input);
  if (Number(current, "deadline") != input.deadline) {
    Ensure(clock_() < Number(current, "deadline"), "原截止已到，不能修改截止");
    Ensure(database_
               .Query("SELECT id FROM registrations WHERE session_id=? AND status='pending'",
                      {session})
               .empty(),
           "既有付款期限调整规则待确认，暂不支持此时修改截止");
  }
  database_.Execute(
      "INSERT OR REPLACE INTO session_proposals VALUES(?,?,?,?,?,?,?)",
      {review, session, input.location, std::to_string(input.deadline), std::to_string(input.start),
       std::to_string(input.end), std::to_string(input.capacity)});
  Log(Value(row, "event_id"), "", "review", review, "propose_session", "", session, request);
  EndRequest(request, review);
  transaction.Commit();
}

void Service::ProposeTicket(const std::string& review, const std::string& session,
                            const std::string& ticket, const std::string& name, int price_cents,
                            const Params& seats, const std::string& request) {
  Transaction transaction(database_);
  const auto row = database_.One("SELECT * FROM reviews WHERE id=?", {review});
  Own(Value(row, "event_id"));
  BeginRequest(request);
  Ensure(Value(row, "status") == "pending" && Value(row, "kind") == "edit", "必须是待审修改申请");
  Ensure(Value(Session(session), "event_id") == Value(row, "event_id"), "场次不属于此活动");
  Ensure(!name.empty() && price_cents >= 0 && !seats.empty(), "票种信息不完整");
  if (!ticket.empty()) {
    Ensure(Value(Ticket(ticket), "session_id") == session, "票种不属于场次");
  }
  std::string labels;
  std::set<std::string> unique;
  for (const auto& seat : seats) {
    Ensure(!seat.empty() && seat.find('\n') == std::string::npos && unique.insert(seat).second,
           "座位编号为空、重复或含换行");
    labels += seat + "\n";
  }
  if (!ticket.empty()) {
    database_.Execute("DELETE FROM ticket_proposals WHERE review_id=? AND ticket_id=?",
                      {review, ticket});
  }
  database_.Execute(
      "INSERT INTO ticket_proposals(review_id,ticket_id,session_id,name,price,labels) "
      "VALUES(?,nullif(?,''),?,?,?,?)",
      {review, ticket, session, name, std::to_string(price_cents), labels});
  Log(Value(row, "event_id"), "", "review", review, "propose_ticket", ticket, name, request);
  EndRequest(request, review);
  transaction.Commit();
}

std::string Service::RequestCancellation(const std::string& event, const std::string& scope,
                                         const std::string& target, const std::string& request) {
  Transaction transaction(database_);
  Ensure(Value(Own(event), "status") == "published", "活动尚未发布或已取消");
  BeginRequest(request);
  Ensure(scope == "event" || scope == "session" || scope == "ticket", "取消范围无效");
  if (scope == "event") {
    Ensure(target == event, "活动编号不匹配");
    Ensure(!database_
                .Query("SELECT id FROM sessions WHERE event_id=? AND end>? AND cancelled=0",
                       {event, std::to_string(clock_())})
                .empty(),
           "没有未结束场次");
  } else {
    const auto child = scope == "session" ? Session(target) : Ticket(target);
    Ensure(Value(child, "event_id") == event && clock_() < Number(child, "end"),
           "对象不属于活动或已结束");
    Ensure(Value(child, "cancelled") == "0", "对象已取消");
  }
  const auto id = NewReview(event, "cancel", scope, target);
  Log(event, "", "review", id, "request_cancel", "", "pending", request);
  EndRequest(request, id);
  transaction.Commit();
  return id;
}

void Service::Withdraw(const std::string& review, const std::string& request) {
  Transaction transaction(database_);
  const auto row = database_.One("SELECT * FROM reviews WHERE id=?", {review});
  Own(Value(row, "event_id"));
  BeginRequest(request);
  Ensure(Value(row, "status") == "pending", "申请已处理或已撤回，不能重复撤回");
  database_.Execute("UPDATE reviews SET status='withdrawn' WHERE id=?", {review});
  Log(Value(row, "event_id"), "", "review", review, "withdraw", "pending", "withdrawn", request);
  EndRequest(request, review);
  transaction.Commit();
}

std::string Service::Snapshot(const std::string& event) {
  std::ostringstream output;
  const auto row = Event(event);
  for (const auto& item : row) {
    output << item.first << '=' << CsvCell(item.second) << '\n';
  }
  for (const auto& session :
       database_.Query("SELECT * FROM sessions WHERE event_id=? ORDER BY id", {event})) {
    output << "SESSION\n";
    for (const auto& item : session) {
      output << item.first << '=' << CsvCell(item.second) << '\n';
    }
    for (const auto& ticket : database_.Query(
             "SELECT * FROM tickets WHERE session_id=? ORDER BY id", {Value(session, "id")})) {
      output << "TICKET\n";
      for (const auto& item : ticket) {
        output << item.first << '=' << CsvCell(item.second) << '\n';
      }
      for (const auto& seat : database_.Query(
               "SELECT label FROM seats WHERE ticket_id=? ORDER BY label", {Value(ticket, "id")})) {
        output << "seat=" << CsvCell(Value(seat, "label")) << '\n';
      }
    }
  }
  return output.str();
}

void Service::ApplyEventEdit(const std::string& review, const std::string& event,
                             const std::string& request) {
  const auto proposal = database_.One("SELECT * FROM proposals WHERE review_id=?", {review});
  for (const auto& change :
       database_.Query("SELECT * FROM session_proposals WHERE review_id=?", {review})) {
    const auto session_id = Value(change, "session_id");
    const auto original = Session(session_id);
    if (Value(change, "deadline") != Value(original, "deadline")) {
      Ensure(clock_() < Number(original, "deadline"), "原截止已到，不能批准截止修改");
      Ensure(database_
                 .Query("SELECT id FROM registrations WHERE session_id=? AND status='pending'",
                        {session_id})
                 .empty(),
             "存在待付款报名，不能批准截止时间修改");
    }
    Ensure(clock_() < Number(original, "start"), "原场次开始后的时间修改暂不支持");
    Ensure(Number(change, "start") > clock_(), "新开始时间必须晚于审核时间");
    database_.Execute(
        "UPDATE sessions SET location=?,deadline=?,start=?,end=?,capacity=? WHERE id=?",
        {Value(change, "location"), Value(change, "deadline"), Value(change, "start"),
         Value(change, "end"), Value(change, "capacity"), session_id});
    if (Value(change, "location") != Value(original, "location") ||
        Value(change, "start") != Value(original, "start") ||
        Value(change, "end") != Value(original, "end")) {
      database_.Execute(
          "INSERT INTO notifications(user_id,event_id,message,time) "
          "SELECT user_id,event_id,?,? FROM registrations WHERE session_id=?",
          {"场次时间或地点已更新，请查看活动详情", std::to_string(clock_()), session_id});
    }
  }
  for (const auto& change :
       database_.Query("SELECT * FROM session_name_proposals WHERE review_id=?", {review})) {
    database_.Execute("UPDATE sessions SET name=? WHERE id=? AND event_id=?",
                      {Value(change, "name"), Value(change, "session_id"), event});
  }
  for (const auto& change : database_.Query(
           "SELECT * FROM new_session_proposals WHERE review_id=? ORDER BY id", {review})) {
    Ensure(clock_() < Number(change, "deadline"), "新增场次的报名截止时间已到，不能批准");
    database_.Execute(
        "INSERT INTO sessions(event_id,name,location,deadline,start,end,capacity) "
        "VALUES(?,?,?,?,?,?,?)",
        {event, Value(change, "name"), Value(change, "location"), Value(change, "deadline"),
         Value(change, "start"), Value(change, "end"), Value(change, "capacity")});
    const auto session = database_.LastId();
    Log(event, "", "session", session, "create_session", "", "published", request);
    for (const auto& ticket :
         database_.Query("SELECT * FROM new_session_tickets WHERE proposal_id=? ORDER BY id",
                         {Value(change, "id")})) {
      database_.Execute(
          "INSERT INTO tickets(session_id,name,price,capacity) VALUES(?,?,?,?)",
          {session, Value(ticket, "name"), Value(ticket, "price"), Value(ticket, "capacity")});
      const auto id = database_.LastId();
      std::istringstream labels(Value(ticket, "labels"));
      std::string seat;
      while (std::getline(labels, seat)) {
        database_.Execute("INSERT INTO seats(session_id,ticket_id,label) VALUES(?,?,?)",
                          {session, id, seat});
      }
      Log(event, "", "ticket", id, "create_ticket", "", "published", request);
    }
  }
  for (const auto& change :
       database_.Query("SELECT * FROM ticket_proposals WHERE review_id=? ORDER BY id", {review})) {
    const auto session = Value(change, "session_id");
    Ensure(clock_() < Number(Session(session), "deadline"), "票种修改或新增必须在截止前通过");
    const auto ticket = Value(change, "ticket_id");
    Params labels;
    std::istringstream stream(Value(change, "labels"));
    std::string label;
    while (std::getline(stream, label)) {
      labels.push_back(label);
    }
    std::string target = ticket;
    if (ticket.empty()) {
      database_.Execute(
          "INSERT INTO tickets(session_id,name,price,capacity) VALUES(?,?,?,?)",
          {session, Value(change, "name"), Value(change, "price"), std::to_string(labels.size())});
      target = database_.LastId();
    } else {
      const auto original = Ticket(ticket);
      if (Value(original, "price") != Value(change, "price")) {
        Ensure(database_.Query("SELECT id FROM registrations WHERE ticket_id=?", {ticket}).empty(),
               "已有报名，禁止修改原票价；请新增票种");
      }
      const auto assigned = database_.Query(
          "SELECT id FROM registrations WHERE ticket_id=? AND seat_id IS NOT NULL", {ticket});
      std::set<std::string> original_labels;
      for (const auto& seat :
           database_.Query("SELECT label FROM seats WHERE ticket_id=?", {ticket})) {
        original_labels.insert(Value(seat, "label"));
      }
      const std::set<std::string> proposed_labels(labels.begin(), labels.end());
      Ensure(assigned.empty() ||
                 (original_labels == proposed_labels &&
                  labels.size() >= static_cast<std::size_t>(Number(original, "capacity"))),
             "已排座后禁止修改座位范围或缩减容量");
      database_.Execute(
          "UPDATE tickets SET name=?,price=?,capacity=? WHERE id=?",
          {Value(change, "name"), Value(change, "price"), std::to_string(labels.size()), ticket});
      if (original_labels == proposed_labels) {
        continue;
      }
      database_.Execute("DELETE FROM seats WHERE ticket_id=?", {ticket});
    }
    for (const auto& seat : labels) {
      Ensure(database_.Query("SELECT id FROM seats WHERE session_id=? AND label=?", {session, seat})
                 .empty(),
             "候选票种的座位与本场次已有座位重复，请调整候选座位范围");
      database_.Execute("INSERT INTO seats(session_id,ticket_id,label) VALUES(?,?,?)",
                        {session, target, seat});
    }
  }
  database_.Execute(
      "UPDATE events SET name=?,category=?,description=?,version=version+1 WHERE id=?",
      {Value(proposal, "name"), Value(proposal, "category"), Value(proposal, "description"),
       event});
  Validate(event);
  ValidateAssignmentSchedule(event);
}

void Service::Decide(const std::string& review, bool approved, const std::string& reason,
                     const std::string& request) {
  Transaction transaction(database_);
  Require("admin");
  BeginRequest(request);
  const auto row = database_.One("SELECT * FROM reviews WHERE id=?", {review});
  Ensure(Value(row, "status") == "pending", "申请不是待审核");
  Ensure(approved || !reason.empty(), "拒绝审核必须填写原因");
  const auto event = Value(row, "event_id");
  const auto kind = Value(row, "kind");
  if (approved && kind == "publish") {
    Validate(event);
    for (const auto& session :
         database_.Query("SELECT * FROM sessions WHERE event_id=?", {event})) {
      Ensure(clock_() < Number(session, "deadline"), "审核时场次已经截止");
    }
    database_.Execute("UPDATE events SET status='published',version=version+1 WHERE id=?", {event});
  }
  if (approved && kind == "edit") {
    ApplyEventEdit(review, event, request);
  }
  if (approved && kind == "cancel") {
    const auto scope = Value(row, "scope");
    const auto target = Value(row, "target_id");
    if (scope != "event") {
      const auto child = scope == "session" ? Session(target) : Ticket(target);
      Ensure(clock_() < Number(child, "end"), "取消申请已过场次结束时间");
    }
    std::string condition = "s.event_id=?";
    if (scope == "session") {
      condition += " AND s.id=" + std::to_string(std::stoll(target));
    }
    if (scope == "ticket") {
      condition += " AND t.id=" + std::to_string(std::stoll(target));
    }
    const auto targets = database_.Query(
        "SELECT t.id FROM tickets t JOIN sessions s ON "
        "s.id=t.session_id WHERE " +
            condition + " AND s.end>?",
        {event, std::to_string(clock_())});
    for (const auto& ticket : targets) {
      const auto ticket_id = Value(ticket, "id");
      for (const auto& registration :
           database_.Query("SELECT * FROM registrations WHERE ticket_id=?", {ticket_id})) {
        const auto status = Value(registration, "status");
        if (status == "waiting" || status == "pending" || status == "registered") {
          Transition(registration, "cancelled", "object_cancel", request);
        }
        if (status != "absent") {
          Refund(Value(registration, "id"), "对象取消获批");
        }
      }
      database_.Execute("UPDATE tickets SET cancelled=1 WHERE id=?", {ticket_id});
    }
    if (scope == "session") {
      database_.Execute("UPDATE sessions SET cancelled=1 WHERE id=?", {target});
    }
    if (scope == "event") {
      database_.Execute("UPDATE events SET status='cancelled' WHERE id=?", {event});
      database_.Execute("UPDATE sessions SET cancelled=1 WHERE event_id=? AND end>?",
                        {event, std::to_string(clock_())});
    }
  }
  if (approved && (kind == "publish" || kind == "edit")) {
    database_.Execute(
        "INSERT INTO versions(event_id,number,snapshot,effective) "
        "SELECT id,version,?,? FROM events WHERE id=?",
        {Snapshot(event), std::to_string(clock_()), event});
  }
  const std::string status = approved ? "approved" : "rejected";
  database_.Execute("UPDATE reviews SET status=?,reason=?,reviewer=? WHERE id=?",
                    {status, reason, user_id_, review});
  Log(event, "", "review", review, "decide_review", "pending", status, request,
      approved ? "approved" : reason);
  EndRequest(request, review);
  transaction.Commit();
}

Rows Service::Reviews() {
  const auto user = Require();
  Ensure(Value(user, "role") == "admin" || Value(user, "role") == "organizer", "无审核查询权限");
  std::string sql =
      "SELECT v.*,e.name AS event_name,coalesce(p.name,'') AS proposed_name,"
      "coalesce(p.description,'') AS proposed_description FROM reviews v "
      "JOIN events e ON e.id=v.event_id LEFT JOIN proposals p ON p.review_id=v.id";
  Params parameters;
  if (Value(user, "role") == "organizer") {
    sql += " WHERE e.owner_id=?";
    parameters.push_back(user_id_);
  }
  return database_.Query(sql + " ORDER BY v.id DESC", parameters);
}

bool Service::Visible(const Row& event, const Row& user) {
  const auto role = Value(user, "role");
  if (role == "admin") {
    return true;
  }
  if (role == "organizer") {
    return Value(event, "owner_id") == user_id_;
  }
  if (role == "participant") {
    return Value(event, "status") == "published";
  }
  return !database_
              .Query(
                  "SELECT a.session_id FROM assignments a JOIN sessions s "
                  "ON s.id=a.session_id WHERE a.checker_id=? AND s.event_id=?",
                  {user_id_, Value(event, "id")})
              .empty();
}

Rows Service::Events(const Search& search) {
  const auto user = Require();
  Rows output;
  for (auto event : database_.Query("SELECT * FROM events ORDER BY id DESC")) {
    if (!Visible(event, user)) {
      continue;
    }
    const auto id = Value(event, "id");
    if (!search.keyword.empty() && Value(event, "name").find(search.keyword) == std::string::npos &&
        Value(event, "description").find(search.keyword) == std::string::npos) {
      continue;
    }
    if (!search.category.empty() && Value(event, "category") != search.category) {
      continue;
    }
    if (!search.status.empty() && database_
                                      .Query("SELECT id FROM registrations WHERE "
                                             "event_id=? AND user_id=? AND status=?",
                                             {id, user_id_, search.status})
                                      .empty()) {
      continue;
    }
    bool matches = search.date.empty() && search.location.empty();
    for (const auto& session : database_.Query("SELECT * FROM sessions WHERE event_id=?", {id})) {
      if ((search.date.empty() ||
           TimeText(Number(session, "start")).substr(0, 10) == search.date) &&
          (search.location.empty() ||
           Value(session, "location").find(search.location) != std::string::npos)) {
        matches = true;
      }
    }
    if (matches) {
      output.push_back(std::move(event));
    }
  }
  return output;
}

Rows Service::Sessions(const std::string& event) {
  const auto user = Require();
  Ensure(Visible(Event(event), user), "无活动查看权限");
  if (Value(user, "role") == "checker") {
    return database_.Query(
        "SELECT s.* FROM sessions s JOIN assignments a ON a.session_id=s.id "
        "WHERE s.event_id=? AND a.checker_id=? ORDER BY s.id",
        {event, user_id_});
  }
  return database_.Query("SELECT * FROM sessions WHERE event_id=? ORDER BY id", {event});
}

Rows Service::Tickets(const std::string& session) {
  const auto parent = Session(session);
  const auto sessions = Sessions(Value(parent, "event_id"));
  Ensure(std::any_of(sessions.begin(), sessions.end(),
                     [&](const Row& row) { return Value(row, "id") == session; }),
         "无场次查看权限");
  return database_.Query(R"(SELECT t.*,(SELECT count(*) FROM registrations r
    WHERE r.ticket_id=t.id AND r.status IN ('pending','registered','checked')) AS occupied,
    (SELECT count(*) FROM registrations r WHERE r.ticket_id=t.id AND r.status='waiting') AS waiting
    FROM tickets t WHERE session_id=? ORDER BY t.id)",
                         {session});
}

Rows Service::Versions(const std::string& event) {
  const auto user = Require();
  Ensure(Visible(Event(event), user), "无活动查看权限");
  return database_.Query("SELECT * FROM versions WHERE event_id=? ORDER BY number DESC", {event});
}

bool Service::Blocked(const Row& ticket, bool cancellation_only) {
  const auto event = Value(ticket, "event_id");
  if (Value(Event(event), "status") != "published" || Value(ticket, "cancelled") == "1" ||
      Value(ticket, "session_cancelled") == "1") {
    return true;
  }
  for (const auto& review :
       database_.Query("SELECT * FROM reviews WHERE event_id=? AND status='pending'", {event})) {
    if (!cancellation_only && Value(review, "kind") == "edit") {
      return true;
    }
    if (Value(review, "kind") != "cancel") {
      continue;
    }
    const auto scope = Value(review, "scope");
    const auto target = Value(review, "target_id");
    if (scope == "event" || (scope == "session" && target == Value(ticket, "session_id")) ||
        (scope == "ticket" && target == Value(ticket, "id"))) {
      return true;
    }
  }
  return false;
}

bool Service::Available(const Row& ticket) {
  const auto occupied = database_.Scalar(
      "SELECT count(*) FROM registrations WHERE ticket_id=? "
      "AND status IN ('pending','registered','checked')",
      {Value(ticket, "id")});
  const auto parent = Session(Value(ticket, "session_id"));
  const auto total = database_.Scalar(
      "SELECT count(*) FROM registrations WHERE session_id=? "
      "AND status IN ('pending','registered','checked')",
      {Value(ticket, "session_id")});
  return std::stoll(occupied) < Number(ticket, "capacity") &&
         std::stoll(total) < Number(parent, "capacity");
}

std::string Service::Register(const std::string& ticket_id, const std::string& request) {
  Transaction transaction(database_);
  Require("participant");
  BeginRequest(request);
  const auto ticket = Ticket(ticket_id);
  Ensure(!Blocked(ticket) && clock_() < Number(ticket, "deadline"), "报名未开放、暂停或已截止");
  const auto previous =
      database_.Query("SELECT status FROM registrations WHERE user_id=? AND event_id=?",
                      {user_id_, Value(ticket, "event_id")});
  if (!previous.empty()) {
    Ensure(false, Value(previous.front(), "status") == "cancelled"
                      ? "你已取消过此活动的报名，不能重新报名同一活动"
                      : "你已报名此活动，请到我的报名查看现有记录");
  }
  Promote(ticket_id, request);
  const bool waiting = !Available(ticket) || !database_
                                                  .Query(
                                                      "SELECT id FROM registrations "
                                                      "WHERE ticket_id=? AND status='waiting'",
                                                      {ticket_id})
                                                  .empty();
  const std::string status = waiting                        ? "waiting"
                             : Number(ticket, "price") == 0 ? "registered"
                                                            : "pending";
  const auto now = std::to_string(clock_());
  database_.Execute(
      "INSERT INTO registrations(user_id,event_id,session_id,ticket_id,status,created,"
      "confirmed,payment_due) VALUES(?,?,?,?,?,?,nullif(?,''),nullif(?,''))",
      {user_id_, Value(ticket, "event_id"), Value(ticket, "session_id"), ticket_id, status, now,
       status == "registered" ? now : "",
       status == "pending" ? std::to_string(std::min(clock_() + 1800, Number(ticket, "deadline")))
                           : ""});
  const auto id = database_.LastId();
  Log(Value(ticket, "event_id"), user_id_, "registration", id, "register", "", status, request);
  EndRequest(request, id);
  transaction.Commit();
  return id;
}

void Service::Transition(const Row& registration, const std::string& status,
                         const std::string& action, const std::string& request) {
  const auto before = Value(registration, "status");
  const std::map<std::string, std::set<std::string>> allowed{
      {"waiting", {"pending", "registered", "cancelled"}},
      {"pending", {"registered", "cancelled"}},
      {"registered", {"checked", "cancelled", "absent"}}};
  const auto found = allowed.find(before);
  Ensure(found != allowed.end() && found->second.count(status), "请求失败");
  database_.Execute("UPDATE registrations SET status=? WHERE id=?",
                    {status, Value(registration, "id")});
  Log(Value(registration, "event_id"), Value(registration, "user_id"), "registration",
      Value(registration, "id"), action, before, status, request);
}

void Service::Promote(const std::string& ticket_id, const std::string& request) {
  const auto ticket = Ticket(ticket_id);
  if (Blocked(ticket) || clock_() >= Number(ticket, "deadline")) {
    return;
  }
  for (const auto& registration :
       database_.Query("SELECT * FROM registrations WHERE "
                       "ticket_id=? AND status='waiting' ORDER BY created,id",
                       {ticket_id})) {
    if (!Available(ticket)) {
      break;
    }
    const bool free = Number(ticket, "price") == 0;
    Transition(registration, free ? "registered" : "pending", "promote", request);
    database_.Execute(
        "UPDATE registrations SET confirmed=nullif(?,''),payment_due=nullif(?,'') WHERE id=?",
        {free ? std::to_string(clock_()) : "",
         free ? "" : std::to_string(std::min(clock_() + 1800, Number(ticket, "deadline"))),
         Value(registration, "id")});
  }
}

void Service::Refund(const std::string& registration, const std::string& reason) {
  database_.Execute(
      "INSERT OR IGNORE INTO refunds(receipt_id,reason) "
      "SELECT id,? FROM receipts WHERE registration_id=?",
      {reason, registration});
}

void Service::Cancel(const std::string& registration, const std::string& request) {
  Transaction transaction(database_);
  Require("participant");
  BeginRequest(request);
  const auto row = Registration(registration);
  Ensure(Value(row, "user_id") == user_id_, "无报名权限");
  const auto ticket = Ticket(Value(row, "ticket_id"));
  Ensure(clock_() < Number(ticket, "deadline"), "已经截止");
  const auto status = Value(row, "status");
  Ensure(status == "waiting" || status == "pending" || status == "registered", "请求失败");
  Transition(row, "cancelled", "cancel_registration", request);
  Refund(registration, "本人取消");
  Promote(Value(row, "ticket_id"), request);
  EndRequest(request, registration);
  transaction.Commit();
}

void Service::Receive(const std::string& registration, int amount_cents,
                      const std::string& request) {
  Transaction transaction(database_);
  const auto row = Registration(registration);
  Own(Value(row, "event_id"));
  BeginRequest(request);
  const auto ticket = Ticket(Value(row, "ticket_id"));
  Ensure(Value(row, "status") == "pending" && !Blocked(ticket, true) &&
             clock_() < Number(row, "payment_due") && clock_() < Number(ticket, "deadline"),
         "收款确认失败");
  Ensure(amount_cents == Number(ticket, "price") && amount_cents > 0, "实收金额应等于票价（分）");
  database_.Execute(
      "INSERT INTO receipts(registration_id,amount,confirmed,actor) VALUES(?,?,?,?)",
      {registration, std::to_string(amount_cents), std::to_string(clock_()), user_id_});
  Transition(row, "registered", "receive", request);
  database_.Execute("UPDATE registrations SET confirmed=? WHERE id=?",
                    {std::to_string(clock_()), registration});
  EndRequest(request, registration);
  transaction.Commit();
}

void Service::AbnormalReceipt(const std::string& registration, int amount_cents,
                              const std::string& request) {
  Transaction transaction(database_);
  const auto row = Registration(registration);
  Own(Value(row, "event_id"));
  BeginRequest(request);
  const auto ticket = Ticket(Value(row, "ticket_id"));
  Ensure(amount_cents > 0 && Number(ticket, "price") > 0, "异常实收金额或票种不合法");
  Ensure(Value(row, "status") == "cancelled" ||
             (Value(row, "status") == "pending" &&
              (clock_() >= Number(row, "payment_due") || clock_() >= Number(ticket, "deadline"))),
         "仅记录超时或取消后的异常实收");
  if (Value(row, "status") == "pending") {
    Transition(row, "cancelled", "payment_timeout", request);
  }
  database_.Execute(
      "INSERT INTO receipts(registration_id,amount,confirmed,actor,abnormal) VALUES(?,?,?,?,1)",
      {registration, std::to_string(amount_cents), std::to_string(clock_()), user_id_});
  Refund(registration, "超时或取消后异常实收");
  Log(Value(row, "event_id"), Value(row, "user_id"), "registration", registration,
      "abnormal_receipt", "", std::to_string(amount_cents), request);
  Promote(Value(row, "ticket_id"), request);
  EndRequest(request, registration);
  transaction.Commit();
}

void Service::CompleteRefund(const std::string& refund, const std::string& request) {
  Transaction transaction(database_);
  const auto row = database_.One(
      "SELECT f.*,r.event_id,r.user_id FROM refunds f "
      "JOIN receipts p ON p.id=f.receipt_id JOIN registrations r ON r.id=p.registration_id WHERE "
      "f.id=?",
      {refund});
  Own(Value(row, "event_id"));
  BeginRequest(request);
  Ensure(Value(row, "status") == "pending", "请求失败");
  database_.Execute("UPDATE refunds SET status='completed',completed=?,actor=? WHERE id=?",
                    {std::to_string(clock_()), user_id_, refund});
  Log(Value(row, "event_id"), Value(row, "user_id"), "refund", refund, "complete_refund", "pending",
      "completed", request);
  EndRequest(request, refund);
  transaction.Commit();
}

bool Service::CheckerConflict(const std::string& checker, const Row& session) {
  // Use half-open intervals [start,end): adjoining sessions do not overlap.
  return !database_
              .Query(
                  R"(SELECT a.session_id FROM assignments a
      JOIN sessions s ON s.id=a.session_id JOIN events e ON e.id=s.event_id
      WHERE a.checker_id=? AND s.id<>? AND e.status='published' AND s.cancelled=0
      AND s.start<? AND s.end>?
      AND EXISTS(SELECT 1 FROM tickets t WHERE t.session_id=s.id AND t.cancelled=0)
      LIMIT 1)",
                  {checker, Value(session, "id"), Value(session, "end"), Value(session, "start")})
              .empty();
}

void Service::ValidateAssignmentSchedule(const std::string& event) {
  for (const auto& session : database_.Query(R"(SELECT s.* FROM sessions s
      JOIN events e ON e.id=s.event_id WHERE s.event_id=? AND e.status='published'
      AND s.cancelled=0 AND EXISTS(SELECT 1 FROM tickets t
      WHERE t.session_id=s.id AND t.cancelled=0))",
                                             {event})) {
    for (const auto& assignment : database_.Query(
             "SELECT checker_id FROM assignments WHERE session_id=?", {Value(session, "id")})) {
      Ensure(!CheckerConflict(Value(assignment, "checker_id"), session),
             "修改后的场次时间与检票人员的其他分配冲突，请调整时间或先撤销冲突分配");
    }
  }
}

Rows Service::AssignmentSessions() {
  Require("admin");
  return database_.Query(R"(SELECT s.*,e.name AS event_name,e.status AS event_status,
      (SELECT count(*) FROM assignments a WHERE a.session_id=s.id) AS assigned_count,
      CASE WHEN e.status='published' AND s.cancelled=0 AND s.end>?
        AND EXISTS(SELECT 1 FROM tickets t WHERE t.session_id=s.id AND t.cancelled=0)
        THEN 1 ELSE 0 END AS can_assign
      FROM sessions s JOIN events e ON e.id=s.event_id
      WHERE e.status='published' OR EXISTS(SELECT 1 FROM assignments a WHERE a.session_id=s.id)
      ORDER BY s.start,s.id)",
                         {std::to_string(clock_())});
}

Rows Service::AssignmentCheckers(const std::string& session, bool assigned) {
  Require("admin");
  const auto parent = Session(session);
  const std::string membership = assigned ? "EXISTS" : "NOT EXISTS";
  auto users = database_.Query(
      "SELECT u.id,u.username,u.name,u.role,u.enabled FROM users u "
      "WHERE u.role='checker'" +
          std::string(assigned ? "" : " AND u.enabled=1") + " AND " + membership +
          "(SELECT 1 FROM assignments a WHERE a.session_id=? "
          "AND a.checker_id=u.id) ORDER BY u.id",
      {session});
  if (!assigned) {
    users.erase(
        std::remove_if(users.begin(), users.end(),
                       [&](const Row& user) { return CheckerConflict(Value(user, "id"), parent); }),
        users.end());
  }
  return users;
}

void Service::Assign(const std::string& session, const std::string& checker, bool remove,
                     const std::string& request) {
  Transaction transaction(database_);
  Require("admin");
  BeginRequest(request);
  const auto parent = Session(session);
  const auto user = database_.One("SELECT role,enabled FROM users WHERE id=?", {checker});
  Ensure(Value(user, "role") == "checker", "目标账号不是检票人员");
  const bool exists = !database_
                           .Query("SELECT 1 FROM assignments WHERE session_id=? AND checker_id=?",
                                  {session, checker})
                           .empty();
  if (remove) {
    Ensure(exists, "该检票人员未分配到此场次，请刷新列表");
    // Disabled accounts and historical assignments must remain removable.
    database_.Execute("DELETE FROM assignments WHERE session_id=? AND checker_id=?",
                      {session, checker});
  } else {
    Ensure(!exists, "该检票人员已分配到此场次，无需重复分配");
    Ensure(Value(user, "enabled") == "1", "检票账号已停用，不能新增分配");
    Ensure(Value(Event(Value(parent, "event_id")), "status") == "published" &&
               Value(parent, "cancelled") == "0" && clock_() < Number(parent, "end"),
           "只能为已发布且未取消、未结束的场次新增分配");
    Ensure(!database_.Query("SELECT id FROM tickets WHERE session_id=? AND cancelled=0", {session})
                .empty(),
           "场次没有可用票种");
    Ensure(!CheckerConflict(checker, parent),
           "该检票人员已分配到时间重叠的场次，请另选人员或先撤销冲突分配");
    database_.Execute("INSERT INTO assignments VALUES(?,?)", {session, checker});
  }
  Log(Value(parent, "event_id"), checker, "session", session,
      remove ? "remove_checker" : "assign_checker", remove ? checker : "", remove ? "" : checker,
      request);
  EndRequest(request, session);
  transaction.Commit();
}

void Service::CheckIn(const std::string& registration, const std::string& request) {
  Transaction transaction(database_);
  Require("checker");
  BeginRequest(request);
  const auto row = Registration(registration);
  const auto ticket = Ticket(Value(row, "ticket_id"));
  Ensure(!database_
              .Query("SELECT * FROM assignments WHERE session_id=? AND checker_id=?",
                     {Value(row, "session_id"), user_id_})
              .empty(),
         "请求失败");
  Ensure(Value(row, "status") == "registered" && !Blocked(ticket, true) &&
             clock_() >= Number(ticket, "start") && clock_() < Number(ticket, "end"),
         "请求失败");
  Transition(row, "checked", "check_in", request);
  database_.Execute("UPDATE registrations SET checked_at=? WHERE id=?",
                    {std::to_string(clock_()), registration});
  EndRequest(request, registration);
  transaction.Commit();
}

Rows Service::Registrations(const std::string& event, const Search& filter) {
  const auto user = Require();
  const auto role = Value(user, "role");
  Ensure(role != "admin", "管理员不代组织者查询报名名单");
  std::string sql = kRegistrationSelect;
  Params parameters;
  if (role == "participant") {
    sql += " AND r.user_id=?";
    parameters.push_back(user_id_);
  } else if (role == "organizer") {
    sql += " AND e.owner_id=?";
    parameters.push_back(user_id_);
  } else {
    sql +=
        " AND EXISTS(SELECT 1 FROM assignments a WHERE a.session_id=r.session_id AND "
        "a.checker_id=?)";
    parameters.push_back(user_id_);
  }
  if (!event.empty()) {
    sql += " AND r.event_id=?";
    parameters.push_back(event);
  }
  if (!filter.session.empty()) {
    sql += " AND r.session_id=?";
    parameters.push_back(filter.session);
  }
  if (!filter.status.empty()) {
    sql += " AND r.status=?";
    parameters.push_back(filter.status);
  }
  if (!filter.keyword.empty()) {
    sql += " AND (instr(u.name,?)>0 OR CAST(r.id AS TEXT)=?)";
    parameters.push_back(filter.keyword);
    parameters.push_back(filter.keyword);
  }
  auto rows = database_.Query(sql + " ORDER BY r.id", parameters);
  if (role == "checker") {
    for (auto& row : rows) {
      for (const auto& key : {"received", "refund_pending", "refunded", "price", "payment_due"}) {
        row.erase(key);
      }
    }
  }
  return rows;
}

Rows Service::Refunds(const std::string& event) {
  Own(event);
  return database_.Query(
      "SELECT f.*,p.amount,p.registration_id FROM refunds f JOIN receipts p "
      "ON p.id=f.receipt_id JOIN registrations r ON r.id=p.registration_id WHERE r.event_id=? "
      "ORDER BY f.id",
      {event});
}

Rows Service::Statistics(const std::string& event) {
  Own(event);
  Rows output;
  for (const auto& session : database_.Query("SELECT * FROM sessions WHERE event_id=?", {event})) {
    for (auto ticket :
         database_.Query("SELECT * FROM tickets WHERE session_id=?", {Value(session, "id")})) {
      const auto id = Value(ticket, "id");
      ticket["session_name"] = Value(session, "name");
      std::int64_t occupied = 0;
      for (const auto& status :
           {"waiting", "pending", "registered", "checked", "absent", "cancelled"}) {
        const auto count = database_.Scalar(
            "SELECT count(*) FROM registrations WHERE ticket_id=? AND status=?", {id, status});
        ticket[status] = count;
        if (std::string(status) == "pending" || std::string(status) == "registered" ||
            std::string(status) == "checked") {
          occupied += std::stoll(count);
        }
      }
      ticket["occupied"] = std::to_string(occupied);
      const bool closed = clock_() >= Number(session, "end") || Value(ticket, "cancelled") == "1" ||
                          Value(session, "cancelled") == "1" ||
                          Value(Event(event), "status") == "cancelled";
      ticket["remaining"] = closed ? "0" : std::to_string(Number(ticket, "capacity") - occupied);
      ticket["note"] = closed ? "已结束或取消，不再分配" : "";
      output.push_back(std::move(ticket));
    }
  }
  return output;
}

Rows Service::Logs(const std::string& event, const std::string& action, const std::string& actor,
                   std::int64_t since) {
  const auto user = Require();
  const auto role = Value(user, "role");
  Ensure(role != "checker", "检票角色不开放日志查询");
  std::string sql = "SELECT l.* FROM logs l WHERE l.time>=?";
  Params parameters{std::to_string(since)};
  if (role == "participant") {
    sql += " AND l.user_id=?";
    parameters.push_back(user_id_);
  }
  if (role == "organizer") {
    sql += " AND l.event_id IN(SELECT id FROM events WHERE owner_id=?)";
    parameters.push_back(user_id_);
  }
  if (!event.empty()) {
    sql += " AND l.event_id=?";
    parameters.push_back(event);
  }
  if (!action.empty()) {
    sql += " AND l.action=?";
    parameters.push_back(action);
  }
  if (!actor.empty()) {
    sql += " AND l.actor=?";
    parameters.push_back(actor);
  }
  return database_.Query(sql + " ORDER BY l.id DESC", parameters);
}

Rows Service::Notifications() {
  Require("participant");
  return database_.Query("SELECT * FROM notifications WHERE user_id=? ORDER BY id DESC",
                         {user_id_});
}

std::string Service::OperationResult(const std::string& request) {
  Require();
  const auto rows =
      database_.Query("SELECT result FROM requests WHERE id=? AND actor=?", {request, user_id_});
  return rows.empty() ? "unknown" : Value(rows.front(), "result");
}

void Service::ExportCsv(const std::string& event, const std::string& destination,
                        const std::string& session, const std::string& ticket,
                        const std::string& status) {
  Own(event);
  Search filter;
  filter.status = status;
  const auto rows = Registrations(event, filter);
  std::ofstream output(destination, std::ios::binary);
  Ensure(output.good(), "无法写入导出文件");
  output << "\xEF\xBB\xBF报名编号,姓名,场次,票种,座位号,报名状态,实收分,待退款分,已退款分,"
            "首次签到时间\r\n";
  for (const auto& row : rows) {
    if ((!session.empty() && Value(row, "session_id") != session) ||
        (!ticket.empty() && Value(row, "ticket_id") != ticket)) {
      continue;
    }
    const Params cells{Value(row, "id"),
                       Value(row, "participant"),
                       Value(row, "session_name"),
                       Value(row, "ticket_name"),
                       Value(row, "seat"),
                       StatusText(Value(row, "status")),
                       Value(row, "received"),
                       Value(row, "refund_pending"),
                       Value(row, "refunded"),
                       Value(row, "checked_at").empty() ? "" : TimeText(Number(row, "checked_at"))};
    for (std::size_t i = 0; i < cells.size(); ++i) {
      output << (i ? "," : "") << CsvCell(cells[i]);
    }
    output << "\r\n";
  }
  output.flush();
  Ensure(output.good(), "导出写入失败");
}

void Service::Allocate(const Row& session, const std::string& request) {
  if (clock_() < Number(session, "deadline") || Value(session, "cancelled") == "1") {
    return;
  }
  const auto session_id = Value(session, "id");
  for (const auto& ticket :
       database_.Query("SELECT id FROM tickets WHERE session_id=? AND cancelled=0", {session_id})) {
    const auto ticket_id = Value(ticket, "id");
    if (Blocked(Ticket(ticket_id), true)) {
      continue;
    }
    const auto seats = database_.Query(
        "SELECT z.id FROM seats z WHERE z.ticket_id=? AND NOT EXISTS "
        "(SELECT 1 FROM registrations r WHERE r.seat_id=z.id) ORDER BY z.label",
        {ticket_id});
    const auto registrations = database_.Query(
        "SELECT * FROM registrations WHERE ticket_id=? "
        "AND status='registered' AND seat_id IS NULL ORDER BY confirmed,id",
        {ticket_id});
    Ensure(seats.size() >= registrations.size(), "可用座位不足");
    for (std::size_t i = 0; i < registrations.size(); ++i) {
      const auto& row = registrations[i];
      database_.Execute("UPDATE registrations SET seat_id=? WHERE id=?",
                        {Value(seats[i], "id"), Value(row, "id")});
      Log(Value(row, "event_id"), Value(row, "user_id"), "registration", Value(row, "id"),
          "allocate_seat", "", Value(seats[i], "id"), request);
    }
  }
}

void Service::Tick() {
  // Scheduler is an internal entry point. Its principal is always system, even
  // when a logged-in desktop window triggers the periodic timer.
  const auto saved_user = user_id_;
  user_id_.clear();
  try {
    Transaction transaction(database_);
    const auto request = "timer-" + RequestId();
    for (const auto& row : database_.Query("SELECT * FROM registrations WHERE status IN "
                                           "('waiting','pending','registered') ORDER BY id")) {
      const auto session = Session(Value(row, "session_id"));
      const auto status = Value(row, "status");
      if ((status == "waiting" || status == "pending") &&
          (clock_() >= Number(session, "deadline") ||
           (status == "pending" && clock_() >= Number(row, "payment_due")))) {
        Transition(row, "cancelled", "timeout_or_deadline", request);
      } else if (status == "registered" && clock_() >= Number(session, "end") &&
                 Value(session, "cancelled") == "0") {
        Transition(row, "absent", "mark_absent", request);
      }
    }
    for (const auto& review : database_.Query(
             "SELECT * FROM reviews WHERE kind='cancel' AND status='pending' AND scope<>'event'")) {
      const auto scope = Value(review, "scope");
      const auto target = Value(review, "target_id");
      const auto child = scope == "session" ? Session(target) : Ticket(target);
      if (clock_() >= Number(child, "end")) {
        database_.Execute("UPDATE reviews SET status='expired' WHERE id=?", {Value(review, "id")});
        Log(Value(review, "event_id"), "", "review", Value(review, "id"), "expire_review",
            "pending", "expired", request);
      }
    }
    for (const auto& ticket : database_.Query("SELECT id FROM tickets ORDER BY id")) {
      Promote(Value(ticket, "id"), request);
    }
    for (const auto& session : database_.Query("SELECT * FROM sessions ORDER BY id")) {
      Allocate(session, request);
    }
    transaction.Commit();
    if (path_ != ":memory:") {
      const auto backup = path_ + ".daily-" + TimeText(clock_()).substr(0, 10) + ".bak";
      if (!std::filesystem::exists(backup)) {
        database_.Backup(backup);
      }
    }
    user_id_ = saved_user;
  } catch (...) {
    user_id_ = saved_user;
    throw;
  }
}

void Service::Backup(const std::string& destination) {
  Require("admin");
  database_.CheckIntegrity();
  database_.Backup(destination);
}

void Service::CheckIntegrity() {
  Require("admin");
  database_.CheckIntegrity();
}

void Service::SeedDemo() {
  Ensure(database_.Scalar("SELECT count(*) FROM users") == "0", "演示数据只可写入空数据库");
  Bootstrap("admin", "Campus2026!");
  Login("admin", "Campus2026!");
  CreateUser("organizer", "校园文艺社", "organizer", "Campus2026!", RequestId());
  CreateUser("student", "程同学", "participant", "Campus2026!", RequestId());
  CreateUser("student2", "林同学", "participant", "Campus2026!", RequestId());
  CreateUser("checker", "入口检票员", "checker", "Campus2026!", RequestId());
  Login("organizer", "Campus2026!");
  const auto event = CreateEvent("秋日草坪音乐会", "文艺演出",
                                 "校园乐队与民谣之夜。仅登记线下收退款。", RequestId());
  const auto now = clock_();
  const auto session =
      AddSession(event, {"第一场", "仙林校区四组团体育场", now + 7200, now + 10800, now + 18000, 5},
                 RequestId());
  AddTicket(session, "普通票", 2000, 3, {"A01", "A02", "A03"}, RequestId());
  AddTicket(session, "前排票", 3500, 2, {"B01", "B02"}, RequestId());
  const auto review = Submit(event, RequestId());
  Login("admin", "Campus2026!");
  Decide(review, true, "信息完整", RequestId());
  Assign(session, "5", false, RequestId());
  Login("organizer", "Campus2026!");
  const auto lecture =
      CreateEvent("大气活性物种的检测及应用", "校园讲座", "欢迎同学们参加学术交流。", RequestId());
  const auto lecture_session =
      AddSession(lecture, {"报告场", "大气楼D103报告厅", now + 86400, now + 90000, now + 93600, 3},
                 RequestId());
  AddTicket(lecture_session, "免费票", 0, 3, {"C01", "C02", "C03"}, RequestId());
  const auto lecture_review = Submit(lecture, RequestId());
  Login("admin", "Campus2026!");
  Decide(lecture_review, true, "通过", RequestId());
  Login("student", "Campus2026!");
  Register("1", RequestId());
  Login("student2", "Campus2026!");
  const auto paid = Register("1", RequestId());
  Login("organizer", "Campus2026!");
  Receive(paid, 2000, RequestId());
  Login("student2", "Campus2026!");
  Cancel(paid, RequestId());
  Register("3", RequestId());
  Logout();
}

}  // namespace campus
