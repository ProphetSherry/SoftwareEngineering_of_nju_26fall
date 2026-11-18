#include <QApplication>
#include <QComboBox>
#include <QInputMethodEvent>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTextEdit>
#include <QTimer>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include "campus/window.h"

namespace {
void Check(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <typename Widget>
Widget* Visible(campus::Window* window, const char* name) {
  for (auto* widget : window->findChildren<Widget*>(name)) {
    if (widget->isVisible()) {
      return widget;
    }
  }
  throw std::runtime_error(std::string("Missing visible widget: ") + name);
}

void Commit(QWidget* field, const QString& text) {
  field->setFocus();
  QInputMethodEvent input;
  input.setCommitString(text);
  QApplication::sendEvent(field, &input);
}
}  // namespace

int main(int argc, char* argv[]) {
  QApplication app(argc, argv);
  const auto directory =
      std::filesystem::temp_directory_path() / ("campus-gui-test-" + campus::RequestId());
  std::filesystem::create_directory(directory);
  int result = 0;
  try {
    const auto database = (directory / "test.db").string();
    campus::Service service(database);
    service.SeedDemo();
    campus::Window window(database);
    window.show();
    QApplication::processEvents();
    Visible<QLineEdit>(&window, "loginUsername")->setText("organizer");
    Visible<QLineEdit>(&window, "loginPassword")->setText("Campus2026!");
    Visible<QPushButton>(&window, "loginButton")->click();
    QApplication::processEvents();
    Visible<QPushButton>(&window, "createEventButton")->click();
    QApplication::processEvents();
    Check(QApplication::activeModalWidget() == nullptr, "Creation opened a modal dialog");
    auto* name = Visible<QLineEdit>(&window, "createEventName");
    auto* category = Visible<QLineEdit>(&window, "createEventCategory");
    auto* description = Visible<QTextEdit>(&window, "createEventDescription");
    for (auto* field : {static_cast<QWidget*>(name), static_cast<QWidget*>(category),
                        static_cast<QWidget*>(description)}) {
      Check(field->window() == &window, "Creation field has a separate input window");
      Check(field->testAttribute(Qt::WA_InputMethodEnabled), "Input method disabled");
    }
    const bool real_ime = qEnvironmentVariableIsSet("CAMPUS_REAL_IME_TEST");
    const QString expected_name = real_ime ? "你好" : "校园中文活动";
    const QString expected_category = real_ime ? "中文" : "文艺演出";
    const QString expected_description =
        real_ime ? "测试" : "这是一段中文简介。\n第二行继续输入中文。";
    if (real_ime) {
      window.activateWindow();
      name->setFocus();
      QTimer probe;
      QObject::connect(&probe, &QTimer::timeout, &app, [&] {
        if (name->text() == expected_name && category->text() == expected_category &&
            description->toPlainText() == expected_description) {
          app.quit();
        }
      });
      probe.start(100);
      QTimer::singleShot(30000, &app, [&] {
        std::cerr << "Real IME timeout: " << name->text().toStdString() << " | "
                  << category->text().toStdString() << " | "
                  << description->toPlainText().toStdString() << '\n';
        app.exit(1);
      });
      QTimer::singleShot(500, &app, [&] {
        std::cout << "READY " << static_cast<std::uint64_t>(window.winId());
        for (QWidget* field : {static_cast<QWidget*>(name), static_cast<QWidget*>(category),
                               static_cast<QWidget*>(description)}) {
          const auto point = field->mapToGlobal(QPoint(30, 20));
          std::cout << ' ' << point.x() << ' ' << point.y();
        }
        std::cout << std::endl;
      });
      Check(app.exec() == 0, "Real keyboard / Fcitx input did not produce Chinese");
    } else {
      Commit(name, expected_name);
      Commit(category, expected_category);
      Commit(description, expected_description);
    }
    Check(name->text() == expected_name, "Chinese title was not committed");
    Check(category->text() == expected_category, "Chinese category was not committed");
    Check(description->toPlainText() == expected_description, "Chinese description missing");
    QTimer::singleShot(10000, &app, [] {
      std::cerr << "FAIL: GUI operation blocked\n";
      QApplication::exit(1);
    });
    Visible<QPushButton>(&window, "saveEventButton")->click();
    QApplication::processEvents();
    Check(QApplication::activeModalWidget() == nullptr, "Saving opened an error dialog");
    service.Login("organizer", "Campus2026!");
    campus::Search search;
    search.keyword = expected_name.toStdString();
    const auto rows = service.Events(search);
    Check(rows.size() == 1, "Chinese draft not saved exactly once");
    Check(campus::Value(rows.front(), "category") == expected_category.toStdString(),
          "Saved category changed");
    Check(campus::Value(rows.front(), "description") == expected_description.toStdString(),
          "Saved multiline description changed");
    Check(campus::Value(rows.front(), "status") == "draft", "New event not a draft");
    Check(window.size() == QSize(393, 852), "Mobile layout changed");
    const auto event_id = campus::Value(rows.front(), "id");
    const auto initial = service.DraftSessions(event_id);
    Check(initial.size() == 1 && initial.front().tickets.size() == 1,
          "new draft did not save initial session and ticket");
    if (!real_ime) {
      const auto edit_name = "editEventButton_" + event_id;
      Visible<QPushButton>(&window, edit_name.c_str())->click();
      QApplication::processEvents();
      Check(Visible<QLineEdit>(&window, "createEventName")->text() == expected_name,
            "draft editor did not reload event");
      Visible<QPushButton>(&window, "addDraftSessionButton")->click();
      QApplication::processEvents();
      for (auto* location : window.findChildren<QLineEdit*>("draftSessionLocation")) {
        if (location->isVisible() && location->text().isEmpty()) {
          Commit(location, "仙林活动中心");
        }
      }
      Visible<QPushButton>(&window, "addDraftTicketButton")->click();
      QApplication::processEvents();
      Visible<QSpinBox>(&window, "draftSessionCapacity")->setValue(10);
      Visible<QPushButton>(&window, "submitEventButton")->click();
      QApplication::processEvents();
      auto updated = service.DraftSessions(event_id);
      Check(updated.size() == 2, "second session not submitted");
      Check(updated.front().tickets.size() == 2, "second ticket not submitted");
      Check(updated.front().id == initial.front().id, "editing replaced retained session id");
      auto reviews = service.Reviews();
      Check(!reviews.empty() && campus::Value(reviews.front(), "event_id") == event_id &&
                campus::Value(reviews.front(), "status") == "pending",
            "complete form not submitted to review");
      service.Login("admin", "Campus2026!");
      service.Decide(campus::Value(reviews.front(), "id"), true, "测试通过", campus::RequestId());
      service.Login("organizer", "Campus2026!");
      Check(campus::Value(service.Events(search).front(), "status") == "published",
            "complete draft not published after approval");
      const auto edit_review =
          service.ProposeEdit(event_id, "候选中文名称", "演出", "候选内容", campus::RequestId());
      for (auto* button : window.findChildren<QPushButton*>()) {
        if (button->isVisible() && button->property("page").toString() == "我的活动") {
          button->click();
          break;
        }
      }
      QApplication::processEvents();
      const auto withdraw_name = "withdrawReviewButton_" + edit_review;
      Visible<QPushButton>(&window, withdraw_name.c_str())->click();
      QApplication::processEvents();
      Check(campus::Value(service.Reviews().front(), "status") == "withdrawn",
            "organizer could not withdraw from activity card");
      Check(campus::Value(service.Events(search).front(), "name") == expected_name.toStdString(),
            "withdrawal changed published content");
      const auto replacement =
          service.ProposeEdit(event_id, "再次提交", "演出", "新内容", campus::RequestId());
      Check(replacement != edit_review, "GUI withdrawal did not permit resubmission");
      service.Withdraw(replacement, campus::RequestId());
      std::cout << "PASS activity-card withdrawal and edit resubmission\n";
      service.Login("admin", "Campus2026!");
      const auto user_id = service.CreateUser("checker2-mistake", "错误姓名", "participant",
                                              "Campus2026!", campus::RequestId());
      campus::Window admin_window(database);
      admin_window.show();
      QApplication::processEvents();
      Visible<QLineEdit>(&admin_window, "loginUsername")->setText("admin");
      Visible<QLineEdit>(&admin_window, "loginPassword")->setText("Campus2026!");
      Visible<QPushButton>(&admin_window, "loginButton")->click();
      QApplication::processEvents();
      for (auto* button : admin_window.findChildren<QPushButton*>()) {
        if (button->isVisible() && button->property("page").toString() == "账号管理") {
          button->click();
          break;
        }
      }
      QApplication::processEvents();
      const auto edit_user_button = "editUserButton_" + user_id;
      Visible<QPushButton>(&admin_window, edit_user_button.c_str())->click();
      QApplication::processEvents();
      Visible<QLineEdit>(&admin_window, "editUserName")->clear();
      Commit(Visible<QLineEdit>(&admin_window, "editUserName"), "正确中文姓名");
      auto* role = Visible<QComboBox>(&admin_window, "editUserRole");
      role->setCurrentIndex(role->findData("checker"));
      Visible<QPushButton>(&admin_window, "saveUserButton")->click();
      QApplication::processEvents();
      const auto edited_user = service.Users("checker2-mistake").front();
      Check(campus::Value(edited_user, "name") == "正确中文姓名",
            "account name input was not saved");
      Check(campus::Value(edited_user, "role") == "checker" &&
                campus::Value(edited_user, "enabled") == "1",
            "account editor only toggled enable state");
      std::cout << "PASS admin account editor saves Chinese name and corrected role\n";
      std::cout
          << "PASS complete draft editor, multiple sessions and tickets, submission and approval\n";
    }
    std::cout << "PASS organizer Chinese input and draft persistence in main window\n";
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    result = 1;
  }
  std::filesystem::remove_all(directory);
  return result;
}
