#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "campus/window.h"

namespace {
using campus::Number;
using campus::Value;
using Step = std::function<void(QDialog*)>;
int assertions = 0;
void Check(bool ok, const std::string& message) {
  ++assertions;
  if (!ok) {
    throw std::runtime_error(message);
  }
}
QString Text(const std::string& text) { return QString::fromStdString(text); }
void Pump() {
  for (int i = 0; i < 4; ++i) {
    QApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  }
}
template <typename T>
std::vector<T*> Visible(QObject* root, const QString& name = {}) {
  std::vector<T*> result;
  for (auto* item : root->findChildren<T*>(name)) {
    if (item->isVisible()) {
      result.push_back(item);
    }
  }
  return result;
}
template <typename T>
T* First(QObject* root, const QString& name = {}) {
  const auto items = Visible<T>(root, name);
  Check(!items.empty(), "missing widget: " + name.toStdString());
  return items.front();
}
QPushButton* Button(QObject* root, const QString& text, int index = 0) {
  for (auto* button : Visible<QPushButton>(root)) {
    if (button->text().simplified() == text.simplified()) {
      if (index-- == 0) {
        return button;
      }
    }
  }
  throw std::runtime_error("missing button: " + text.toStdString());
}
bool HasLabel(QObject* root, const QString& text) {
  for (auto* label : Visible<QLabel>(root)) {
    if (label->text().contains(text)) {
      return true;
    }
  }
  return false;
}
QFrame* Card(QObject* root, const QString& title) {
  for (auto* frame : Visible<QFrame>(root, "card")) {
    for (auto* label : Visible<QLabel>(frame)) {
      if (label->text() == title) {
        return frame;
      }
    }
  }
  throw std::runtime_error("missing card: " + title.toStdString());
}
void BoxButton(QDialog* dialog, bool accept) {
  auto* box = dialog->findChild<QDialogButtonBox*>();
  Check(box != nullptr, "dialog has no button box");
  auto* button = box->button(accept ? QDialogButtonBox::Ok : QDialogButtonBox::Cancel);
  Check(button != nullptr, "dialog missing OK/Cancel");
  button->click();
}
Step Form(const QStringList& values, bool accept = true, int integer = -1, int role = -1) {
  return [=](QDialog* dialog) {
    auto* form = dialog->findChild<QFormLayout*>();
    Check(form != nullptr, "not a form dialog");
    int index = 0;
    for (int i = 0; i < form->rowCount(); ++i) {
      auto* item = form->itemAt(i, QFormLayout::FieldRole);
      if (!item || !item->widget()) {
        continue;
      }
      if (auto* field = qobject_cast<QLineEdit*>(item->widget())) {
        if (index < values.size()) {
          field->setText(values[index++]);
        }
      }
    }
    if (integer >= 0) {
      First<QSpinBox>(dialog)->setValue(integer);
    }
    if (role >= 0) {
      First<QComboBox>(dialog)->setCurrentIndex(role);
    }
    BoxButton(dialog, accept);
  };
}
Step Input(const QString& value, bool accept = true) {
  return [=](QDialog* dialog) {
    auto* input = qobject_cast<QInputDialog*>(dialog);
    Check(input != nullptr, "not an input dialog");
    input->setTextValue(value);
    BoxButton(dialog, accept);
  };
}
Step Pick(int index = 0, bool accept = true) {
  return [=](QDialog* dialog) {
    Check(qobject_cast<QInputDialog*>(dialog) != nullptr, "not a selection dialog");
    First<QComboBox>(dialog)->setCurrentIndex(index);
    BoxButton(dialog, accept);
  };
}
Step Integer(int value, bool accept = true) {
  return [=](QDialog* dialog) {
    auto* input = qobject_cast<QInputDialog*>(dialog);
    Check(input != nullptr, "not an integer dialog");
    input->setIntValue(value);
    BoxButton(dialog, accept);
  };
}
Step Message(QMessageBox::StandardButton answer, const QString& contains = {}) {
  return [=](QDialog* dialog) {
    auto* message = qobject_cast<QMessageBox*>(dialog);
    Check(message != nullptr, "not a message box");
    Check(!message->text().contains("UNIQUE constraint failed") &&
              !message->text().contains("FOREIGN KEY constraint failed"),
          "raw database constraint exposed: " + message->text().toStdString());
    if (!contains.isEmpty()) {
      Check(message->text().contains(contains), "message did not contain " +
                                                    contains.toStdString() + ": " +
                                                    message->text().toStdString());
    }
    auto* button = message->button(answer);
    Check(button != nullptr, "message button missing");
    button->click();
  };
}
Step File(const std::string& path, bool accept = true) {
  return [=](QDialog* dialog) {
    auto* file = qobject_cast<QFileDialog*>(dialog);
    Check(file != nullptr, "not a file dialog");
    if (!accept) {
      auto* buttons = file->findChild<QDialogButtonBox*>();
      Check(buttons && buttons->button(QDialogButtonBox::Cancel), "file cancel button missing");
      buttons->button(QDialogButtonBox::Cancel)->click();
      return;
    }
    file->setDirectory(QFileInfo(Text(path)).absolutePath());
    file->selectFile(QFileInfo(Text(path)).fileName());
    auto* buttons = file->findChild<QDialogButtonBox*>();
    Check(buttons && buttons->button(QDialogButtonBox::Save), "save button missing");
    buttons->button(QDialogButtonBox::Save)->click();
  };
}
void Perform(const std::function<void()>& action, const std::vector<Step>& steps = {}) {
  std::size_t index = 0;
  std::string error;
  QTimer dialogs;
  dialogs.setInterval(1);
  QObject::connect(&dialogs, &QTimer::timeout, [&] {
    auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
    if (!dialog) {
      return;
    }
    try {
      if (index < steps.size()) {
        const auto step = steps[index++];
        step(dialog);
      } else {
        auto* message = qobject_cast<QMessageBox*>(dialog);
        error = "unexpected dialog: " +
                (message ? message->text().toStdString() : dialog->windowTitle().toStdString());
        dialog->reject();
      }
    } catch (const std::exception& exception) {
      error = exception.what();
      dialog->reject();
    }
  });
  dialogs.start();
  action();
  Pump();
  dialogs.stop();
  Check(error.empty(), error);
  Check(index == steps.size(), "expected dialog not opened");
}
struct Results {
  QJsonArray rows;
  int failed = 0;
  void Test(const QString& id, const QString& area, const QString& button, const QString& steps,
            const QString& expected, const std::function<void()>& action) {
    const int before = assertions;
    QString actual = expected;
    QString status = "PASS";
    try {
      action();
    } catch (const std::exception& error) {
      status = "FAIL";
      actual = Text(error.what());
      ++failed;
    }
    rows.append(QJsonObject{{"id", id},
                            {"area", area},
                            {"button", button},
                            {"steps", steps},
                            {"expected", expected},
                            {"actual", actual},
                            {"status", status},
                            {"assertions", assertions - before}});
    std::cout << status.toStdString() << " " << id.toStdString() << " " << button.toStdString();
    if (status == "FAIL") {
      std::cout << ": " << actual.toStdString();
    }
    std::cout << std::endl;
  }
};
struct Fixture {
  std::filesystem::path directory =
      std::filesystem::temp_directory_path() / ("campus-buttons-" + campus::RequestId());
  std::string path;
  std::int64_t time = campus::Now();
  std::unique_ptr<campus::Service> api;
  std::unique_ptr<campus::Window> window;
  explicit Fixture(bool seed = true) {
    std::filesystem::create_directory(directory);
    path = (directory / "test.db").string();
    api = std::make_unique<campus::Service>(path, [this] { return time; });
    if (seed) {
      api->SeedDemo();
    }
    window = std::make_unique<campus::Window>(path);
    window->show();
    Pump();
  }
  ~Fixture() {
    window.reset();
    api.reset();
    Pump();
    std::filesystem::remove_all(directory);
  }
  void Click(const QString& text, const std::vector<Step>& steps = {}, QObject* root = nullptr,
             int index = 0) {
    auto* button = Button(root ? root : window.get(), text, index);
    Perform([=] { button->click(); }, steps);
  }
  void Named(const QString& name, const std::vector<Step>& steps = {}) {
    auto* button = First<QPushButton>(window.get(), name);
    Perform([=] { button->click(); }, steps);
  }
  void Menu(const QString& text, const std::vector<Step>& steps = {}) {
    QAction* found = nullptr;
    for (auto* action : window->findChildren<QAction*>()) {
      if (action->text() == text && action->isEnabled()) {
        found = action;
      }
    }
    Check(found != nullptr, "missing menu: " + text.toStdString());
    Perform([=] { found->trigger(); }, steps);
  }
  void Nav(const QString& page) {
    for (auto* button : Visible<QPushButton>(window.get())) {
      if (button->property("page").toString() == page) {
        Perform([=] { button->click(); });
        return;
      }
    }
    throw std::runtime_error("missing navigation: " + page.toStdString());
  }
  void Login(const QString& user) {
    if (Visible<QLineEdit>(window.get(), "loginUsername").empty()) {
      Menu("退出登录");
    }
    First<QLineEdit>(window.get(), "loginUsername")->setText(user);
    First<QLineEdit>(window.get(), "loginPassword")->setText("Campus2026!");
    Named("loginButton");
    Check(Visible<QLineEdit>(window.get(), "loginUsername").empty(), "login did not enter account");
  }
  void Owner() { api->Login("organizer", "Campus2026!"); }
  void Admin() { api->Login("admin", "Campus2026!"); }
  std::string Publish(const std::string& name, int price = 0, int capacity = 2, int cutoff = 3600,
                      int start = 7200, int end = 10800) {
    Owner();
    campus::DraftSession session;
    session.details = {"测试场次",   "仙林测试教室", time + cutoff,
                       time + start, time + end,     capacity};
    campus::DraftTicket ticket;
    ticket.name = "普通票";
    ticket.price_cents = price;
    ticket.capacity = capacity;
    for (int i = 1; i <= capacity; ++i) {
      ticket.seats.push_back("S" + std::to_string(i));
    }
    session.tickets.push_back(ticket);
    const auto event =
        api->SaveDraft("", name, "测试类别", "按钮测试简介", {session}, true, campus::RequestId());
    const auto review = Value(api->Reviews().front(), "id");
    Admin();
    api->Decide(review, true, "通过", campus::RequestId());
    return event;
  }
  campus::Row Reg(const std::string& id) {
    Owner();
    for (const auto& row : api->Registrations()) {
      if (Value(row, "id") == id) {
        return row;
      }
    }
    throw std::runtime_error("registration not found");
  }
  void Select(const QString& title) {
    auto* card = Card(window.get(), title);
    QMouseEvent event(QEvent::MouseButtonPress, QPointF(4, 4), Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(card, &event);
    Pump();
  }
};

void LoginNavigation(Results& r) {
  Fixture f(false);
  r.Test("L01", "通用", "登录页底部三个导航", "依次点击发现活动、我的报名、操作记录",
         "保持登录页，账号框获得焦点", [&] {
           for (const auto& text : {"发现活动", "我的报名", "操作记录"}) {
             f.Click(text);
             Check(First<QLineEdit>(f.window.get(), "loginUsername")->hasFocus(),
                   "login navigation focus missing");
           }
         });
  r.Test("L02", "通用", "首次使用 · 初始化管理员", "填写账号密码，触发初始化菜单",
         "管理员创建成功且密码可登录", [&] {
           First<QLineEdit>(f.window.get(), "loginUsername")->setText("admin");
           First<QLineEdit>(f.window.get(), "loginPassword")->setText("Campus2026!");
           f.Menu("首次使用 · 初始化管理员");
           f.api->Login("admin", "Campus2026!");
         });
  r.Test("L03", "通用", "重复初始化", "再次点击初始化菜单", "提示已经初始化，不增加账号", [&] {
    f.Menu("首次使用 · 初始化管理员", {Message(QMessageBox::Ok, "已经初始化")});
    Check(f.api->Users().size() == 1, "duplicate bootstrap account");
  });
  r.Test("L04", "通用", "登录错误密码", "密码改错后点击登录", "拒绝并保持登录页", [&] {
    First<QLineEdit>(f.window.get(), "loginPassword")->setText("wrong");
    f.Named("loginButton", {Message(QMessageBox::Ok, "密码")});
    Check(!Visible<QLineEdit>(f.window.get(), "loginUsername").empty(), "bad password logged in");
  });
  r.Test("L05", "通用", "密码框回车登录", "填写正确密码并触发回车信号", "进入管理员界面", [&] {
    auto* password = First<QLineEdit>(f.window.get(), "loginPassword");
    password->setText("Campus2026!");
    Perform([=] { QMetaObject::invokeMethod(password, "returnPressed", Qt::DirectConnection); });
    Check(HasLabel(f.window.get(), "管理员"), "admin header missing");
  });
  r.Test("L06", "通用", "右上角更多菜单", "打开更多菜单后关闭", "菜单可见，关闭不改变登录身份",
         [&] {
           auto* more = First<QToolButton>(f.window.get(), "more");
           bool visible = false;
           QTimer::singleShot(10, [&] {
             visible = more->menu()->isVisible();
             more->menu()->close();
           });
           more->showMenu();
           Pump();
           Check(visible, "more menu did not open");
         });
  r.Test("L07", "通用", "退出登录", "触发退出登录菜单", "返回账号密码页", [&] {
    f.Menu("退出登录");
    Check(!Visible<QLineEdit>(f.window.get(), "loginUsername").empty(), "logout failed");
  });
  Fixture demo;
  const std::vector<std::pair<QString, QStringList>> roles{
      {"student", {"发现活动", "我的报名", "操作记录"}},
      {"organizer", {"我的活动", "报名与收退款", "统计与导出", "操作记录"}},
      {"admin", {"活动审核", "账号管理", "检票分配", "操作记录"}},
      {"checker", {"分配场次与检票"}}};
  int id = 0;
  for (const auto& role : roles) {
    r.Test(QString("N%1").arg(++id, 2, 10, QChar('0')), "导航", role.first + " 登录",
           "使用对应角色账号点击登录", "进入正确工作台，保持393×852", [&] {
             demo.Login(role.first);
             Check(demo.window->size() == QSize(393, 852), "mobile size changed");
           });
    for (const auto& page : role.second) {
      r.Test(QString("N%1").arg(++id, 2, 10, QChar('0')), "导航", role.first + " → " + page,
             "点击底部导航", "目标导航选中且对应页面加载", [&] {
               demo.Nav(page);
               bool checked = false;
               for (auto* tab : Visible<QPushButton>(demo.window.get())) {
                 if (tab->property("page").toString() == page) {
                   checked = tab->isChecked();
                 }
               }
               Check(checked, "tab not selected");
             });
    }
    if (role.first == "student") {
      r.Test("N-MSG", "导航", "消息通知", "触发右上角消息通知", "消息列表或空状态正常显示", [&] {
        demo.Menu("消息通知");
        Check(HasLabel(demo.window.get(), "消息通知"), "notifications page missing");
      });
    }
    if (role.first == "organizer") {
      r.Test("N-REQ", "导航", "我的申请菜单与页面按钮", "分别通过活动页按钮和右上角菜单进入",
             "两入口均进入我的申请", [&] {
               demo.Nav("我的活动");
               demo.Named("myApplicationsButton");
               Check(HasLabel(demo.window.get(), "我的申请"), "request button failed");
               demo.Menu("我的申请");
             });
    }
    r.Test(QString("N%1").arg(++id, 2, 10, QChar('0')), "导航", role.first + " 退出",
           "触发退出登录", "返回登录页", [&] {
             demo.Menu("退出登录");
             Check(!Visible<QLineEdit>(demo.window.get(), "loginUsername").empty(),
                   "logout failed");
           });
  }
}

void ParticipantButtons(Results& r) {
  Fixture f;
  f.Login("student");
  r.Test("P01", "参与者", "筛选取消", "打开筛选表单后点击取消", "不改变原筛选及活动结果", [&] {
    f.Click("筛选", {Form({}, false)});
    Check(Visible<QPushButton>(f.window.get()).size() > 3, "list disappeared");
  });
  r.Test("P02", "参与者", "筛选确认", "输入音乐会、文艺演出、仙林并确认", "仅显示同时匹配的音乐会",
         [&] {
           f.Click("筛选", {Form({"音乐会", "文艺演出", "仙林", ""})});
           Check(
               HasLabel(f.window.get(), "秋日草坪音乐会") && !HasLabel(f.window.get(), "大气活性"),
               "combined search wrong");
         });
  r.Test("P03", "参与者", "搜索框回车", "输入不存在的关键词并回车", "显示暂无记录", [&] {
    auto* q = First<QLineEdit>(f.window.get(), "eventKeyword");
    q->setText("不存在的活动xyz");
    Perform([=] { QMetaObject::invokeMethod(q, "returnPressed", Qt::DirectConnection); });
    Check(HasLabel(f.window.get(), "暂无记录"), "empty search failed");
  });
  r.Test("P04", "参与者", "清空组合筛选", "筛选四项全部清空后确认", "恢复两项演示活动", [&] {
    f.Click("筛选", {Form({"", "", "", ""})});
    Check(HasLabel(f.window.get(), "秋日草坪音乐会") && HasLabel(f.window.get(), "大气活性"),
          "reset search failed");
  });
  r.Test("P05", "参与者", "类别地点日期下拉框", "逐一选择首个非空选项再清空",
         "筛选更新且没有异常弹窗", [&] {
           for (auto* box : Visible<QComboBox>(f.window.get())) {
             if (box->count() > 1) {
               Perform([=] { box->setCurrentIndex(1); });
               Perform([=] { box->setCurrentIndex(0); });
             }
           }
         });
  r.Test("P06", "参与者", "查看活动", "点击音乐会卡片查看活动", "展示场次地点、截止和简介", [&] {
    f.Click("查看活动 ›", {}, Card(f.window.get(), "秋日草坪音乐会"));
    Check(HasLabel(f.window.get(), "选择场次") && HasLabel(f.window.get(), "报名截止"),
          "event detail incomplete");
  });
  r.Test("P07", "参与者", "返回发现活动", "从详情点击返回", "恢复发现活动列表", [&] {
    f.Click("‹ 返回发现活动");
    Check(!Visible<QLineEdit>(f.window.get(), "eventKeyword").empty(), "back failed");
  });
  const auto event = f.Publish("按钮报名活动");
  f.api->Login("student", "Campus2026!");
  f.Nav("发现活动");
  r.Test("P08", "参与者", "选择票种", "进入新活动详情并选择票种", "显示票种余量及应付金额", [&] {
    f.Click("查看活动 ›", {}, Card(f.window.get(), "按钮报名活动"));
    f.Click("选择票种");
    Check(HasLabel(f.window.get(), "应付金额") && !Visible<QRadioButton>(f.window.get()).empty(),
          "ticket page missing");
  });
  r.Test("P09", "参与者", "票种单选按钮", "点击票种单选框", "选中对应票种", [&] {
    auto* radio = First<QRadioButton>(f.window.get());
    radio->click();
    Check(radio->isChecked(), "radio not checked");
  });
  r.Test("P10", "参与者", "返回活动详情", "从票种页面点击返回详情再进入",
         "场次仍可选择，票种页可再次打开", [&] {
           f.Click("‹ 返回活动详情");
           Check(HasLabel(f.window.get(), "按钮报名活动"), "detail not restored");
           f.Click("选择票种");
         });
  std::string registration;
  r.Test("P11", "参与者", "确认报名", "免费票点击确认报名", "创建已报名记录并进入我的报名", [&] {
    f.Click("确认报名");
    f.api->Login("student", "Campus2026!");
    auto rows = f.api->Registrations(event);
    Check(rows.size() == 1 && Value(rows.front(), "status") == "registered", "registration failed");
    registration = Value(rows.front(), "id");
  });
  r.Test("P12", "参与者", "取消报名卡片按钮", "点击新活动卡片取消报名", "本人记录取消且保留历史",
         [&] {
           f.Click("取消报名", {}, Card(f.window.get(), "按钮报名活动"));
           Check(Value(f.Reg(registration), "status") == "cancelled", "cancellation failed");
         });
  r.Test("P13", "参与者", "重复报名拦截", "取消后再次从票种页确认报名",
         "拒绝重新报名，记录仍只有一条", [&] {
           f.Nav("发现活动");
           f.Click("查看活动 ›", {}, Card(f.window.get(), "按钮报名活动"));
           f.Click("选择票种");
           f.Click("确认报名", {Message(QMessageBox::Ok)});
           f.api->Login("student", "Campus2026!");
           Check(f.api->Registrations(event).size() == 1, "duplicate registration persisted");
         });
  r.Test("P14", "参与者", "取消本人报名菜单", "选中演示音乐会报名并通过菜单取消",
         "菜单取消正确的本人待付款记录", [&] {
           f.Nav("我的报名");
           f.Select("秋日草坪音乐会");
           f.Menu("取消本人报名");
           Check(Value(f.Reg("1"), "status") == "cancelled", "menu cancellation failed");
         });
  r.Test("P15", "参与者", "多个票种单选与价格更新", "进入音乐会票种页并切换普通票和前排票",
         "选择改变，应付金额分别为20和35元", [&] {
           f.Nav("发现活动");
           f.Click("查看活动 ›", {}, Card(f.window.get(), "秋日草坪音乐会"));
           f.Click("选择票种");
           const auto choices = Visible<QRadioButton>(f.window.get());
           Check(choices.size() == 2, "two ticket choices missing");
           choices[1]->click();
           Pump();
           Check(HasLabel(f.window.get(), "应付金额：¥35.00"), "second ticket price wrong");
           choices[0]->click();
           Pump();
           Check(HasLabel(f.window.get(), "应付金额：¥20.00"), "first ticket price wrong");
         });
}

void DraftButtons(Results& r) {
  Fixture f;
  f.Login("organizer");
  f.Owner();
  const auto baseline = f.api->Events().size();
  r.Test("D01", "完整草稿", "新建活动与取消返回", "打开完整表单后点击取消",
         "不创建活动，返回我的活动", [&] {
           f.Named("createEventButton");
           f.Click("取消，返回我的活动");
           Check(f.api->Events().size() == baseline, "cancel created event");
         });
  r.Test("D02", "完整草稿", "保存空名称", "新建活动，名称留空点击保存", "提示必填并保留表单", [&] {
    f.Named("createEventButton");
    f.Named("saveEventButton", {Message(QMessageBox::Ok, "名称")});
    Check(!Visible<QLineEdit>(f.window.get(), "createEventName").empty(), "invalid save lost form");
  });
  r.Test("D03", "完整草稿", "添加场次 ＋", "点击添加场次", "表单增加第二个场次", [&] {
    f.Click("添加场次 ＋");
    Check(Visible<QLineEdit>(f.window.get(), "draftSessionName").size() == 2, "session not added");
  });
  r.Test("D04", "完整草稿", "移除此场次", "移除第二场", "只剩第一场且未写入数据库", [&] {
    f.Click("移除此场次", {}, nullptr, 1);
    Check(Visible<QLineEdit>(f.window.get(), "draftSessionName").size() == 1,
          "session removal failed");
    Check(f.api->Events().size() == baseline, "unsaved edit persisted");
  });
  r.Test("D05", "完整草稿", "添加票种 ＋", "在第一场点击添加票种", "出现第二个票种表单", [&] {
    f.Click("添加票种 ＋");
    Check(Visible<QLineEdit>(f.window.get(), "draftTicketName").size() == 2, "ticket not added");
  });
  r.Test("D06", "完整草稿", "移除此票种", "移除第二个票种", "保留首个票种", [&] {
    f.Click("移除此票种", {}, nullptr, 1);
    Check(Visible<QLineEdit>(f.window.get(), "draftTicketName").size() == 1,
          "ticket removal failed");
  });
  r.Test("D07", "完整草稿", "按容量生成座位编号", "容量改为3，点击生成座位", "生成3个不重复编号",
         [&] {
           First<QSpinBox>(f.window.get(), "draftTicketCapacity")->setValue(3);
           f.Click("按容量生成座位编号");
           auto text = First<QTextEdit>(f.window.get(), "draftTicketSeats")->toPlainText();
           Check(text.split(',').size() == 3 && text.contains("003"), "seat generator wrong");
         });
  std::string draft;
  r.Test("D08", "完整草稿", "保存活动草稿", "填写中文基本信息、地点并保存",
         "场次票种及3个座位整体保存", [&] {
           First<QLineEdit>(f.window.get(), "createEventName")->setText("完整按钮草稿");
           First<QLineEdit>(f.window.get(), "createEventCategory")->setText("按钮类别");
           First<QTextEdit>(f.window.get(), "createEventDescription")->setPlainText("中文活动简介");
           First<QLineEdit>(f.window.get(), "draftSessionLocation")->setText("按钮地点");
           f.Named("saveEventButton");
           campus::Search q;
           q.keyword = "完整按钮草稿";
           auto events = f.api->Events(q);
           Check(events.size() == 1, "draft not saved");
           draft = Value(events.front(), "id");
           auto content = f.api->DraftSessions(draft);
           Check(content.size() == 1 && content.front().tickets.front().seats.size() == 3,
                 "draft nested content wrong");
         });
  r.Test("D09", "完整草稿", "编辑活动卡片", "重新打开已存草稿", "载入名称及原场次票种", [&] {
    f.Named("editEventButton_" + Text(draft));
    Check(First<QLineEdit>(f.window.get(), "createEventName")->text() == "完整按钮草稿",
          "edit reload failed");
    f.Click("取消，返回我的活动");
  });
  std::string review;
  r.Test("D10", "完整草稿", "卡片提交发布审核", "点击草稿卡片提交审核", "新建待审核发布申请", [&] {
    f.Click("提交发布审核", {}, Card(f.window.get(), "完整按钮草稿"));
    review = Value(f.api->Reviews().front(), "id");
    Check(Value(f.api->Reviews().front(), "status") == "pending", "submit failed");
  });
  r.Test("D11", "完整草稿", "查看待审核申请", "点击活动卡片查看待审核申请",
         "进入我的申请且显示撤回按钮", [&] {
           f.Click("查看待审核申请", {}, Card(f.window.get(), "完整按钮草稿"));
           Check(HasLabel(f.window.get(), "我的申请"), "pending view failed");
         });
  r.Test("D12", "完整草稿", "撤回申请卡片按钮", "点击待审核发布申请撤回",
         "申请已撤回，草稿可以继续编辑", [&] {
           f.Click("撤回申请", {}, Card(f.window.get(), "完整按钮草稿"));
           Check(Value(f.api->Reviews().front(), "status") == "withdrawn", "review not withdrawn");
         });
  r.Test("D13", "完整草稿", "菜单提交发布审核", "回到活动页触发提交菜单", "可在撤回后重新提交",
         [&] {
           f.Nav("我的活动");
           f.Menu("提交发布审核");
           Check(Value(f.api->Reviews().front(), "status") == "pending", "menu submit failed");
           review = Value(f.api->Reviews().front(), "id");
         });
  r.Test("D14", "完整草稿", "撤回待审核申请活动按钮", "从活动卡片直接撤回", "原申请撤回且历史保留",
         [&] {
           f.Named("withdrawReviewButton_" + Text(review));
           Check(Value(f.api->Reviews().front(), "status") == "withdrawn",
                 "activity withdrawal failed");
         });
  r.Test("D14B", "完整草稿", "提交不合法时间", "将报名截止设在开始之后，点击保存提交",
         "拒绝提交并保留表单，原草稿未修改", [&] {
           f.Named("editEventButton_" + Text(draft));
           auto* deadline = First<QDateTimeEdit>(f.window.get(), "draftSessionDeadline");
           auto* start = First<QDateTimeEdit>(f.window.get(), "draftSessionStart");
           deadline->setDateTime(start->dateTime().addSecs(3600));
           f.Named("submitEventButton", {Message(QMessageBox::Ok, "时间")});
           Check(!Visible<QLineEdit>(f.window.get(), "createEventName").empty(),
                 "invalid submit lost form");
           Check(Value(f.api->Reviews().front(), "status") == "withdrawn",
                 "invalid submit created review");
           f.Click("取消，返回我的活动");
         });
  r.Test("D15", "完整草稿", "保存并提交发布审核", "编辑已配置草稿并点击保存提交",
         "完整配置和新申请共同保存", [&] {
           f.Named("editEventButton_" + Text(draft));
           f.Named("submitEventButton");
           Check(Value(f.api->Reviews().front(), "status") == "pending", "form submit failed");
         });
  r.Test("D16", "完整草稿", "查看原版本与申请内容", "打开申请内容对照后关闭",
         "弹窗含活动和场次快照", [&] {
           f.Click("查看原版本与申请内容 ›", {[](QDialog* d) {
                     auto* text = First<QTextEdit>(d);
                     Check(text->toPlainText().contains("完整按钮草稿") &&
                               text->toPlainText().contains("SESSION"),
                           "review snapshot incomplete");
                     d->close();
                   }},
                   Card(f.window.get(), "完整按钮草稿"));
         });
  r.Test("D16B", "完整草稿", "查看原内容与候选内容菜单", "通过菜单打开申请快照并关闭",
         "显示活动快照且无修改", [&] {
           f.Menu("查看原内容与候选内容", {[](QDialog* dialog) {
                    Check(First<QTextEdit>(dialog)->toPlainText().contains("完整按钮草稿"),
                          "snapshot menu wrong");
                    dialog->close();
                  }});
         });
  r.Test("D17", "完整草稿", "撤回选中申请菜单", "触发撤回菜单", "最新申请已撤回", [&] {
    f.Menu("撤回选中申请");
    Check(Value(f.api->Reviews().front(), "status") == "withdrawn", "menu withdrawal failed");
  });
  r.Test("D18", "完整草稿", "管理员批准首次发布", "重新提交草稿后管理员在卡片点击通过",
         "首次发布成功且创建版本快照", [&] {
           f.Owner();
           f.api->Submit(draft, campus::RequestId());
           f.Login("admin");
           f.Click("通过审核", {Input("首次发布通过")}, Card(f.window.get(), "完整按钮草稿"));
           f.Admin();
           campus::Search query;
           query.keyword = "完整按钮草稿";
           Check(Value(f.api->Events(query).front(), "status") == "published" &&
                     f.api->Versions(draft).size() == 1,
                 "first publication failed");
         });
}

void LegacyDraftButtons(Results& r) {
  Fixture f;
  f.Owner();
  const auto event = f.api->CreateEvent("菜单草稿", "测试类别", "测试简介", campus::RequestId());
  f.Login("organizer");
  const auto cutoff = Text(campus::TimeText(f.time + 86400));
  const auto start = Text(campus::TimeText(f.time + 90000));
  const auto end = Text(campus::TimeText(f.time + 93600));
  r.Test("O01", "草稿快捷菜单", "添加场次取消", "打开添加场次表单并取消", "草稿没有新增场次", [&] {
    f.Menu("添加场次（草稿）", {Form({}, false)});
    Check(f.api->Sessions(event).empty(), "cancel added session");
  });
  std::string session;
  r.Test("O02", "草稿快捷菜单", "添加场次确认", "填写名称地点日期容量并确认", "新增场次且配置正确",
         [&] {
           f.Menu("添加场次（草稿）",
                  {Form({"菜单场次", "菜单地点", cutoff, start, end}, true, 4)});
           auto rows = f.api->Sessions(event);
           Check(rows.size() == 1 && Value(rows.front(), "capacity") == "4", "session not saved");
           session = Value(rows.front(), "id");
         });
  r.Test("O03", "草稿快捷菜单", "添加票种取消选择", "选择场次弹窗中取消",
         "不增加票种且不弹错误警告", [&] {
           f.Menu("添加票种与座位（草稿）", {Pick(0, false)});
           Check(f.api->Tickets(session).empty(), "cancel added ticket");
         });
  r.Test("O04", "草稿快捷菜单", "添加票种确认", "选择场次并填写票名价格座位", "增加2座位票种", [&] {
    f.Menu("添加票种与座位（草稿）", {Pick(), Form({"菜单票种", "Z01,Z02"}, true, 2000)});
    Check(f.api->Tickets(session).size() == 1 &&
              Value(f.api->Tickets(session).front(), "price") == "2000",
          "ticket not saved");
  });
  r.Test("O04B", "草稿快捷菜单", "新增票种座位冲突", "使用原票种已经占用的Z01再次创建票种",
         "提示重复座位且不留下半个票种", [&] {
           f.Menu("添加票种与座位（草稿）",
                  {Pick(), Form({"重复座位票", "Z01"}, true, 0), Message(QMessageBox::Ok)});
           Check(f.api->Tickets(session).size() == 1, "conflicting ticket partially saved");
         });
  r.Test("O05", "草稿快捷菜单", "编辑草稿场次取消", "选择场次后取消名称输入",
         "原场次不变且无错误警告", [&] {
           f.Menu("编辑草稿场次", {Pick(), Input("", false)});
           Check(Value(f.api->Sessions(event).front(), "name") == "菜单场次",
                 "cancel changed session");
         });
  r.Test("O06", "草稿快捷菜单", "编辑草稿场次确认", "依次填写名称地点时间容量", "修改整组场次字段",
         [&] {
           f.Menu("编辑草稿场次", {Pick(), Input("修改场次"), Input("修改地点"), Input(cutoff),
                                   Input(start), Input(end), Integer(5)});
           Check(Value(f.api->Sessions(event).front(), "name") == "修改场次" &&
                     Value(f.api->Sessions(event).front(), "capacity") == "5",
                 "session edit failed");
         });
  r.Test("O07", "草稿快捷菜单", "编辑草稿票种确认", "选择票种后修改名称座位和价格",
         "票种名称价格座位同步更新", [&] {
           f.Menu("编辑草稿票种与座位",
                  {Pick(), Pick(), Input("修改票种"), Input("Y01,Y02,Y03"), Integer(0)});
           auto rows = f.api->Tickets(session);
           Check(Value(rows.front(), "name") == "修改票种" && Value(rows.front(), "price") == "0" &&
                     Value(rows.front(), "capacity") == "3",
                 "ticket edit failed");
         });
  r.Test("O07B", "草稿快捷菜单", "编辑票种重复座位", "把原票种座位填写为两个相同编号",
         "提示重复且原票种和座位整体回滚", [&] {
           f.Menu("编辑草稿票种与座位", {Pick(), Pick(), Input("不应保存"), Input("Q01,Q01"),
                                         Integer(0), Message(QMessageBox::Ok, "重复")});
           Check(Value(f.api->Tickets(session).front(), "name") == "修改票种" &&
                     Value(f.api->Tickets(session).front(), "capacity") == "3",
                 "invalid ticket edit partly saved");
         });
  r.Test("O08", "草稿快捷菜单", "删除票种取消", "输入删除范围后取消场次选择",
         "票种不删除，无错误警告", [&] {
           f.Menu("删除草稿场次 / 票种", {Input("ticket"), Pick(0, false)});
           Check(f.api->Tickets(session).size() == 1, "cancel deleted ticket");
         });
  r.Test("O09", "草稿快捷菜单", "删除草稿票种", "选择ticket范围、场次和票种", "票种及座位删除",
         [&] {
           f.Menu("删除草稿场次 / 票种", {Input("ticket"), Pick(), Pick()});
           Check(f.api->Tickets(session).empty(), "ticket deletion failed");
         });
  r.Test("O10", "草稿快捷菜单", "删除草稿场次", "选择session范围并确认场次", "场次删除", [&] {
    f.Menu("删除草稿场次 / 票种", {Input("session"), Pick()});
    Check(f.api->Sessions(event).empty(), "session deletion failed");
  });
  r.Test("O11", "草稿快捷菜单", "删除草稿菜单", "触发删除草稿菜单", "草稿活动消失", [&] {
    f.Menu("删除草稿");
    campus::Search q;
    q.keyword = "菜单草稿";
    Check(f.api->Events(q).empty(), "draft deletion failed");
  });
  r.Test("O12", "草稿快捷菜单", "删除草稿卡片", "建立未提交草稿并点击卡片删除", "仅删除目标草稿",
         [&] {
           f.api->CreateEvent("卡片删除草稿", "类别", "简介", campus::RequestId());
           f.Nav("我的活动");
           f.Click("删除草稿", {}, Card(f.window.get(), "卡片删除草稿"));
           campus::Search q;
           q.keyword = "卡片删除草稿";
           Check(f.api->Events(q).empty(), "card delete failed");
         });
}

void ReviewButtons(Results& r) {
  Fixture f;
  const auto event = f.Publish("审核按钮活动");
  f.Owner();
  f.Login("organizer");
  r.Test("R01", "审核", "已发布编辑取消", "打开已发布编辑表单后取消", "原版本不变，无新增申请",
         [&] {
           f.Click("编辑活动", {}, Card(f.window.get(), "审核按钮活动"));
           Check(f.window->findChild<QLineEdit*>("draftSessionLocation") != nullptr,
                 "published editor has no session fields");
           f.Click("取消，返回我的活动");
           Check(f.api->Reviews().size() == 3, "cancel created review");
         });
  std::string review;
  r.Test("R02", "审核", "编辑活动 / 申请发布后修改", "修改基本信息并确认",
         "完整表单的活动、场次和票种生成同一候选版本，原内容不变", [&] {
           f.Menu("编辑活动 / 申请发布后修改");
           f.window->findChild<QLineEdit*>("createEventName")->setText("审核按钮新名称");
           f.window->findChild<QLineEdit*>("createEventCategory")->setText("测试类别");
           f.window->findChild<QLineEdit*>("draftSessionLocation")->setText("完整表单候选地点");
           f.window->findChild<QLineEdit*>("draftSessionName")->setText("完整表单候选场次");
           f.window->findChild<QLineEdit*>("draftTicketName")->setText("完整表单候选票种");
           f.Click("提交修改审核");
           review = Value(f.api->Reviews().front(), "id");
           Check(Value(f.api->Events().front(), "name") == "审核按钮活动" &&
                     Value(f.api->Reviews().front(), "status") == "pending",
                 "proposal overwrote published event");
           Check(f.api->ReviewDetails(review).find("完整表单候选地点") != std::string::npos &&
                     f.api->ReviewDetails(review).find("完整表单候选票种") != std::string::npos,
                 "full form omitted session or ticket proposal");
         });
  f.Nav("我的活动");
  f.Named("myApplicationsButton");
  r.Test("R03", "审核", "为修改申请添加场次变更", "选择原场次，填写新地点、时间和容量",
         "候选场次保存，现行场次不变", [&] {
           f.Menu("为修改申请添加场次变更",
                  {Pick(), Input("新测试地点"), Input(Text(campus::TimeText(f.time + 3600))),
                   Input(Text(campus::TimeText(f.time + 7200))),
                   Input(Text(campus::TimeText(f.time + 10800))), Integer(3)});
           Check(Value(f.api->Sessions(event).front(), "location") == "仙林测试教室",
                 "proposal applied early");
           Check(f.api->ReviewDetails(review).find("新测试地点") != std::string::npos,
                 "session proposal missing");
         });
  r.Test("R04", "审核", "修改申请中新增票种", "选择场次，选择新增，填写票价和座位",
         "候选票种保存但未发布", [&] {
           f.Menu("为修改申请修改 / 新增票种", {Pick(), Message(QMessageBox::Yes),
                                                Input("新增票种"), Input("N01"), Integer(500)});
           Check(f.api->Tickets(Value(f.api->Sessions(event).front(), "id")).size() == 1,
                 "new ticket applied early");
         });
  r.Test("R05", "审核", "修改申请中修改原票种", "选择已有票种并修改候选名称", "候选原票种可保存",
         [&] {
           f.Menu("为修改申请修改 / 新增票种", {Pick(), Message(QMessageBox::No), Pick(),
                                                Input("原票种新名称"), Input("S1,S2"), Integer(0)});
           Check(f.api->ReviewDetails(review).find("原票种新名称") != std::string::npos,
                 "ticket proposal missing");
         });
  f.Login("admin");
  r.Test("R06", "审核", "通过审核取消", "点击通过后取消意见输入", "申请仍待审核，无错误警告", [&] {
    f.Click("通过审核", {Input("", false)}, Card(f.window.get(), "审核按钮活动"));
    f.Admin();
    Check(Value(f.api->Reviews().front(), "status") == "pending", "cancel approved review");
  });
  r.Test("R07", "审核", "通过审核确认", "填写意见并确认", "候选名称地点票种版本一起生效", [&] {
    f.Click("通过审核", {Input("内容合法")}, Card(f.window.get(), "审核按钮活动"));
    Check(Value(f.api->Events().front(), "name") == "审核按钮新名称", "approval not applied");
    Check(Value(f.api->Sessions(event).front(), "location") == "新测试地点", "session not applied");
    Check(f.api->Tickets(Value(f.api->Sessions(event).front(), "id")).size() == 2,
          "ticket not applied");
  });
  f.Owner();
  review = f.api->ProposeEdit(event, "拒绝候选", "测试类别", "简介", campus::RequestId());
  f.Login("admin");
  r.Test("R08", "审核", "拒绝申请空原因", "拒绝时输入空原因", "明确拒绝空原因，申请保持待审核",
         [&] {
           f.Click("拒绝申请", {Input(""), Message(QMessageBox::Ok, "原因")},
                   Card(f.window.get(), "审核按钮新名称"));
           f.Admin();
           Check(Value(f.api->Reviews().front(), "status") == "pending",
                 "empty rejection changed review");
         });
  r.Test("R09", "审核", "拒绝申请有效原因", "填写拒绝原因并确认", "申请被拒绝，原发布内容保留",
         [&] {
           f.Click("拒绝申请", {Input("资料不足")}, Card(f.window.get(), "审核按钮新名称"));
           Check(Value(f.api->Reviews().front(), "status") == "rejected" &&
                     Value(f.api->Events().front(), "name") == "审核按钮新名称",
                 "rejection failed");
         });
  int number = 0;
  for (const auto& scope : {"event", "session", "ticket"}) {
    const auto name = "取消范围" + std::string(scope);
    const auto target = f.Publish(name);
    f.Owner();
    f.Login("organizer");
    r.Test(QString("R-C%1").arg(++number), "审核", "申请取消 " + Text(scope),
           "从活动卡片选择对应取消范围", "生成范围正确的取消审核申请", [&] {
             std::vector<Step> steps{Input(scope)};
             if (std::string(scope) != "event") {
               steps.push_back(Pick());
             }
             if (std::string(scope) == "ticket") {
               steps.push_back(Pick());
             }
             f.Click("申请取消", steps, Card(f.window.get(), Text(name)));
             Check(Value(f.api->Reviews().front(), "scope") == scope, "cancel scope wrong");
           });
    f.Login("admin");
    r.Test(QString("R-CA%1").arg(number), "审核", "批准取消 " + Text(scope),
           "管理员通过对应范围的取消申请", "目标活动/场次/票种取消", [&] {
             f.Click("通过审核", {Input("同意取消")}, Card(f.window.get(), Text(name)));
             f.Admin();
             const auto session = Value(f.api->Sessions(target).front(), "id");
             if (std::string(scope) == "event") {
               campus::Search q;
               q.keyword = name;
               Check(Value(f.api->Events(q).front(), "status") == "cancelled",
                     "event not cancelled");
             } else if (std::string(scope) == "session") {
               Check(Value(f.api->Sessions(target).front(), "cancelled") == "1",
                     "session not cancelled");
             } else {
               Check(Value(f.api->Tickets(session).front(), "cancelled") == "1",
                     "ticket not cancelled");
             }
           });
  }
}

void AccountAssignmentButtons(Results& r) {
  Fixture f;
  f.Admin();
  f.Login("admin");
  f.Nav("账号管理");
  const auto count = f.api->Users().size();
  r.Test("A01", "账号", "创建新账号取消", "打开创建表单后取消", "账号数不变", [&] {
    f.Click("创建新账号", {Form({}, false)});
    Check(f.api->Users().size() == count, "cancel created user");
  });
  r.Test("A02", "账号", "创建新账号确认", "填写中文姓名、密码，选择检票角色",
         "新账号可见且角色正确", [&] {
           f.Click("创建新账号",
                   {Form({"button_checker", "按钮检票员", "Campus2026!"}, true, -1, 2)});
           auto rows = f.api->Users("button_checker");
           Check(rows.size() == 1 && Value(rows.front(), "role") == "checker", "user not created");
         });
  const auto checker = Value(f.api->Users("button_checker").front(), "id");
  r.Test("A03", "账号", "重复账号创建", "再次使用相同登录名创建", "拒绝重复账号且无第二条记录",
         [&] {
           f.Click("创建新账号", {Form({"button_checker", "重复", "Campus2026!"}, true, -1, 2),
                                  Message(QMessageBox::Ok)});
           Check(f.api->Users("button_checker").size() == 1, "duplicate user created");
         });
  r.Test("A04", "账号", "创建账号短密码", "使用不足8位密码创建", "提示密码要求，不创建账号", [&] {
    f.Click("创建新账号",
            {Form({"short_password", "短密码", "123"}), Message(QMessageBox::Ok, "8位")});
    Check(f.api->Users("short_password").empty(), "short password accepted");
  });
  r.Test("A05", "账号", "编辑 › 与取消返回", "打开新增检票员编辑页，修改后取消", "姓名未保存", [&] {
    f.Named("editUserButton_" + Text(checker));
    First<QLineEdit>(f.window.get(), "editUserName")->setText("取消的姓名");
    f.Click("取消，返回账号管理");
    Check(Value(f.api->Users("button_checker").front(), "name") == "按钮检票员",
          "cancel saved user");
  });
  r.Test("A06", "账号", "保存账号修改", "修改姓名并将启用改为停用后保存", "姓名与启用状态同时保存",
         [&] {
           f.Named("editUserButton_" + Text(checker));
           First<QLineEdit>(f.window.get(), "editUserName")->setText("已修改检票员");
           First<QCheckBox>(f.window.get(), "editUserEnabled")->setChecked(false);
           f.Named("saveUserButton");
           auto user = f.api->Users("button_checker").front();
           Check(Value(user, "name") == "已修改检票员" && Value(user, "enabled") == "0",
                 "user edit failed");
         });
  r.Test("A07", "账号", "启用 / 停用菜单", "选中检票员卡片后触发启用菜单", "仅目标账号启用", [&] {
    f.Select("button_checker");
    f.Menu("启用 / 停用选中账号");
    Check(Value(f.api->Users("button_checker").front(), "enabled") == "1", "enable failed");
  });
  r.Test("A07B", "账号", "停用及重新启用菜单", "对同一人员先停用再启用",
         "状态依次改变且其他账号不变", [&] {
           f.Select("button_checker");
           f.Menu("启用 / 停用选中账号");
           Check(Value(f.api->Users("button_checker").front(), "enabled") == "0",
                 "disable menu failed");
           f.Select("button_checker");
           f.Menu("启用 / 停用选中账号");
           Check(Value(f.api->Users("button_checker").front(), "enabled") == "1",
                 "re-enable menu failed");
         });
  r.Test("A08", "账号", "编辑账号菜单", "选中检票员并通过菜单进入编辑", "显示正确账号和当前角色",
         [&] {
           f.Select("button_checker");
           f.Menu("编辑账号");
           Check(First<QLineEdit>(f.window.get(), "editUserName")->text() == "已修改检票员",
                 "wrong user edited");
           f.Click("取消，返回账号管理");
         });
  r.Test("A08B", "账号", "编辑角色与空姓名拒绝", "先尝试空姓名，再修正角色并保存，最后改回检票员",
         "空姓名被拒绝，合法角色修改生效", [&] {
           f.Named("editUserButton_" + Text(checker));
           First<QLineEdit>(f.window.get(), "editUserName")->clear();
           f.Named("saveUserButton", {Message(QMessageBox::Ok, "姓名")});
           First<QLineEdit>(f.window.get(), "editUserName")->setText("已修改检票员");
           auto* role = First<QComboBox>(f.window.get(), "editUserRole");
           role->setCurrentIndex(role->findData("participant"));
           f.Named("saveUserButton");
           Check(Value(f.api->Users("button_checker").front(), "role") == "participant",
                 "role correction failed");
           f.Named("editUserButton_" + Text(checker));
           role = First<QComboBox>(f.window.get(), "editUserRole");
           role->setCurrentIndex(role->findData("checker"));
           f.Named("saveUserButton");
         });
  r.Test("A09", "账号", "保护当前管理员", "打开当前管理员编辑表单", "自身角色和启用控件禁用", [&] {
    f.Named("editUserButton_1");
    Check(!First<QComboBox>(f.window.get(), "editUserRole")->isEnabled() &&
              !First<QCheckBox>(f.window.get(), "editUserEnabled")->isEnabled(),
          "self admin controls enabled");
    f.Click("取消，返回账号管理");
  });
  r.Test("A09B", "账号", "当前管理员停用菜单", "选择admin卡片并触发停用菜单",
         "拒绝自我停用，管理员仍启用", [&] {
           f.Select("admin");
           f.Menu("启用 / 停用选中账号", {Message(QMessageBox::Ok, "管理员")});
           Check(Value(f.api->Users("admin").front(), "enabled") == "1", "self disabled via menu");
         });
  f.Nav("检票分配");
  r.Test("A10", "检票分配", "确认分配", "为音乐会新增button_checker", "列表显示2名已分配人员", [&] {
    auto* people = First<QComboBox>(f.window.get(), "assignmentChecker");
    people->setCurrentIndex(people->findData(Text(checker)));
    f.Named("assignmentConfirm");
    Check(f.api->AssignmentCheckers("1", true).size() == 2, "assignment not added");
  });
  r.Test("A11", "检票分配", "新增候选排除重复", "新增完成后检查候选与确认按钮",
         "已分配人员不重复出现，空列表不能提交", [&] {
           Check(First<QComboBox>(f.window.get(), "assignmentChecker")->count() == 0 &&
                     !First<QPushButton>(f.window.get(), "assignmentConfirm")->isEnabled(),
                 "duplicate remains assignable");
         });
  r.Test("A12", "检票分配", "确认撤销分配", "切换撤销模式，选择button_checker并撤销",
         "仅撤销所选人员，保留其他检票员", [&] {
           First<QComboBox>(f.window.get(), "assignmentOperation")->setCurrentIndex(1);
           Pump();
           auto* people = First<QComboBox>(f.window.get(), "assignmentChecker");
           people->setCurrentIndex(people->findData(Text(checker)));
           f.Named("assignmentConfirm");
           Check(f.api->AssignmentCheckers("1", true).size() == 1, "assignment not removed");
         });
  r.Test("A13", "检票分配", "活动场次人员联动", "切换新增模式和免费讲座，分配同一人员",
         "新场次可独立分配，旧场次不受影响", [&] {
           First<QComboBox>(f.window.get(), "assignmentOperation")->setCurrentIndex(0);
           Pump();
           auto* events = First<QComboBox>(f.window.get(), "assignmentEvent");
           events->setCurrentIndex(events->findData("2"));
           Pump();
           auto* people = First<QComboBox>(f.window.get(), "assignmentChecker");
           people->setCurrentIndex(people->findData(Text(checker)));
           f.Named("assignmentConfirm");
           Check(f.api->AssignmentCheckers("2", true).size() == 1,
                 "second session assignment failed");
         });
  const auto overlap = f.Publish("时间冲突按钮活动", 0, 2, 7200, 10800, 18000);
  f.Admin();
  f.api->Assign("1", checker, false, campus::RequestId());
  r.Test("A14", "检票分配", "刷新与时间冲突过滤", "刷新并切换到时间重叠场次",
         "冲突检票员不显示为可新增", [&] {
           f.Click("刷新活动与人员");
           auto* events = First<QComboBox>(f.window.get(), "assignmentEvent");
           events->setCurrentIndex(events->findData(Text(overlap)));
           Pump();
           Check(First<QComboBox>(f.window.get(), "assignmentChecker")->count() == 0 &&
                     !First<QPushButton>(f.window.get(), "assignmentConfirm")->isEnabled(),
                 "time conflict not filtered");
         });
  r.Test("A15", "检票分配", "创建新人员后刷新", "后台新增第三检票员，点击刷新",
         "出现新可分配账号并启用确认按钮", [&] {
           f.api->CreateUser("third_checker", "第三检票员", "checker", "Campus2026!",
                             campus::RequestId());
           f.Click("刷新活动与人员");
           Check(First<QComboBox>(f.window.get(), "assignmentChecker")->count() == 1 &&
                     First<QPushButton>(f.window.get(), "assignmentConfirm")->isEnabled(),
                 "refresh missed new account");
         });
  const auto now = f.time;
  f.time -= 86400;
  const auto past = f.Publish("已结束分配按钮活动");
  f.Admin();
  const auto past_session = Value(f.api->Sessions(past).front(), "id");
  f.api->Assign(past_session, "5", false, campus::RequestId());
  f.time = now;
  r.Test("A16", "检票分配", "已结束场次切换与撤销",
         "新增模式不显示过期活动，撤销模式可撤销历史授权", "不分配过期场次，历史授权可移除", [&] {
           f.Click("刷新活动与人员");
           Check(First<QComboBox>(f.window.get(), "assignmentEvent")->findData(Text(past)) < 0,
                 "expired event shown for new assignment");
           First<QComboBox>(f.window.get(), "assignmentOperation")->setCurrentIndex(1);
           Pump();
           auto* events = First<QComboBox>(f.window.get(), "assignmentEvent");
           events->setCurrentIndex(events->findData(Text(past)));
           Pump();
           f.Named("assignmentConfirm");
           Check(f.api->AssignmentCheckers(past_session, true).empty(),
                 "past assignment not removable");
         });
}

void MoneyButtons(Results& r) {
  Fixture f;
  f.Owner();
  f.Login("organizer");
  f.Nav("报名与收退款");
  r.Test("M01", "收款", "确认线下收款 → 否", "选中待付款报名，取消实际收款确认",
         "状态仍待付款，金额不变", [&] {
           f.Click("确认线下收款", {Message(QMessageBox::No)}, Card(f.window.get(), "1，程同学"));
           Check(Value(f.Reg("1"), "status") == "pending", "declined receipt changed state");
         });
  r.Test("M02", "收款", "确认线下收款 → 是", "核实收到20元后确认", "报名已确认且只记录2000分", [&] {
    f.Click("确认线下收款", {Message(QMessageBox::Yes)}, Card(f.window.get(), "1，程同学"));
    auto row = f.Reg("1");
    Check(Value(row, "status") == "registered" && Value(row, "received") == "2000",
          "payment failed");
  });
  r.Test("M03", "收款", "重复确认收款菜单", "选择已付款记录再次确认收款",
         "拒绝重复收款，金额不增加", [&] {
           f.Select("1，程同学");
           f.Menu("确认已收到线下付款",
                  {Message(QMessageBox::Yes), Message(QMessageBox::Ok, "收款确认失败")});
           Check(Value(f.Reg("1"), "received") == "2000", "double payment");
         });
  r.Test("M04", "退款", "确认线下退款 → 否", "选择原收款后取消退款确认", "退款任务仍待处理", [&] {
    f.Click("确认线下退款", {Pick(), Message(QMessageBox::No)}, Card(f.window.get(), "2，林同学"));
    Check(Value(f.Reg("2"), "refund_pending") == "2000", "declined refund persisted");
  });
  r.Test("M05", "退款", "确认线下退款 → 是", "选择原收款并确认已完成退款",
         "全额退款完成，不恢复报名", [&] {
           f.Click("确认线下退款", {Pick(), Message(QMessageBox::Yes)},
                   Card(f.window.get(), "2，林同学"));
           auto row = f.Reg("2");
           Check(Value(row, "refund_pending") == "0" && Value(row, "refunded") == "2000" &&
                     Value(row, "status") == "cancelled",
                 "refund failed");
         });
  r.Test("M06", "名单", "报名状态筛选", "选择已取消筛选项", "只显示已取消报名卡片", [&] {
    auto* filter = First<QComboBox>(f.window.get(), "registrationStatusFilter");
    filter->setCurrentIndex(filter->findData("cancelled"));
    Pump();
    Check(HasLabel(f.window.get(), "2，林同学") && !HasLabel(f.window.get(), "1，程同学"),
          "registration filter failed");
    filter->setCurrentIndex(0);
    Pump();
  });
  r.Test("M06A", "名单", "活动与场次联动筛选", "选择音乐会场次并组合状态，再切换讲座",
         "只显示对应名单，切换活动后重置场次，全部活动恢复名单", [&] {
           auto* events = First<QComboBox>(f.window.get(), "registrationEventFilter");
           auto* sessions = First<QComboBox>(f.window.get(), "registrationSessionFilter");
           auto* status = First<QComboBox>(f.window.get(), "registrationStatusFilter");
           Check(!sessions->isEnabled(), "session enabled without event");
           events->setCurrentIndex(events->findData("1"));
           Pump();
           Check(
               sessions->isEnabled() && sessions->findData("1") > 0 && sessions->findData("2") < 0,
               "sessions not scoped to event");
           sessions->setCurrentIndex(sessions->findData("1"));
           status->setCurrentIndex(status->findData("cancelled"));
           Pump();
           Check(HasLabel(f.window.get(), "2，林同学") && !HasLabel(f.window.get(), "1，程同学") &&
                     !HasLabel(f.window.get(), "3，林同学"),
                 "combined filters failed");
           events->setCurrentIndex(events->findData("2"));
           Pump();
           Check(sessions->currentIndex() == 0 && sessions->findData("1") < 0 &&
                     !HasLabel(f.window.get(), "2，林同学"),
                 "event change retained old session");
           status->setCurrentIndex(0);
           Pump();
           Check(HasLabel(f.window.get(), "3，林同学"), "lecture registration missing");
           events->setCurrentIndex(0);
           Pump();
           Check(!sessions->isEnabled() && HasLabel(f.window.get(), "1，程同学") &&
                     HasLabel(f.window.get(), "3，林同学"),
                 "all events failed");
         });
  const auto now = f.time;
  f.time -= 3600;
  const auto event = f.Publish("异常收款按钮活动", 2000);
  f.api->Login("student", "Campus2026!");
  const auto ticket =
      Value(f.api->Tickets(Value(f.api->Sessions(event).front(), "id")).front(), "id");
  const auto registration = f.api->Register(ticket, campus::RequestId());
  f.time = now;
  f.api->Tick();
  f.Owner();
  f.Nav("报名与收退款");
  const auto title = Text(registration) + "，程同学";
  r.Test("M07", "异常收款", "异常实收金额取消", "选择过期报名，取消金额输入",
         "不生成收款与退款记录", [&] {
           f.Select(title);
           f.Menu("登记超时 / 取消后的异常实收（全额待退款）", {Integer(2000, false)});
           Check(Value(f.Reg(registration), "received") == "0", "cancel recorded abnormal receipt");
         });
  r.Test("M08", "异常收款", "登记异常实收确认", "对超时报名登记实际2000分",
         "记录实收与全额待退款，报名仍取消", [&] {
           f.Select(title);
           f.Menu("登记超时 / 取消后的异常实收（全额待退款）", {Integer(2000)});
           auto row = f.Reg(registration);
           Check(Value(row, "received") == "2000" && Value(row, "refund_pending") == "2000" &&
                     Value(row, "status") == "cancelled",
                 "abnormal receipt state wrong");
         });
  r.Test("M09", "退款", "确认已完成线下全额退款菜单",
         "选中异常实收报名，通过菜单选择原收款并确认退款", "异常实收退款完成，报名不恢复", [&] {
           f.Select(title);
           f.Menu("确认已完成线下全额退款", {Pick(), Message(QMessageBox::Yes)});
           const auto row = f.Reg(registration);
           Check(Value(row, "refunded") == "2000" && Value(row, "status") == "cancelled",
                 "refund menu failed");
         });
}

void QueryFileButtons(Results& r) {
  Fixture f;
  f.Owner();
  f.Login("organizer");
  f.Nav("统计与导出");
  r.Test("Q01", "统计导出", "活动场次票种状态筛选", "选择音乐会、第一场、普通票及待付款",
         "容量数字与筛选后的导出条件匹配", [&] {
           auto boxes = Visible<QComboBox>(f.window.get());
           Check(boxes.size() == 4, "statistics filters missing");
           boxes[0]->setCurrentIndex(boxes[0]->findData("1"));
           Pump();
           boxes[1]->setCurrentIndex(1);
           Pump();
           boxes[2]->setCurrentIndex(1);
           boxes[3]->setCurrentIndex(boxes[3]->findData("pending"));
           Pump();
           Check(HasLabel(f.window.get(), "容量") && HasLabel(f.window.get(), "3"),
                 "statistics tiles missing");
         });
  const auto csv = (f.directory / "名单.csv").string();
  r.Test("Q02", "统计导出", "导出CSV取消", "打开保存文件窗口后取消", "不创建文件", [&] {
    f.Click("导出 CSV 名单", {File(csv, false)});
    Check(!std::filesystem::exists(csv), "cancel wrote file");
  });
  r.Test("Q03", "统计导出", "导出CSV保存", "选择临时文件并确认保存",
         "UTF-8名单包含目标报名，不含被筛掉人员", [&] {
           f.Click("导出 CSV 名单", {File(csv)});
           QFile file(Text(csv));
           Check(file.open(QIODevice::ReadOnly), "CSV not created");
           const auto bytes = file.readAll();
           Check(bytes.startsWith("\xEF\xBB\xBF") && bytes.contains("程同学") &&
                     !bytes.contains("林同学"),
                 "CSV filter or encoding wrong");
         });
  f.Nav("操作记录");
  r.Test("Q04", "日志", "查询日志有效组合", "输入活动1与register后查询", "仅显示匹配创建报名日志",
         [&] {
           auto fields = Visible<QLineEdit>(f.window.get());
           Check(fields.size() == 3, "log fields missing");
           fields[0]->setText("1");
           fields[1]->setText("register");
           f.Click("查询日志");
           Check(HasLabel(f.window.get(), "创建报名") && !HasLabel(f.window.get(), "收款确认"),
                 "log filter wrong");
         });
  r.Test("Q05", "日志", "查询日志空结果与SQL字符", "将活动编号改为1 OR 1=1后查询",
         "无匹配，不扩大数据范围", [&] {
           First<QLineEdit>(f.window.get())->setText("1 OR 1=1");
           f.Click("查询日志");
           Check(HasLabel(f.window.get(), "暂无记录"), "parameterized log query failed");
         });
  f.Login("admin");
  f.Menu("备份与检查");
  r.Test("Q06", "备份", "检查数据完整性", "点击检查数据完整性", "合法数据库通过检查，无错误警告",
         [&] { f.Click("检查数据完整性"); });
  const auto backup = (f.directory / "backup.db").string();
  r.Test("Q07", "备份", "备份数据库取消", "取消备份路径选择", "不创建备份文件", [&] {
    f.Click("备份数据库", {File(backup, false)});
    Check(!std::filesystem::exists(backup), "cancel wrote backup");
  });
  r.Test("Q08", "备份", "备份数据库保存与恢复读取", "保存备份后用独立连接读取检查",
         "备份可打开且账号报名日志数量一致", [&] {
           f.Click("备份数据库", {File(backup)});
           campus::Database restored(backup);
           campus::Database original(f.path);
           restored.CheckIntegrity();
           for (const auto& table : {"users", "registrations", "logs"}) {
             Check(restored.Scalar("SELECT count(*) FROM " + std::string(table)) ==
                       original.Scalar("SELECT count(*) FROM " + std::string(table)),
                   "backup table mismatch");
           }
         });
  r.Test("Q09", "备份", "完整性异常提示", "临时破坏已发布票种座位数后点击检查",
         "提示不一致，原始业务库未受影响", [&] {
           campus::Database db(f.path);
           db.Execute("UPDATE tickets SET capacity=2 WHERE id=1");
           f.Click("检查数据完整性", {Message(QMessageBox::Ok, "座位")});
           db.Execute("UPDATE tickets SET capacity=3 WHERE id=1");
         });
  r.Test("Q10", "日志", "参与者查询他人日志", "参与者日志中筛选其他操作者再查询自己",
         "他人筛选为空，本人筛选可查询", [&] {
           f.Login("student");
           f.Nav("操作记录");
           auto fields = Visible<QLineEdit>(f.window.get());
           fields[0]->setText("1");
           fields[2]->setText("4");
           f.Click("查询日志");
           Check(HasLabel(f.window.get(), "暂无记录"), "participant queried another user's log");
           fields[2]->setText("3");
           f.Click("查询日志");
           Check(HasLabel(f.window.get(), "创建报名"), "participant cannot query own log");
         });
}

void CheckInButtons(Results& r) {
  Fixture f;
  const auto now = f.time;
  f.time -= 3600;
  const auto event = f.Publish("签到按钮活动", 0, 2, 1800, 3540, 7200);
  f.Admin();
  const auto session = Value(f.api->Sessions(event).front(), "id");
  f.api->Assign(session, "5", false, campus::RequestId());
  f.api->Login("student", "Campus2026!");
  const auto ticket = Value(f.api->Tickets(session).front(), "id");
  const auto registration = f.api->Register(ticket, campus::RequestId());
  f.time = now;
  f.api->Tick();
  f.Login("checker");
  r.Test("C01", "检票", "核验并签到卡片按钮", "对已开始场次的已报名人员点击签到",
         "状态已签到且记录首次时间", [&] {
           f.Click("核验并签到", {}, Card(f.window.get(), Text(registration) + "，程同学"));
           auto row = f.Reg(registration);
           Check(Value(row, "status") == "checked" && !Value(row, "checked_at").empty(),
                 "checkin failed");
         });
  const auto first_time = Value(f.Reg(registration), "checked_at");
  r.Test("C02", "检票", "重复签到菜单", "选中已签到人员并再次触发签到",
         "返回失败，不增加首次签到或日志", [&] {
           f.Select(Text(registration) + "，程同学");
           f.Menu("核验并签到", {Message(QMessageBox::Ok, "请求失败")});
           Check(Value(f.Reg(registration), "checked_at") == first_time,
                 "duplicate checkin changed time");
         });
  r.Test("C03", "检票", "未分配场次名单隔离", "检查免费讲座的原报名编号3",
         "检票员页面不显示未分配场次记录",
         [&] { Check(!HasLabel(f.window.get(), "3，林同学"), "unassigned session leaked"); });
}
}  // namespace

int main(int argc, char* argv[]) {
  QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
  QApplication app(argc, argv);
  Results results;
  const std::vector<std::pair<const char*, std::function<void(Results&)>>> groups{
      {"login_navigation", LoginNavigation},
      {"participant", ParticipantButtons},
      {"draft", DraftButtons},
      {"legacy_draft", LegacyDraftButtons},
      {"review", ReviewButtons},
      {"accounts_assignments", AccountAssignmentButtons},
      {"money", MoneyButtons},
      {"queries_files", QueryFileButtons},
      {"checkin", CheckInButtons}};
  for (const auto& group : groups) {
    try {
      group.second(results);
    } catch (const std::exception& error) {
      results.Test(QString("SETUP-") + group.first, "测试环境", "测试组前置数据",
                   "准备独立数据与角色", "前置准备完成",
                   [&] { throw std::runtime_error(error.what()); });
    }
  }
  const QString output = argc > 1 ? argv[1] : "button-results.json";
  QFile file(output);
  if (!file.open(QIODevice::WriteOnly)) {
    std::cerr << "Cannot save results\n";
    return 2;
  }
  file.write(QJsonDocument(QJsonObject{{"cases", results.rows},
                                       {"failed", results.failed},
                                       {"assertions", assertions},
                                       {"platform", QApplication::platformName()},
                                       {"qt_version", QT_VERSION_STR}})
                 .toJson());
  std::cout << results.rows.size() << " cases, " << results.failed << " failures, " << assertions
            << " assertions\n";
  return results.failed == 0 ? 0 : 1;
}
