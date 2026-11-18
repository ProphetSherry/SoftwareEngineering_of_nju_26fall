#include <iostream>
#include <string>

#include "campus/service.h"

namespace {
void Print(const campus::Rows& rows) {
  for (const auto& row : rows) {
    for (const auto& field : row) {
      std::cout << field.first << '=' << field.second << " | ";
    }
    std::cout << '\n';
  }
  std::cout << rows.size() << " record(s)\n";
}

void Help() {
  std::cout << "CampusTickets CLI\n"
               "Usage: campus_cli DATABASE COMMAND [arguments]\n"
               "  demo                              seed an empty demonstration database\n"
               "  init ADMIN PASSWORD               initialize an empty database\n"
               "  tick                              run timeout, queue, seat and absence jobs\n"
               "  shell                             interactive authenticated terminal\n"
               "Shell commands (IDs are integers):\n"
               "  login USERNAME PASSWORD | logout | whoami\n"
               "  events | sessions EVENT | tickets SESSION | mine\n"
               "  register TICKET | cancel REGISTRATION\n"
               "  receive REGISTRATION AMOUNT_CENTS | abnormal REGISTRATION AMOUNT_CENTS\n"
               "  refund REFUND | checkin REGISTRATION\n"
               "  reviews | approve REVIEW | reject REVIEW REASON\n"
               "  stats EVENT | refunds EVENT | logs | users | notifications\n"
               "  export EVENT PATH | backup PATH | integrity | tick | help | quit\n"
               "Quoted values containing spaces are supported in the shell.\n";
}
}  // namespace

#include <iomanip>
#include <sstream>

int main(int argc, char* argv[]) {
  if (argc < 3) {
    Help();
    return 2;
  }
  try {
    campus::Service service(argv[1]);
    const std::string command = argv[2];
    if (command == "demo") {
      service.SeedDemo();
      std::cout << "Demo ready\n";
      return 0;
    }
    if (command == "init" && argc == 5) {
      service.Bootstrap(argv[3], argv[4]);
      std::cout << "Administrator created\n";
      return 0;
    }
    if (command == "tick") {
      service.Tick();
      return 0;
    }
    if (command != "shell") {
      Help();
      return 2;
    }
    Help();
    std::string line;
    while (std::cout << "campus> " && std::getline(std::cin, line)) {
      try {
        std::istringstream input(line);
        std::string operation;
        input >> operation;
        if (operation.empty()) {
          continue;
        }
        if (operation == "quit") {
          break;
        }
        campus::Params arguments;
        std::string argument;
        while (input >> std::quoted(argument)) {
          arguments.push_back(argument);
        }
        const auto get = [&](std::size_t index) -> std::string {
          if (index >= arguments.size()) {
            throw campus::Error("Missing command argument");
          }
          return arguments[index];
        };
        const auto request = campus::RequestId();
        if (operation == "login") {
          Print({service.Login(get(0), get(1))});
        } else if (operation == "logout") {
          service.Logout();
        } else if (operation == "whoami") {
          Print({service.CurrentUser()});
        } else if (operation == "events") {
          Print(service.Events());
        } else if (operation == "sessions") {
          Print(service.Sessions(get(0)));
        } else if (operation == "tickets") {
          Print(service.Tickets(get(0)));
        } else if (operation == "mine") {
          Print(service.Registrations());
        } else if (operation == "register") {
          std::cout << service.Register(get(0), request) << '\n';
        } else if (operation == "cancel") {
          service.Cancel(get(0), request);
        } else if (operation == "receive") {
          service.Receive(get(0), std::stoi(get(1)), request);
        } else if (operation == "abnormal") {
          service.AbnormalReceipt(get(0), std::stoi(get(1)), request);
        } else if (operation == "refund") {
          service.CompleteRefund(get(0), request);
        } else if (operation == "checkin") {
          service.CheckIn(get(0), request);
        } else if (operation == "reviews") {
          Print(service.Reviews());
        } else if (operation == "approve") {
          service.Decide(get(0), true, "CLI approved", request);
        } else if (operation == "reject") {
          service.Decide(get(0), false, get(1), request);
        } else if (operation == "stats") {
          Print(service.Statistics(get(0)));
        } else if (operation == "refunds") {
          Print(service.Refunds(get(0)));
        } else if (operation == "logs") {
          Print(service.Logs());
        } else if (operation == "users") {
          Print(service.Users());
        } else if (operation == "notifications") {
          Print(service.Notifications());
        } else if (operation == "export") {
          service.ExportCsv(get(0), get(1));
        } else if (operation == "backup") {
          service.Backup(get(0));
        } else if (operation == "integrity") {
          service.CheckIntegrity();
        } else if (operation == "tick") {
          service.Tick();
        } else if (operation == "help") {
          Help();
        } else {
          throw campus::Error("Unknown command");
        }
      } catch (const std::exception& error) {
        std::cout << "FAILED: " << error.what() << '\n';
      }
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
}
