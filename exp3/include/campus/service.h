#ifndef EXP3_INCLUDE_CAMPUS_SERVICE_H_
#define EXP3_INCLUDE_CAMPUS_SERVICE_H_

#include <functional>
#include <string>
#include <vector>

#include "campus/database.h"

namespace campus {

struct SessionInput {
  std::string name;
  std::string location;
  std::int64_t deadline = 0;
  std::int64_t start = 0;
  std::int64_t end = 0;
  int capacity = 1;
};

struct DraftTicket {
  std::string id;
  std::string name;
  int price_cents = 0;
  int capacity = 1;
  Params seats;
  bool price_locked = false;
  bool seats_locked = false;
  bool cancelled = false;
};

struct DraftSession {
  std::string id;
  SessionInput details;
  std::vector<DraftTicket> tickets;
  bool deadline_locked = false;
  bool cancelled = false;
};

struct Search {
  std::string keyword;
  std::string category;
  std::string location;
  std::string date;
  std::string status;
  std::string session = "";
};

// The UI never receives a database connection. Every service entry point checks
// the current account again, including account suspension and object ownership.
class Service {
 public:
  explicit Service(const std::string& path, std::function<std::int64_t()> clock = Now);
  void Bootstrap(const std::string& username, const std::string& password);
  Row Login(const std::string& username, const std::string& password);
  void Logout();
  Row CurrentUser();
  std::string CreateUser(const std::string& username, const std::string& name,
                         const std::string& role, const std::string& password,
                         const std::string& request);
  void EditUser(const std::string& user, const std::string& name, const std::string& role,
                bool enabled, const std::string& request);
  void SetEnabled(const std::string& user, bool enabled, const std::string& request);
  Rows Users(const std::string& keyword = "");
  std::string CreateEvent(const std::string& name, const std::string& category,
                          const std::string& description, const std::string& request);
  std::vector<DraftSession> EventConfiguration(const std::string& event);
  std::string ProposeCompleteEdit(const std::string& event, const std::string& name,
                                  const std::string& category, const std::string& description,
                                  const std::vector<DraftSession>& sessions,
                                  const std::string& request);
  std::vector<DraftSession> DraftSessions(const std::string& event);
  std::string SaveDraft(const std::string& event, const std::string& name,
                        const std::string& category, const std::string& description,
                        const std::vector<DraftSession>& sessions, bool submit,
                        const std::string& request);
  void EditDraft(const std::string& event, const std::string& name, const std::string& category,
                 const std::string& description, const std::string& request);
  void DeleteDraft(const std::string& event, const std::string& request);
  std::string AddSession(const std::string& event, const SessionInput& input,
                         const std::string& request);
  std::string AddTicket(const std::string& session, const std::string& name, int price_cents,
                        int capacity, const Params& seats, const std::string& request);
  void EditSessionDraft(const std::string& session, const SessionInput& input,
                        const std::string& request);
  void EditTicketDraft(const std::string& ticket, const std::string& name, int price_cents,
                       const Params& seats, const std::string& request);
  void DeleteDraftChild(const std::string& scope, const std::string& target,
                        const std::string& request);
  std::string ReviewDetails(const std::string& review);
  std::string Submit(const std::string& event, const std::string& request);
  std::string ProposeEdit(const std::string& event, const std::string& name,
                          const std::string& category, const std::string& description,
                          const std::string& request);
  void ProposeSession(const std::string& review, const std::string& session,
                      const SessionInput& input, const std::string& request);
  void ProposeTicket(const std::string& review, const std::string& session,
                     const std::string& ticket, const std::string& name, int price_cents,
                     const Params& seats, const std::string& request);
  std::string RequestCancellation(const std::string& event, const std::string& scope,
                                  const std::string& target, const std::string& request);
  void Withdraw(const std::string& review, const std::string& request);
  void Decide(const std::string& review, bool approved, const std::string& reason,
              const std::string& request);
  Rows Reviews();
  Rows Events(const Search& search = {});
  Rows Sessions(const std::string& event);
  Rows Tickets(const std::string& session);
  Rows Versions(const std::string& event);
  std::string Register(const std::string& ticket, const std::string& request);
  void Cancel(const std::string& registration, const std::string& request);
  void Receive(const std::string& registration, int amount_cents, const std::string& request);
  void AbnormalReceipt(const std::string& registration, int amount_cents,
                       const std::string& request);
  void CompleteRefund(const std::string& refund, const std::string& request);
  Rows AssignmentSessions();
  Rows AssignmentCheckers(const std::string& session, bool assigned);
  void Assign(const std::string& session, const std::string& checker, bool remove,
              const std::string& request);
  void CheckIn(const std::string& registration, const std::string& request);
  Rows Registrations(const std::string& event = "", const Search& filter = {});
  Rows Refunds(const std::string& event);
  Rows Statistics(const std::string& event);
  Rows Logs(const std::string& event = "", const std::string& action = "",
            const std::string& actor = "", std::int64_t since = 0);
  Rows Notifications();
  std::string OperationResult(const std::string& request);
  void ExportCsv(const std::string& event, const std::string& destination,
                 const std::string& session = "", const std::string& ticket = "",
                 const std::string& status = "");
  void Tick();
  void Backup(const std::string& destination);
  void CheckIntegrity();
  void SeedDemo();

 private:
  Database database_;
  std::string path_;
  std::function<std::int64_t()> clock_;
  std::string user_id_;
  Row Require(const std::string& role = "");
  Row Own(const std::string& event);
  Row Event(const std::string& event);
  Row Session(const std::string& session);
  Row Ticket(const std::string& ticket);
  Row Registration(const std::string& registration);
  bool Visible(const Row& event, const Row& user);
  bool Blocked(const Row& ticket, bool cancellation_only = false);
  bool Available(const Row& ticket);
  bool CheckerConflict(const std::string& checker, const Row& session);
  void ValidateAssignmentSchedule(const std::string& event);
  void Validate(const std::string& event);
  void BeginRequest(const std::string& request);
  void EndRequest(const std::string& request, const std::string& result);
  void Log(const std::string& event, const std::string& user, const std::string& type,
           const std::string& object, const std::string& action, const std::string& before,
           const std::string& after, const std::string& request,
           const std::string& result = "success");
  void Transition(const Row& registration, const std::string& status, const std::string& action,
                  const std::string& request);
  void Promote(const std::string& ticket, const std::string& request);
  void Refund(const std::string& registration, const std::string& reason);
  void Allocate(const Row& session, const std::string& request);
  void ApplyEventEdit(const std::string& review, const std::string& event,
                      const std::string& request);
  std::string Snapshot(const std::string& event);
  std::string NewReview(const std::string& event, const std::string& kind, const std::string& scope,
                        const std::string& target);
};

}  // namespace campus
#endif  // EXP3_INCLUDE_CAMPUS_SERVICE_H_
