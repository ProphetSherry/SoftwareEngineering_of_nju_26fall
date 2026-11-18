#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "campus/service.h"

int main() {
  const auto path = "/tmp/campus-benchmark-" + campus::RequestId() + ".db";
  try {
    campus::Service seed(path);
    seed.SeedDemo();
    {
      campus::Database database(path);
      campus::Transaction transaction(database);
      database.Execute("UPDATE sessions SET capacity=10000 WHERE id=1");
      database.Execute("UPDATE tickets SET capacity=10000 WHERE id=1");
      const auto additional =
          10000 - std::stoi(database.Scalar("SELECT count(*) FROM registrations"));
      for (int i = 0; i < additional; ++i) {
        database.Execute(
            "INSERT INTO users(username,name,role,salt,password_hash) "
            "SELECT ?,?,'participant',salt,password_hash FROM users WHERE id=3",
            {"load" + std::to_string(i), "负载用户" + std::to_string(i)});
        const auto user = database.LastId();
        database.Execute(
            "INSERT INTO "
            "registrations(user_id,event_id,session_id,ticket_id,status,created,confirmed) "
            "VALUES(?,1,1,1,'registered',?,?)",
            {user, std::to_string(campus::Now()), std::to_string(campus::Now())});
        database.Execute("INSERT INTO seats(session_id,ticket_id,label) VALUES(1,1,?)",
                         {"LOAD-" + std::to_string(i)});
      }
      database.Execute("UPDATE sessions SET capacity=10002 WHERE id=1");
      transaction.Commit();
      database.CheckIntegrity();
    }
    std::mutex mutex;
    std::vector<double> durations;
    std::atomic<int> ready{0};
    std::atomic<int> failures{0};
    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    for (int i = 0; i < 20; ++i) {
      workers.emplace_back([&] {
        try {
          campus::Service service(path);
          service.Login("student", "Campus2026!");
          ++ready;
          while (!start.load()) {
            std::this_thread::yield();
          }
          for (int query = 0; query < 5; ++query) {
            const auto begin = std::chrono::steady_clock::now();
            bool success = false;
            try {
              const auto rows = service.Events({"音乐会", "文艺演出", "仙林", "", ""});
              success = rows.size() == 1;
            } catch (...) { /* Failed queries are counted, never discarded. */
            }
            const auto elapsed =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin)
                    .count();
            std::lock_guard<std::mutex> lock(mutex);
            durations.push_back(elapsed);
            if (!success || elapsed >= 1000) {
              ++failures;
            }
          }
        } catch (...) {
          failures += 5;
          ++ready;
        }
      });
    }
    while (ready.load() < 20) {
      std::this_thread::yield();
    }
    start = true;
    for (auto& worker : workers) {
      worker.join();
    }
    std::cout << "dataset_registrations=10000\nworkers=20\nrequested_queries=100\n"
              << "measured_queries=" << durations.size() << "\nfailed_or_over_1s=" << failures
              << '\n';
    for (std::size_t i = 0; i < durations.size(); ++i) {
      std::cout << "query_" << i + 1 << "_ms=" << std::fixed << std::setprecision(3) << durations[i]
                << '\n';
    }
    std::sort(durations.begin(), durations.end());
    if (durations.size() == 100) {
      std::cout << "p95_ms=" << durations[94] << "\nmax_ms=" << durations.back() << '\n';
    }
    for (const auto& suffix : {"", "-wal", "-shm"}) {
      std::filesystem::remove(path + suffix);
    }
    return failures <= 5 && durations.size() == 100 ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
