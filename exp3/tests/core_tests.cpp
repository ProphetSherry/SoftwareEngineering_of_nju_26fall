#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "campus/service.h"

namespace {
int assertions = 0;

void Check(bool condition, const std::string& message) {
  ++assertions;
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void Reject(const std::function<void()>& operation, const std::string& message) {
  ++assertions;
  try {
    operation();
  } catch (const campus::Error&) {
    return;
  }
  throw std::runtime_error("Expected rejection: " + message);
}

struct Fixture {
  std::string path = "/tmp/campus-test-" + campus::RequestId() + ".db";
  std::int64_t time = 1900000000;
  campus::Service service{path, [this] { return time; }};
  std::string event;
  std::string session;
  std::string ticket;
  std::string checker;

  Fixture() {
    service.Bootstrap("admin", "TestPass2026!");
    service.Login("admin", "TestPass2026!");
    service.CreateUser("owner", "组织者", "organizer", "TestPass2026!", campus::RequestId());
    service.CreateUser("other", "另一组织者", "organizer", "TestPass2026!", campus::RequestId());
    checker =
        service.CreateUser("checker", "检票员", "checker", "TestPass2026!", campus::RequestId());
    for (int i = 0; i < 8; ++i) {
      const auto name = "p" + std::to_string(i);
      service.CreateUser(name, i == 0 ? "=HYPERLINK(1)" : name, "participant", "TestPass2026!",
                         campus::RequestId());
    }
    Login("owner");
  }

  ~Fixture() {
    for (const auto& file : std::filesystem::directory_iterator("/tmp")) {
      if (file.path().string().find(path + ".daily-") == 0) {
        std::error_code ignored;
        std::filesystem::remove(file.path(), ignored);
      }
    }
    // The connection is closed after this body; POSIX permits removing open files.
    for (const auto& suffix : {"", "-wal", "-shm", ".bak", ".csv"}) {
      std::error_code ignored;
      std::filesystem::remove(path + suffix, ignored);
    }
  }

  void Login(const std::string& name) { service.Login(name, "TestPass2026!"); }

  void Publish(int price = 0, int capacity = 1) {
    Login("owner");
    event = service.CreateEvent("音乐会", "文艺", "校园音乐会", campus::RequestId());
    session = service.AddSession(
        event, {"第一场", "仙林", time + 3600, time + 7200, time + 10800, capacity},
        campus::RequestId());
    campus::Params seats;
    for (int i = 0; i < capacity; ++i) {
      seats.push_back("A0" + std::to_string(i));
    }
    ticket = service.AddTicket(session, "普通票", price, capacity, seats, campus::RequestId());
    const auto review = service.Submit(event, campus::RequestId());
    Login("admin");
    service.Decide(review, true, "ok", campus::RequestId());
    service.Assign(session, checker, false, campus::RequestId());
  }

  std::string Register(int participant) {
    Login("p" + std::to_string(participant));
    return service.Register(ticket, campus::RequestId());
  }

  campus::Row Registration(const std::string& id) {
    Login("owner");
    for (const auto& row : service.Registrations(event)) {
      if (campus::Value(row, "id") == id) {
        return row;
      }
    }
    throw std::runtime_error("Missing registration");
  }
};

void TestQueueAndCancellation() {
  Fixture f;
  f.Publish();
  const auto first = f.Register(0);
  const auto second = f.Register(1);
  const auto third = f.Register(2);
  Check(campus::Value(f.Registration(second), "status") == "waiting", "full -> waiting");
  f.Login("p0");
  f.service.Cancel(first, campus::RequestId());
  Check(campus::Value(f.Registration(second), "status") == "registered", "first waiter promoted");
  Check(campus::Value(f.Registration(third), "status") == "waiting", "FIFO remains");
  f.Login("p0");
  Reject([&] { f.service.Register(f.ticket, campus::RequestId()); },
         "cancelled cannot re-register");
  f.Login("owner");
  Check(f.service.Refunds(f.event).empty(), "free cancellation no refund");
  Check(campus::Value(f.service.Statistics(f.event).front(), "occupied") == "1", "one occupied");
}

void TestPaymentAndRefund() {
  Fixture f;
  f.Publish(2000);
  const auto first = f.Register(0);
  const auto second = f.Register(1);
  Check(campus::Value(f.Registration(first), "status") == "pending", "paid ticket pending");
  f.Login("other");
  Reject([&] { f.service.Receive(first, 2000, campus::RequestId()); }, "cross owner money");
  f.Login("owner");
  Reject([&] { f.service.Receive(first, 1999, campus::RequestId()); }, "wrong amount");
  const auto request = campus::RequestId();
  f.service.Receive(first, 2000, request);
  Check(f.service.OperationResult(request) == first, "query successful request");
  const auto logs = f.service.Logs().size();
  Reject([&] { f.service.Receive(first, 2000, request); }, "duplicate request");
  Check(f.service.Logs().size() == logs, "failure creates no log");
  f.Login("p0");
  f.service.Cancel(first, campus::RequestId());
  Check(campus::Value(f.Registration(second), "status") == "pending", "paid waiter pending");
  Check(campus::Number(f.Registration(second), "payment_due") == f.time + 1800,
        "promotion starts payment timer");
  f.Login("owner");
  const auto refund = campus::Value(f.service.Refunds(f.event).front(), "id");
  f.service.CompleteRefund(refund, campus::RequestId());
  Reject([&] { f.service.CompleteRefund(refund, campus::RequestId()); }, "duplicate refund");
  Check(campus::Value(f.Registration(first), "status") == "cancelled",
        "refund never restores state");
}

void TestTimeoutBoundary() {
  Fixture f;
  f.Publish(2000);
  const auto first = f.Register(0);
  const auto second = f.Register(1);
  f.time += 1800;
  f.Login("owner");
  Reject([&] { f.service.Receive(first, 2000, campus::RequestId()); }, "exact payment deadline");
  f.service.Tick();
  Check(campus::Value(f.Registration(first), "status") == "cancelled", "timeout cancel");
  Check(campus::Value(f.Registration(second), "status") == "pending", "timeout promote");
  f.service.AbnormalReceipt(first, 2000, campus::RequestId());
  Check(f.service.Refunds(f.event).size() == 1, "late actual payment refunded");
  Check(campus::Value(f.Registration(first), "status") == "cancelled", "late receipt no recovery");
  f.time += 1800;
  f.service.Tick();
  Check(campus::Value(f.Registration(second), "status") == "cancelled", "cutoff cancels pending");
  f.Login("p2");
  Reject([&] { f.service.Register(f.ticket, campus::RequestId()); }, "exact cutoff rejects");
}

void TestSeatsCheckInAbsence() {
  Fixture f;
  f.Publish(0, 2);
  const auto first = f.Register(0);
  const auto second = f.Register(1);
  f.time += 3600;
  f.service.Tick();
  const auto first_seat = campus::Value(f.Registration(first), "seat");
  const auto second_seat = campus::Value(f.Registration(second), "seat");
  Check(first_seat == "A00" && second_seat == "A01", "deterministic seats");
  f.service.Tick();
  Check(campus::Value(f.Registration(first), "seat") == first_seat, "allocation idempotent");
  f.Login("checker");
  Reject([&] { f.service.CheckIn(first, campus::RequestId()); }, "before start");
  f.time += 3600;
  f.service.CheckIn(first, campus::RequestId());
  Reject([&] { f.service.CheckIn(first, campus::RequestId()); }, "duplicate checkin");
  Check(f.service.Registrations().front().count("received") == 0, "checker money hidden");
  f.time += 3600;
  f.service.Tick();
  Check(campus::Value(f.Registration(first), "status") == "checked", "checked terminal");
  Check(campus::Value(f.Registration(second), "status") == "absent", "unattended absent");
  f.Login("owner");
  Check(campus::Value(f.service.Statistics(f.event).front(), "remaining") == "0",
        "ended available zero");
}

void TestReviewGates() {
  Fixture f;
  f.Publish(2000);
  const auto registration = f.Register(0);
  f.Login("owner");
  const auto edit = f.service.ProposeEdit(f.event, "新名称", "文艺", "新介绍", campus::RequestId());
  Check(campus::Value(f.service.Events().front(), "name") == "音乐会", "candidate not published");
  Reject([&] { f.service.RequestCancellation(f.event, "event", f.event, campus::RequestId()); },
         "review mutual exclusion");
  f.Login("p1");
  Reject([&] { f.service.Register(f.ticket, campus::RequestId()); }, "edit pauses registration");
  f.Login("owner");
  f.service.Receive(registration, 2000, campus::RequestId());
  f.Login("admin");
  Reject([&] { f.service.Decide(edit, false, "", campus::RequestId()); },
         "rejection reason required");
  f.service.Decide(edit, false, "不完整", campus::RequestId());
  f.Login("owner");
  const auto cancel = f.service.RequestCancellation(f.event, "event", f.event, campus::RequestId());
  f.Login("admin");
  f.service.Decide(cancel, true, "批准", campus::RequestId());
  Check(campus::Value(f.Registration(registration), "status") == "cancelled",
        "cancel review applied");
  f.Login("owner");
  Check(f.service.Refunds(f.event).size() == 1, "cancel review creates refund");
}

void TestEditAndNotifications() {
  Fixture f;
  f.Publish();
  f.Register(0);
  f.Login("owner");
  const auto edit =
      f.service.ProposeEdit(f.event, "新音乐会", "文艺", "新介绍", campus::RequestId());
  f.service.ProposeSession(edit, f.session,
                           {"第一场", "新地点", f.time + 4000, f.time + 8000, f.time + 12000, 1},
                           campus::RequestId());
  f.Login("admin");
  f.service.Decide(edit, true, "批准", campus::RequestId());
  Check(f.service.Versions(f.event).size() == 2, "immutable versions");
  f.Login("p0");
  Check(f.service.Notifications().size() == 1, "affected participant notified");
  Check(campus::Value(f.service.Sessions(f.event).front(), "location") == "新地点",
        "new location active");
}

void TestExpiryAndPrivacy() {
  Fixture f;
  f.Publish();
  const auto registration = f.Register(0);
  f.Login("p1");
  Check(f.service.Registrations().empty(), "participant privacy");
  Reject([&] { f.service.Cancel(registration, campus::RequestId()); }, "cross user cancel");
  f.Login("other");
  Reject([&] { f.service.Statistics(f.event); }, "cross owner stats");
  Check(f.service.Registrations(f.event).empty(), "cross owner list empty");
  f.Login("owner");
  f.service.RequestCancellation(f.event, "session", f.session, campus::RequestId());
  f.time += 10800;
  f.service.Tick();
  Check(campus::Value(f.Registration(registration), "status") == "absent",
        "pause crossing end still absent");
  f.Login("admin");
  Check(campus::Value(f.service.Reviews().front(), "status") == "expired",
        "session cancellation expires");
  f.service.SetEnabled("2", false, campus::RequestId());
  Reject([&] { f.Login("owner"); }, "disabled login denied");
  f.service.Logout();
  Reject([&] { f.service.Events(); }, "anonymous query denied");
}

void TestRollbackBackupCsv() {
  Fixture f;
  f.Publish(2000);
  const auto registration = f.Register(0);
  f.Login("owner");
  campus::Database fault(f.path);
  fault.Execute(
      "CREATE TRIGGER fail_log BEFORE INSERT ON logs BEGIN SELECT RAISE(ABORT,'injected failure'); "
      "END");
  Reject([&] { f.service.Receive(registration, 2000, campus::RequestId()); },
         "log failure rollback");
  Check(fault.Scalar("SELECT count(*) FROM receipts") == "0", "receipt rolled back");
  Check(campus::Value(f.Registration(registration), "status") == "pending", "state rolled back");
  fault.Execute("DROP TRIGGER fail_log");
  f.service.Receive(registration, 2000, campus::RequestId());
  f.service.ExportCsv(f.event, f.path + ".csv");
  std::ifstream input(f.path + ".csv");
  const std::string csv((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  Check(csv.find("'=HYPERLINK") != std::string::npos, "CSV formula neutralized");
  f.Login("admin");
  f.service.Backup(f.path + ".bak");
  campus::Service restored(f.path + ".bak", [&f] { return f.time; });
  restored.Login("admin", "TestPass2026!");
  restored.CheckIntegrity();
  Check(restored.Logs().size() == f.service.Logs().size(), "backup logs preserved");
  restored.Login("owner", "TestPass2026!");
  Check(campus::Value(restored.Registrations().front(), "received") == "2000",
        "backup funds preserved");
}

void TestConcurrentLastSeat() {
  Fixture f;
  f.Publish();
  std::atomic<int> ready{0};
  std::atomic<bool> start{false};
  std::atomic<int> failures{0};
  std::vector<std::thread> workers;
  for (int i = 0; i < 8; ++i) {
    workers.emplace_back([&, i] {
      try {
        campus::Service connection(f.path, [&f] { return f.time; });
        connection.Login("p" + std::to_string(i), "TestPass2026!");
        ++ready;
        while (!start.load()) {
          std::this_thread::yield();
        }
        connection.Register(f.ticket, campus::RequestId());
      } catch (...) {
        ++failures;
        ++ready;
      }
    });
  }
  while (ready.load() < 8) {
    std::this_thread::yield();
  }
  start = true;
  for (auto& worker : workers) {
    worker.join();
  }
  Check(failures == 0, "all concurrent requests finish");
  f.Login("owner");
  const auto stats = f.service.Statistics(f.event).front();
  Check(campus::Value(stats, "occupied") == "1", "no oversell");
  Check(campus::Value(stats, "waiting") == "7", "remaining users queued");
  f.Login("admin");
  f.service.CheckIntegrity();
}

void TestDraftValidation() {
  Fixture f;
  const auto event = f.service.CreateEvent("草稿", "", "", campus::RequestId());
  Reject([&] { f.service.Submit(event, campus::RequestId()); }, "incomplete draft");
  f.service.EditDraft(event, "草稿", "讲座", "介绍", campus::RequestId());
  const auto session = f.service.AddSession(
      event, {"场次", "地点", f.time + 1, f.time + 2, f.time + 3, 1}, campus::RequestId());
  Reject([&] { f.service.AddTicket(session, "票", -1, 1, {"A"}, campus::RequestId()); },
         "negative price");
  Reject([&] { f.service.AddTicket(session, "票", 0, 2, {"A", "A"}, campus::RequestId()); },
         "overlap seats");
  Check(f.service.Tickets(session).empty(), "invalid ticket fully rolled back");
  f.service.AddTicket(session, "票", 0, 2, {"A", "B"}, campus::RequestId());
  Reject([&] { f.service.Submit(event, campus::RequestId()); }, "ticket sum exceeds capacity");
}
void TestDraftEditing() {
  Fixture f;
  const auto event = f.service.CreateEvent("草稿", "讲座", "介绍", campus::RequestId());
  const auto session = f.service.AddSession(
      event, {"一", "旧地点", f.time + 100, f.time + 200, f.time + 300, 2}, campus::RequestId());
  const auto ticket = f.service.AddTicket(session, "旧票", 100, 1, {"A"}, campus::RequestId());
  f.service.EditSessionDraft(session,
                             {"新场次", "新地点", f.time + 100, f.time + 200, f.time + 300, 3},
                             campus::RequestId());
  f.service.EditTicketDraft(ticket, "新票", 0, {"B", "C"}, campus::RequestId());
  Check(campus::Value(f.service.Tickets(session).front(), "capacity") == "2",
        "ticket edit seats and capacity");
  Reject([&] { f.service.EditTicketDraft(ticket, "错误", 0, {"D", "D"}, campus::RequestId()); },
         "duplicate replacement seats");
  Check(campus::Value(f.service.Tickets(session).front(), "name") == "新票",
        "bad edit rolled back");
  f.service.DeleteDraftChild("ticket", ticket, campus::RequestId());
  Check(f.service.Tickets(session).empty(), "delete draft ticket");
  f.service.DeleteDraftChild("session", session, campus::RequestId());
  f.service.DeleteDraft(event, campus::RequestId());
  Check(f.service.Events().empty(), "delete draft hierarchy");
}

void TestPublishedTicketProtection() {
  Fixture f;
  f.Publish(2000);
  f.Register(0);
  f.Login("owner");
  auto review = f.service.ProposeEdit(f.event, "音乐会", "文艺", "新介绍", campus::RequestId());
  f.service.ProposeTicket(review, f.session, f.ticket, "普通票", 3000, {"A00"},
                          campus::RequestId());
  Check(f.service.ReviewDetails(review).find("3000") != std::string::npos,
        "candidate visible for review");
  f.Login("admin");
  Reject([&] { f.service.Decide(review, true, "ok", campus::RequestId()); },
         "price protected after registration");
  Check(f.service.Versions(f.event).size() == 1, "failed approval no version");
  f.Login("owner");
  f.service.Withdraw(review, campus::RequestId());
  review = f.service.ProposeEdit(f.event, "音乐会", "文艺", "新介绍", campus::RequestId());
  f.service.ProposeSession(review, f.session,
                           {"第一场", "仙林", f.time + 3600, f.time + 7200, f.time + 10800, 2},
                           campus::RequestId());
  f.service.ProposeTicket(review, f.session, "", "新票价", 3000, {"B01"}, campus::RequestId());
  f.Login("admin");
  f.service.Decide(review, true, "ok", campus::RequestId());
  Check(f.service.Tickets(f.session).size() == 2, "new price uses new ticket");
  Check(campus::Value(f.service.Tickets(f.session).front(), "price") == "2000",
        "old ticket price unchanged");
}

void TestOriginalTicketQueue() {
  Fixture f;
  f.Login("owner");
  f.event = f.service.CreateEvent("双票种", "文艺", "介绍", campus::RequestId());
  f.session =
      f.service.AddSession(f.event, {"一", "仙林", f.time + 3600, f.time + 7200, f.time + 10800, 2},
                           campus::RequestId());
  f.ticket = f.service.AddTicket(f.session, "A票", 0, 1, {"A"}, campus::RequestId());
  const auto second_ticket =
      f.service.AddTicket(f.session, "B票", 0, 1, {"B"}, campus::RequestId());
  const auto review = f.service.Submit(f.event, campus::RequestId());
  f.Login("admin");
  f.service.Decide(review, true, "ok", campus::RequestId());
  const auto first = f.Register(0);
  const auto waiter = f.Register(1);
  f.Login("p2");
  const auto other = f.service.Register(second_ticket, campus::RequestId());
  f.service.Cancel(other, campus::RequestId());
  Check(campus::Value(f.Registration(waiter), "status") == "waiting", "no cross ticket promotion");
  f.Login("p0");
  f.service.Cancel(first, campus::RequestId());
  Check(campus::Value(f.Registration(waiter), "status") == "registered",
        "original ticket promotes");
}

void TestCancellationGatesAndHistory() {
  Fixture f;
  f.Publish(2000, 2);
  const auto first = f.Register(0);
  const auto second = f.Register(1);
  f.Login("owner");
  f.service.Receive(first, 2000, campus::RequestId());
  const auto review =
      f.service.RequestCancellation(f.event, "ticket", f.ticket, campus::RequestId());
  Reject([&] { f.service.Receive(second, 2000, campus::RequestId()); },
         "cancellation pauses money");
  f.time += 7200;
  f.Login("checker");
  Reject([&] { f.service.CheckIn(first, campus::RequestId()); }, "cancellation pauses gate");
  f.Login("admin");
  f.service.Decide(review, true, "ok", campus::RequestId());
  Check(campus::Value(f.Registration(first), "status") == "cancelled",
        "ticket cancellation applies");
  f.Login("owner");
  Check(f.service.Refunds(f.event).size() == 1, "only actual receipt refunded");
}

void TestAssignmentManagement() {
  Fixture f;
  f.Publish();
  Check(f.service.AssignmentCheckers(f.session, true).size() == 1, "existing checker listed");
  Check(f.service.AssignmentCheckers(f.session, false).empty(),
        "existing checker excluded from new candidates");
  const auto before = f.service.Logs().size();
  try {
    f.service.Assign(f.session, f.checker, false, campus::RequestId());
    Check(false, "duplicate assignment rejected");
  } catch (const campus::Error& error) {
    Check(std::string(error.what()).find("UNIQUE") == std::string::npos,
          "duplicate has domain message");
  }
  Check(f.service.Logs().size() == before, "duplicate creates no log");
  const auto second = f.service.CreateUser("checker2", "第二检票员", "checker", "TestPass2026!",
                                           campus::RequestId());
  Check(f.service.AssignmentCheckers(f.session, false).size() == 1,
        "new checker becomes candidate");
  f.service.Assign(f.session, second, false, campus::RequestId());
  Check(f.service.AssignmentCheckers(f.session, true).size() == 2, "multiple checkers supported");
  f.service.SetEnabled(second, false, campus::RequestId());
  f.service.Assign(f.session, second, true, campus::RequestId());
  Check(f.service.AssignmentCheckers(f.session, true).size() == 1, "disabled checker removable");
  Reject([&] { f.service.Assign(f.session, second, false, campus::RequestId()); },
         "disabled checker not assignable");
  Reject([&] { f.service.Assign(f.session, second, true, campus::RequestId()); },
         "missing assignment removal rejected");
  f.service.Assign(f.session, f.checker, true, campus::RequestId());
  Check(f.service.AssignmentCheckers(f.session, false).size() == 1,
        "removed enabled checker assignable again");
  f.service.Assign(f.session, f.checker, false, campus::RequestId());
  f.time += 10800;
  Check(campus::Value(f.service.AssignmentSessions().front(), "can_assign") == "0",
        "ended session cannot add");
  Check(campus::Value(f.service.AssignmentSessions().front(), "assigned_count") == "1",
        "history remains visible for removal");
  f.service.Assign(f.session, f.checker, true, campus::RequestId());
  Reject([&] { f.service.Assign(f.session, f.checker, false, campus::RequestId()); },
         "ended assignment rejected by service");
  f.Login("owner");
  Reject([&] { f.service.AssignmentSessions(); },
         "organizer cannot list assignment management data");
  Reject([&] { f.service.AssignmentCheckers(f.session, false); },
         "organizer cannot list candidates");
  f.Login("checker");
  Check(f.service.Registrations().empty(), "revoked checker no longer sees registrations");
}

std::vector<std::string> PublishSchedule(Fixture& f,
                                         const std::vector<std::pair<int, int>>& times) {
  f.Login("owner");
  const auto event = f.service.CreateEvent("检票排班", "讲座", "跨场次检票", campus::RequestId());
  std::vector<std::string> sessions;
  for (const auto& time : times) {
    const auto session = f.service.AddSession(
        event, {"排班场次", "仙林", f.time + 1800, f.time + time.first, f.time + time.second, 1},
        campus::RequestId());
    f.service.AddTicket(session, "免费票", 0, 1, {"A01"}, campus::RequestId());
    sessions.push_back(session);
  }
  const auto review = f.service.Submit(event, campus::RequestId());
  f.Login("admin");
  f.service.Decide(review, true, "通过", campus::RequestId());
  return sessions;
}

void TestAssignmentTimeConflicts() {
  Fixture f;
  f.Publish();  // Existing assignment covers [7200,10800).
  const auto sessions =
      PublishSchedule(f, {{3600, 7200}, {10800, 14400}, {8000, 9000}, {6000, 12000}});
  f.service.Assign(sessions[0], f.checker, false, campus::RequestId());
  f.service.Assign(sessions[1], f.checker, false, campus::RequestId());
  Check(f.service.AssignmentCheckers(sessions[0], true).size() == 1,
        "adjoining earlier session allowed");
  Check(f.service.AssignmentCheckers(sessions[1], true).size() == 1,
        "adjoining later session allowed");
  const auto logs = f.service.Logs().size();
  for (int index : {2, 3}) {
    Check(f.service.AssignmentCheckers(sessions[index], false).empty(),
          "conflicting checker excluded from candidates");
    Reject([&] { f.service.Assign(sessions[index], f.checker, false, campus::RequestId()); },
           "overlap rejected by service");
    Check(f.service.AssignmentCheckers(sessions[index], true).empty(),
          "conflicting assignment not saved");
  }
  Check(f.service.Logs().size() == logs, "conflicts create no success logs");
  f.service.Assign(f.session, f.checker, true, campus::RequestId());
  Check(f.service.AssignmentCheckers(sessions[2], false).size() == 1,
        "removing conflict makes checker available");
  f.service.Assign(sessions[2], f.checker, false, campus::RequestId());
  f.service.Assign(sessions[2], f.checker, true, campus::RequestId());
  f.service.Assign(f.session, f.checker, false, campus::RequestId());
  // Cancel the original activity: its retained assignment must not block others.
  f.Login("owner");
  const auto cancel = f.service.RequestCancellation(f.event, "event", f.event, campus::RequestId());
  f.Login("admin");
  f.service.Decide(cancel, true, "取消", campus::RequestId());
  Check(f.service.AssignmentCheckers(f.session, true).size() == 1,
        "cancelled session retains historical assignment");
  Check(f.service.AssignmentCheckers(sessions[2], false).size() == 1,
        "cancelled session no longer blocks overlapping assignment");
  f.service.Assign(sessions[2], f.checker, false, campus::RequestId());
}

void TestAssignmentTimeEditProtection() {
  Fixture f;
  f.Publish();
  const auto sessions = PublishSchedule(f, {{10800, 14400}});
  f.service.Assign(sessions[0], f.checker, false, campus::RequestId());
  const auto all = f.service.AssignmentSessions();
  std::string event;
  for (const auto& row : all) {
    if (campus::Value(row, "id") == sessions[0]) {
      event = campus::Value(row, "event_id");
    }
  }
  f.Login("owner");
  const auto review =
      f.service.ProposeEdit(event, "检票排班", "讲座", "调整时间", campus::RequestId());
  f.service.ProposeSession(review, sessions[0],
                           {"排班场次", "新地点", f.time + 1800, f.time + 10000, f.time + 14400, 1},
                           campus::RequestId());
  f.Login("admin");
  const auto logs = f.service.Logs().size();
  Reject([&] { f.service.Decide(review, true, "批准", campus::RequestId()); },
         "edit cannot create checker conflict");
  Check(campus::Number(f.service.Sessions(event).front(), "start") == f.time + 10800,
        "failed time edit rolled back");
  Check(campus::Value(f.service.Sessions(event).front(), "location") == "仙林",
        "failed location edit rolled back too");
  Check(f.service.Logs().size() == logs, "failed approval creates no log");
  f.service.Assign(sessions[0], f.checker, true, campus::RequestId());
  f.service.Decide(review, true, "已撤销冲突分配", campus::RequestId());
  Check(campus::Number(f.service.Sessions(event).front(), "start") == f.time + 10000,
        "time edit allowed after revocation");
}

void TestConcurrentCheckerSchedule() {
  Fixture f;
  f.Publish();
  const auto sessions = PublishSchedule(f, {{4000, 6000}, {5000, 7000}});
  const auto checker = f.service.CreateUser("concurrent_checker", "并发检票员", "checker",
                                            "TestPass2026!", campus::RequestId());
  std::atomic<int> ready{0};
  std::atomic<bool> start{false};
  std::atomic<int> success{0};
  std::atomic<int> rejected{0};
  std::vector<std::thread> workers;
  for (const auto& session : sessions) {
    workers.emplace_back([&, session] {
      bool signalled = false;
      try {
        campus::Service connection(f.path, [&f] { return f.time; });
        connection.Login("admin", "TestPass2026!");
        ++ready;
        signalled = true;
        while (!start.load()) {
          std::this_thread::yield();
        }
        connection.Assign(session, checker, false, campus::RequestId());
        ++success;
      } catch (const campus::Error&) {
        if (!signalled) {
          ++ready;
        }
        ++rejected;
      }
    });
  }
  while (ready.load() < 2) {
    std::this_thread::yield();
  }
  start = true;
  for (auto& worker : workers) {
    worker.join();
  }
  Check(success == 1 && rejected == 1, "concurrent overlapping assignments cannot both succeed");
  int count = 0;
  for (const auto& session : sessions) {
    for (const auto& user : f.service.AssignmentCheckers(session, true)) {
      if (campus::Value(user, "id") == checker) {
        ++count;
      }
    }
  }
  Check(count == 1, "only one conflicting assignment persisted");
}

std::vector<campus::DraftSession> CompleteDraft(Fixture& f) {
  campus::DraftSession first;
  first.details = {"第一场", "仙林", f.time + 3600, f.time + 7200, f.time + 10800, 3};
  first.tickets = {{"", "免费票", 0, 2, {"A01", "A02"}}, {"", "收费票", 2000, 1, {"B01"}}};
  auto second = first;
  second.details.name = "第二场";
  second.details.start += 86400;
  second.details.end += 86400;
  return {first, second};
}

void TestCompleteDraftWorkflow() {
  Fixture f;
  auto input = CompleteDraft(f);
  const auto event =
      f.service.SaveDraft("", "中文活动", "演出", "完整草稿", input, false, campus::RequestId());
  auto saved = f.service.DraftSessions(event);
  Check(saved.size() == 2 && saved.front().tickets.size() == 2,
        "all sessions and tickets persisted");
  const auto session_id = saved.front().id;
  const auto ticket_id = saved.front().tickets.front().id;
  saved.front().details.location = "鼓楼";
  saved.pop_back();
  f.service.SaveDraft(event, "修改活动", "演出", "完整草稿", saved, true, campus::RequestId());
  const auto edited = f.service.DraftSessions(event);
  Check(edited.size() == 1 && edited.front().id == session_id,
        "removed session deleted; retained session keeps id");
  Check(edited.front().tickets.front().id == ticket_id, "retained ticket keeps id");
  const auto review = campus::Value(f.service.Reviews().front(), "id");
  Reject(
      [&] { f.service.SaveDraft(event, "再改", "演出", "", edited, false, campus::RequestId()); },
      "pending review prevents draft overwrite");
  f.Login("admin");
  f.service.Decide(review, true, "通过", campus::RequestId());
  f.Login("p0");
  Check(!f.service.Register(ticket_id, campus::RequestId()).empty(),
        "published aggregate accepts registration");
}

void TestDraftPartialAndRollback() {
  Fixture f;
  auto invalid = CompleteDraft(f);
  invalid.front().details.capacity = 1;
  Reject(
      [&] {
        f.service.SaveDraft("", "不合法", "演出", "简介", invalid, true, campus::RequestId());
      },
      "invalid complete submit rejected");
  Check(f.service.Events().empty() && f.service.Reviews().empty(),
        "failed new submission leaves no partial event or review");
  invalid.front().details.location.clear();
  invalid.front().tickets.front().seats.clear();
  const auto partial =
      f.service.SaveDraft("", "未完成", "", "", invalid, false, campus::RequestId());
  Check(f.service.DraftSessions(partial).front().tickets.front().seats.empty(),
        "incomplete seat configuration can be saved");
  auto complete = CompleteDraft(f);
  const auto event =
      f.service.SaveDraft("", "完整", "演出", "简介", complete, true, campus::RequestId());
  Check(!event.empty(), "unrelated incomplete draft does not prevent valid submission");
  f.Login("admin");
  f.service.Decide(campus::Value(f.service.Reviews().front(), "id"), true, "通过",
                   campus::RequestId());
  f.Login("owner");
  auto previous = f.service.DraftSessions(partial);
  auto changed = previous;
  changed.front().details.name = "不应保存";
  campus::Database fault(f.path);
  fault.Execute(
      "CREATE TRIGGER fail_draft_log BEFORE INSERT ON logs BEGIN SELECT RAISE(ABORT,'injected'); "
      "END");
  Reject(
      [&] {
        f.service.SaveDraft(partial, "不应保存", "演出", "", changed, false, campus::RequestId());
      },
      "aggregate log failure rolls back");
  fault.Execute("DROP TRIGGER fail_draft_log");
  Check(f.service.DraftSessions(partial).front().details.name == previous.front().details.name,
        "all nested edits rolled back");
  f.Login("other");
  Reject(
      [&] { f.service.SaveDraft(partial, "越权", "", "", previous, false, campus::RequestId()); },
      "cross organizer aggregate editing rejected");
}

void TestReviewWithdrawalLifecycle() {
  Fixture f;
  f.Publish();
  const auto first = f.Register(0);
  const auto waiting = f.Register(1);
  f.Login("owner");
  const auto review =
      f.service.ProposeEdit(f.event, "候选活动", "文艺", "候选简介", campus::RequestId());
  const auto logs = f.service.Logs().size();
  try {
    f.service.ProposeEdit(f.event, "重复", "文艺", "简介", campus::RequestId());
    Check(false, "pending edit prevents duplicate");
  } catch (const campus::Error& error) {
    Check(std::string(error.what()).find("UNIQUE") == std::string::npos &&
              std::string(error.what()).find("撤回") != std::string::npos,
          "duplicate review has actionable message");
  }
  Check(f.service.Logs().size() == logs, "duplicate review produces no success log");
  f.Login("other");
  Reject([&] { f.service.Withdraw(review, campus::RequestId()); },
         "cross organizer withdrawal denied");
  f.Login("admin");
  Reject([&] { f.service.Withdraw(review, campus::RequestId()); },
         "admin cannot withdraw organizer review");
  f.Login("p0");
  f.service.Cancel(first, campus::RequestId());
  Check(campus::Value(f.Registration(waiting), "status") == "waiting",
        "edit gate pauses promotion");
  f.Login("owner");
  f.service.Withdraw(review, campus::RequestId());
  Check(campus::Value(f.service.Reviews().front(), "status") == "withdrawn",
        "review marked withdrawn");
  Check(campus::Value(f.service.Events().front(), "name") == "音乐会",
        "withdrawal preserves published content");
  const auto after = f.service.Logs().size();
  Reject([&] { f.service.Withdraw(review, campus::RequestId()); }, "repeated withdrawal rejected");
  Check(f.service.Logs().size() == after, "repeated withdrawal has no log");
  f.service.Tick();
  Check(campus::Value(f.Registration(waiting), "status") == "registered",
        "withdrawal restores promotion");
  f.Login("owner");
  const auto replacement =
      f.service.ProposeEdit(f.event, "重新提交", "文艺", "新简介", campus::RequestId());
  Check(replacement != review, "withdrawal permits a new review without losing history");
  Check(f.service.Reviews().size() == 3, "publish and both edit review histories preserved");
  f.Login("admin");
  Reject([&] { f.service.Decide(review, true, "过期页面", campus::RequestId()); },
         "withdrawn review cannot be approved");
  f.service.Decide(replacement, true, "通过", campus::RequestId());
  Check(campus::Value(f.service.Events().front(), "name") == "重新提交",
        "replacement edit can be approved");
}

void TestConcurrentReviewWithdrawal() {
  Fixture f;
  f.Publish();
  f.Login("owner");
  const auto review = f.service.ProposeEdit(f.event, "新内容", "文艺", "简介", campus::RequestId());
  std::atomic<int> ready{0};
  std::atomic<bool> start{false};
  std::atomic<int> success{0};
  std::atomic<int> rejected{0};
  std::vector<std::thread> workers;
  for (bool approve : {true, false}) {
    workers.emplace_back([&, approve] {
      bool signalled = false;
      try {
        campus::Service connection(f.path, [&f] { return f.time; });
        connection.Login(approve ? "admin" : "owner", "TestPass2026!");
        ++ready;
        signalled = true;
        while (!start.load()) {
          std::this_thread::yield();
        }
        if (approve) {
          connection.Decide(review, true, "并发审核", campus::RequestId());
        } else {
          connection.Withdraw(review, campus::RequestId());
        }
        ++success;
      } catch (const campus::Error&) {
        if (!signalled) {
          ++ready;
        }
        ++rejected;
      }
    });
  }
  while (ready.load() < 2) {
    std::this_thread::yield();
  }
  start = true;
  for (auto& worker : workers) {
    worker.join();
  }
  Check(success == 1 && rejected == 1, "approval and withdrawal cannot both succeed");
  const auto status = campus::Value(f.service.Reviews().front(), "status");
  Check(status == "approved" || status == "withdrawn", "race leaves one terminal state");
  Check(campus::Value(f.service.Events().front(), "name") ==
            (status == "approved" ? "新内容" : "音乐会"),
        "published content agrees with review outcome");
}

void TestAccountEditing() {
  Fixture f;
  f.Publish();
  const auto id = f.service.CreateUser("checker2", "错误姓名", "participant", "TestPass2026!",
                                       campus::RequestId());
  campus::Service existing_session(f.path, [&f] { return f.time; });
  existing_session.Login("checker2", "TestPass2026!");
  const auto request = campus::RequestId();
  f.service.EditUser(id, "正确姓名", "checker", true, request);
  const auto updated = f.service.Users("checker2").front();
  Check(campus::Value(updated, "name") == "正确姓名" && campus::Value(updated, "role") == "checker",
        "account name and mistaken role updated");
  Check(campus::Value(existing_session.CurrentUser(), "role") == "checker",
        "existing session reads changed role");
  Reject([&] { existing_session.Register(f.ticket, campus::RequestId()); },
         "old role privileges no longer apply");
  existing_session.Login("checker2", "TestPass2026!");
  Check(campus::Value(existing_session.CurrentUser(), "username") == "checker2",
        "username and password preserved");
  const auto logs = f.service.Logs().size();
  Reject([&] { f.service.EditUser(id, "重复", "participant", true, request); },
         "duplicate account edit rejected");
  Check(f.service.Logs().size() == logs, "duplicate edit creates no log");
  f.service.Assign(f.session, id, false, campus::RequestId());
  Reject([&] { f.service.EditUser(id, "不能保存", "participant", true, campus::RequestId()); },
         "assigned checker cannot change role");
  Check(campus::Value(f.service.Users("checker2").front(), "name") == "正确姓名",
        "rejected role edit does not partly change name");
  f.service.EditUser(id, "分配后仍能改名", "checker", false, campus::RequestId());
  Reject([&] { existing_session.CurrentUser(); }, "disabled edit blocks existing session");
  f.service.Assign(f.session, id, true, campus::RequestId());
  f.service.EditUser(id, "参与者姓名", "participant", true, campus::RequestId());
  Check(campus::Value(f.service.Users("checker2").front(), "role") == "participant",
        "role can be corrected after assignments removed");
  existing_session.Login("checker2", "TestPass2026!");
  Reject([&] { existing_session.EditUser(id, "越权", "admin", true, campus::RequestId()); },
         "participant cannot edit accounts");
  const auto admin = campus::Value(f.service.CurrentUser(), "id");
  Reject([&] { f.service.EditUser(admin, "管理员", "checker", true, campus::RequestId()); },
         "cannot change own admin role");
  Reject([&] { f.service.EditUser(admin, "管理员", "admin", false, campus::RequestId()); },
         "cannot disable own admin account");
  f.service.EditUser(admin, "管理员新姓名", "admin", true, campus::RequestId());
  Check(campus::Value(f.service.CurrentUser(), "name") == "管理员新姓名",
        "admin can correct own name");
}

void TestAccountEditHistoryAndRollback() {
  Fixture f;
  f.Publish();
  f.Register(0);
  const auto participant = campus::Value(f.service.Registrations().front(), "user_id");
  f.Login("admin");
  const auto owner = campus::Value(f.service.Users("owner").front(), "id");
  Reject([&] { f.service.EditUser(owner, "组织者", "participant", true, campus::RequestId()); },
         "event ownership prevents role change");
  Reject([&] { f.service.EditUser(participant, "参与者", "checker", true, campus::RequestId()); },
         "registration history prevents role change");
  f.service.EditUser(participant, "报名后改名", "participant", true, campus::RequestId());
  Check(campus::Value(f.service.Users("p0").front(), "name") == "报名后改名",
        "history does not prevent name correction");
  const auto user = f.service.CreateUser("rollback_user", "原姓名", "participant", "TestPass2026!",
                                         campus::RequestId());
  campus::Database fault(f.path);
  fault.Execute(
      "CREATE TRIGGER fail_account_log BEFORE INSERT ON logs BEGIN SELECT RAISE(ABORT,'injected'); "
      "END");
  Reject([&] { f.service.EditUser(user, "不应保存", "checker", false, campus::RequestId()); },
         "account edit log failure rolls back");
  fault.Execute("DROP TRIGGER fail_account_log");
  const auto row = f.service.Users("rollback_user").front();
  Check(campus::Value(row, "name") == "原姓名" && campus::Value(row, "role") == "participant" &&
            campus::Value(row, "enabled") == "1",
        "all account fields rolled back");
}

void TestCompletePublishedEdit() {
  Fixture f;
  f.Publish(0, 2);
  f.Login("owner");
  auto sessions = f.service.EventConfiguration(f.event);
  sessions[0].details.name = "修改后场次";
  sessions[0].details.location = "新会场";
  sessions[0].details.capacity = 3;
  sessions[0].tickets[0].name = "修改后票种";
  sessions[0].tickets[0].price_cents = 500;
  sessions[0].tickets.push_back({"", "新增票种", 0, 1, {"B01"}});
  campus::DraftSession added;
  added.details = {"新增场次", "第二会场", f.time + 4000, f.time + 12000, f.time + 15000, 1};
  added.tickets.push_back({"", "第二场票", 0, 1, {"C01"}});
  sessions.push_back(added);
  const auto review = f.service.ProposeCompleteEdit(f.event, "完整修改", "文艺", "完整修改简介",
                                                    sessions, campus::RequestId());
  Check(f.service.Sessions(f.event).size() == 1, "new session must await approval");
  Check(campus::Value(f.service.Sessions(f.event).front(), "location") == "仙林",
        "preview must not mutate published session");
  Check(f.service.Tickets(f.session).size() == 1, "new ticket must await approval");
  Check(f.service.ReviewDetails(review).find("第二会场") != std::string::npos,
        "review must include new session details");
  f.Login("admin");
  f.service.Decide(review, true, "批准", campus::RequestId());
  Check(f.service.Sessions(f.event).size() == 2, "approval creates new session");
  const auto first = f.service.Sessions(f.event).front();
  Check(campus::Value(first, "id") == f.session && campus::Value(first, "name") == "修改后场次" &&
            campus::Value(first, "location") == "新会场",
        "original session identity preserved");
  Check(f.service.Tickets(f.session).size() == 2, "approval creates new ticket");
  Check(campus::Value(f.service.Tickets(f.session).front(), "price") == "500",
        "approval updates original ticket");
}

void TestCompletePublishedEditProtection() {
  Fixture f;
  f.Publish();
  f.Register(0);
  f.Login("owner");
  auto sessions = f.service.EventConfiguration(f.event);
  Check(sessions[0].tickets[0].price_locked, "registration locks price in editor");
  sessions[0].tickets[0].price_cents = 500;
  Reject(
      [&] {
        f.service.ProposeCompleteEdit(f.event, "音乐会", "文艺", "简介", sessions,
                                      campus::RequestId());
      },
      "registered price change");
  Check(f.service.Reviews().size() == 1, "failed preview leaves no pending review");
  sessions = f.service.EventConfiguration(f.event);
  sessions[0].tickets.clear();
  Reject(
      [&] {
        f.service.ProposeCompleteEdit(f.event, "音乐会", "文艺", "简介", sessions,
                                      campus::RequestId());
      },
      "published ticket removal");
  sessions = f.service.EventConfiguration(f.event);
  sessions[0].details.location = "候选会场";
  const auto review = f.service.ProposeCompleteEdit(f.event, "音乐会", "文艺", "简介", sessions,
                                                    campus::RequestId());
  f.service.Withdraw(review, campus::RequestId());
  Check(campus::Value(f.service.Sessions(f.event).front(), "location") == "仙林",
        "withdrawal preserves original configuration");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"complete_published_edit", TestCompletePublishedEdit},
      {"complete_published_edit_protection", TestCompletePublishedEditProtection},
      {"queue_and_cancellation", TestQueueAndCancellation},
      {"payment_and_refund", TestPaymentAndRefund},
      {"timeout_boundary", TestTimeoutBoundary},
      {"seats_checkin_absence", TestSeatsCheckInAbsence},
      {"review_gates", TestReviewGates},
      {"edit_and_notifications", TestEditAndNotifications},
      {"expiry_and_privacy", TestExpiryAndPrivacy},
      {"rollback_backup_csv", TestRollbackBackupCsv},
      {"concurrent_last_seat", TestConcurrentLastSeat},
      {"draft_validation", TestDraftValidation},
      {"draft_editing", TestDraftEditing},
      {"published_ticket_protection", TestPublishedTicketProtection},
      {"original_ticket_queue", TestOriginalTicketQueue},
      {"cancellation_gates_and_history", TestCancellationGatesAndHistory},
      {"assignment_management", TestAssignmentManagement},
      {"assignment_time_conflicts", TestAssignmentTimeConflicts},
      {"assignment_time_edit_protection", TestAssignmentTimeEditProtection},
      {"concurrent_checker_schedule", TestConcurrentCheckerSchedule},
      {"complete_draft_workflow", TestCompleteDraftWorkflow},
      {"draft_partial_and_rollback", TestDraftPartialAndRollback},
      {"review_withdrawal_lifecycle", TestReviewWithdrawalLifecycle},
      {"concurrent_review_withdrawal", TestConcurrentReviewWithdrawal},
      {"account_editing", TestAccountEditing},
      {"account_edit_history_and_rollback", TestAccountEditHistoryAndRollback}};
  int failed = 0;
  for (const auto& test : tests) {
    try {
      test.second();
      std::cout << "PASS " << test.first << '\n';
    } catch (const std::exception& error) {
      ++failed;
      std::cerr << "FAIL " << test.first << ": " << error.what() << '\n';
    }
  }
  std::cout << assertions << " assertions, " << tests.size() << " scenarios, " << failed
            << " failures\n";
  return failed == 0 ? 0 : 1;
}
