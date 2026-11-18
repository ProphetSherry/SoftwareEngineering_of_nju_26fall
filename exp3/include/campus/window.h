#ifndef EXP3_INCLUDE_CAMPUS_WINDOW_H_
#define EXP3_INCLUDE_CAMPUS_WINDOW_H_

#include <QMainWindow>
#include <QMenu>
#include <QPushButton>
#include <QVBoxLayout>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "campus/service.h"

namespace campus {
class Window : public QMainWindow {
 public:
  explicit Window(const std::string& database);
  void SmokeScreenshot(const QString& directory);

 private:
  Service service_;
  QVBoxLayout* body_ = nullptr;
  QWidget* records_ = nullptr;
  int selected_row_ = -1;
  QMenu* more_menu_ = nullptr;
  QVBoxLayout* root_layout_ = nullptr;
  std::vector<QPushButton*> tabs_;
  std::map<QString, std::function<void()>> actions_by_name_;
  Row detail_event_;
  Row draft_event_;
  Row editing_user_;
  std::string detail_session_;
  std::string registration_event_;
  std::string registration_session_;
  std::string registration_status_;
  void DetailsPage();
  void TicketPage();
  void CardAction(QVBoxLayout* layout, const QString& label, int row, const QString& action,
                  bool secondary = false);
  QString page_;
  Row user_;
  Rows displayed_;
  void LoginPage();
  void Shell();
  void ShowPage(const QString& page);
  void EventsPage();
  void RegistrationsPage();
  void ReviewsPage();
  void UsersPage();
  void EditUserPage();
  void AssignmentPage();
  void StatisticsPage();
  void LogsPage();
  void NotificationsPage();
  void SetRecords(const Rows& rows, const std::vector<std::pair<std::string, QString>>& columns);
  Row Selected();
  void Button(const QString& text, const std::function<void()>& action);
  void Run(const std::function<void()>& action, bool refresh = true);
  void EventDetails(const Row& event);
  void CreateEventPage();
  void EditEventDialog(const Row& event);
  void AddSessionDialog(const Row& event);
  void AddTicketDialog(const Row& event);
  QString Ask(const QString& title, const QString& label, const QString& initial = "");
  QString Choose(const QString& title, const Rows& rows, const std::string& label = "name");
};
}  // namespace campus
#endif  // EXP3_INCLUDE_CAMPUS_WINDOW_H_
