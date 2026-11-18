#include "campus/window.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QInputMethodEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace campus {
namespace {
// User cancellation stops the current UI flow without an error warning.
class UserCancelled : public std::exception {
 public:
  const char* what() const noexcept override { return "User cancelled"; }
};

// A card can be selected for the additional actions in the header menu.
class RecordCard : public QFrame {
 public:
  explicit RecordCard(std::function<void()> select) : select_(std::move(select)) {}

 protected:
  void mousePressEvent(QMouseEvent* event) override {
    select_();
    QFrame::mousePressEvent(event);
  }

 private:
  std::function<void()> select_;
};

QString Text(const std::string& value) { return QString::fromStdString(value); }
std::string String(const QString& value) { return value.trimmed().toStdString(); }

QLineEdit* Field(QFormLayout* form, const QString& label, const QString& value = "") {
  form->setRowWrapPolicy(QFormLayout::WrapAllRows);
  auto* input = new QLineEdit(value);
  input->setAttribute(Qt::WA_InputMethodEnabled, true);
  input->setInputMethodHints(Qt::ImhNone);
  form->addRow(label, input);
  return input;
}

bool Accept(QDialog* dialog, QFormLayout* form) {
  dialog->setFixedWidth(361);
  form->setRowWrapPolicy(QFormLayout::WrapAllRows);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  form->addRow(buttons);
  QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
  return dialog->exec() == QDialog::Accepted;
}

struct TicketEditor : public QFrame {
  std::string id;
  QLineEdit* name = nullptr;
  QDoubleSpinBox* price = nullptr;
  QSpinBox* capacity = nullptr;
  QTextEdit* seats = nullptr;
  bool removed = false;

  TicketEditor(const DraftTicket& input, int ordinal, bool published = false) : id(input.id) {
    setObjectName("card");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* layout = new QVBoxLayout(this);
    layout->setSizeConstraint(QLayout::SetMinimumSize);
    layout->setContentsMargins(12, 14, 12, 14);
    auto* heading = new QLabel(QString("票种 %1").arg(ordinal));
    heading->setObjectName("cardTitle");
    layout->addWidget(heading);
    auto* form = new QFormLayout;
    name = Field(form, "票种名称", Text(input.name));
    name->setObjectName("draftTicketName");
    price = new QDoubleSpinBox;
    price->setRange(0, 100000);
    price->setDecimals(2);
    price->setSuffix(" 元");
    price->setValue(input.price_cents / 100.0);
    form->addRow("价格（免费填 0）", price);
    capacity = new QSpinBox;
    capacity->setObjectName("draftTicketCapacity");
    capacity->setRange(1, 100000);
    capacity->setValue(input.capacity);
    form->addRow("票种容量", capacity);
    auto* prefix = Field(form, "自动排号前缀", QString("T%1-").arg(ordinal));
    auto* generate = new QPushButton("按容量生成座位编号");
    generate->setProperty("secondary", true);
    form->addRow(generate);
    seats = new QTextEdit;
    seats->setObjectName("draftTicketSeats");
    seats->setAcceptRichText(false);
    seats->setFixedHeight(90);
    seats->setPlaceholderText("例如 A01,A02,A03；场次内座位编号不能重复");
    QStringList labels;
    for (const auto& label : input.seats) {
      labels << Text(label);
    }
    seats->setPlainText(labels.join(','));
    form->addRow("座位编号（逗号或换行分隔）", seats);
    layout->addLayout(form);
    connect(generate, &QPushButton::clicked, this, [this, prefix] {
      QStringList labels;
      for (int i = 1; i <= capacity->value(); ++i) {
        labels << prefix->text().trimmed() + QString::number(i).rightJustified(3, '0');
      }
      seats->setPlainText(labels.join(','));
    });
    auto* remove = new QPushButton("移除此票种");
    remove->setProperty("secondary", true);
    remove->setVisible(!published || id.empty());
    layout->addWidget(remove);
    if (published && !id.empty()) {
      price->setEnabled(!input.price_locked);
      if (input.price_locked) {
        auto* locked = new QLabel("已有报名，原票价不可修改；新价格请添加票种。");
        locked->setWordWrap(true);
        locked->setObjectName("muted");
        layout->addWidget(locked);
      }
      if (input.seats_locked) {
        capacity->setEnabled(false);
        seats->setReadOnly(true);
        prefix->setEnabled(false);
        generate->setEnabled(false);
      }
      if (input.cancelled) {
        setEnabled(false);
      }
    }
    connect(remove, &QPushButton::clicked, this, [this] {
      removed = true;
      hide();
    });
  }

  DraftTicket Read() const {
    DraftTicket output;
    output.id = id;
    output.name = String(name->text());
    output.price_cents = qRound(price->value() * 100);
    output.capacity = capacity->value();
    auto text = seats->toPlainText();
    text.replace(QString("，"), ",").replace('\n', ',');
    for (const auto& seat : text.split(',', Qt::SkipEmptyParts)) {
      if (!seat.trimmed().isEmpty()) {
        output.seats.push_back(String(seat));
      }
    }
    return output;
  }
};

struct SessionEditor : public QFrame {
  std::string id;
  QLineEdit* name = nullptr;
  QLineEdit* location = nullptr;
  QDateTimeEdit* deadline = nullptr;
  QDateTimeEdit* start = nullptr;
  QDateTimeEdit* end = nullptr;
  QSpinBox* capacity = nullptr;
  QVBoxLayout* ticket_layout = nullptr;
  std::vector<TicketEditor*> tickets;
  bool removed = false;
  bool published = false;
  bool tickets_locked = false;

  SessionEditor(const DraftSession& input, int ordinal, bool published_edit = false)
      : id(input.id), published(published_edit) {
    setObjectName("card");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* layout = new QVBoxLayout(this);
    layout->setSizeConstraint(QLayout::SetMinimumSize);
    layout->setContentsMargins(12, 16, 12, 16);
    layout->setSpacing(12);
    auto* heading = new QLabel(QString("场次 %1 · 报名设置").arg(ordinal));
    heading->setObjectName("cardTitle");
    layout->addWidget(heading);
    auto* form = new QFormLayout;
    name = Field(form, "场次名称", Text(input.details.name));
    name->setObjectName("draftSessionName");
    location = Field(form, "活动地点", Text(input.details.location));
    location->setObjectName("draftSessionLocation");
    auto date = [&](const QString& label, std::int64_t time, const char* object) {
      auto* field = new QDateTimeEdit(QDateTime::fromSecsSinceEpoch(time));
      field->setObjectName(object);
      field->setDisplayFormat("yyyy-MM-dd HH:mm");
      field->setCalendarPopup(true);
      form->addRow(label, field);
      return field;
    };
    deadline = date("报名截止时间", input.details.deadline, "draftSessionDeadline");
    start = date("场次开始时间", input.details.start, "draftSessionStart");
    end = date("场次结束时间", input.details.end, "draftSessionEnd");
    capacity = new QSpinBox;
    capacity->setObjectName("draftSessionCapacity");
    capacity->setRange(1, 100000);
    capacity->setValue(input.details.capacity);
    form->addRow("场次总容量", capacity);
    layout->addLayout(form);
    ticket_layout = new QVBoxLayout;
    layout->addLayout(ticket_layout);
    tickets_locked =
        published && !id.empty() && (input.cancelled || Now() >= input.details.deadline);
    for (const auto& ticket : input.tickets) {
      AddTicket(ticket);
    }
    auto* add = new QPushButton("添加票种 ＋");
    add->setObjectName("addDraftTicketButton");
    add->setProperty("secondary", true);
    layout->addWidget(add);
    connect(add, &QPushButton::clicked, this, [this] {
      DraftTicket ticket;
      ticket.name = "普通票";
      ticket.capacity = 5;
      const auto prefix = "T" + std::to_string(tickets.size() + 1) + "-";
      for (int i = 1; i <= 5; ++i) {
        ticket.seats.push_back(prefix + std::to_string(i));
      }
      AddTicket(ticket);
    });
    auto* remove = new QPushButton("移除此场次");
    remove->setProperty("secondary", true);
    remove->setVisible(!published || id.empty());
    if (published && !id.empty()) {
      if (input.deadline_locked) {
        deadline->setReadOnly(true);
      }
      if (input.cancelled || Now() >= input.details.start) {
        for (auto* field : {static_cast<QWidget*>(name), static_cast<QWidget*>(location),
                            static_cast<QWidget*>(deadline), static_cast<QWidget*>(start),
                            static_cast<QWidget*>(end), static_cast<QWidget*>(capacity)}) {
          field->setEnabled(false);
        }
      }
      add->setEnabled(!tickets_locked);
      auto* note = new QLabel(
          "原有场次、票种不能直接删除，取消需单独提交取消审核。锁定的票价、座位或截止时间不能修改"
          "。");
      note->setWordWrap(true);
      note->setObjectName("muted");
      layout->addWidget(note);
    }
    layout->addWidget(remove);
    connect(remove, &QPushButton::clicked, this, [this] {
      removed = true;
      hide();
    });
  }

  void AddTicket(const DraftTicket& input) {
    auto* editor = new TicketEditor(input, static_cast<int>(tickets.size() + 1), published);
    if (tickets_locked) {
      editor->setEnabled(false);
    }
    tickets.push_back(editor);
    ticket_layout->addWidget(editor);
  }

  DraftSession Read() const {
    DraftSession output;
    output.id = id;
    output.details = {String(name->text()),
                      String(location->text()),
                      deadline->dateTime().toSecsSinceEpoch(),
                      start->dateTime().toSecsSinceEpoch(),
                      end->dateTime().toSecsSinceEpoch(),
                      capacity->value()};
    for (const auto* ticket : tickets) {
      if (!ticket->removed) {
        output.tickets.push_back(ticket->Read());
      }
    }
    return output;
  }
};

DraftSession DefaultDraftSession(int ordinal) {
  DraftSession session;
  const auto now = Now();
  session.details = {
      "第" + std::to_string(ordinal) + "场", "", now + 86400, now + 90000, now + 93600, 5};
  DraftTicket ticket;
  ticket.name = "普通票";
  ticket.capacity = 5;
  for (int i = 1; i <= 5; ++i) {
    ticket.seats.push_back("A0" + std::to_string(i));
  }
  session.tickets.push_back(ticket);
  return session;
}

QString Format(const Row& row, const std::string& key) {
  const auto value = Value(row, key);
  if (key == "status") {
    return Text(StatusText(value));
  }
  if (key == "role") {
    return Text(RoleText(value));
  }
  if (key == "enabled") {
    return value == "1" ? "启用" : "停用";
  }
  if (key == "deadline" || key == "start" || key == "end" || key == "payment_due" ||
      key == "time" || key == "checked_at") {
    return value.empty() ? "—" : Text(TimeText(std::stoll(value)));
  }
  if (key == "price" || key == "received" || key == "refund_pending" || key == "refunded" ||
      key == "amount") {
    return value.empty() ? "—" : QString("¥%1").arg(std::stoll(value) / 100.0, 0, 'f', 2);
  }
  return Text(value);
}
}  // namespace

Window::Window(const std::string& database) : service_(database) {
  setWindowTitle("校园活动票务系统");
  setFixedSize(393, 852);
  statusBar()->hide();
  setStyleSheet(R"(
    QWidget { font-family: 'Noto Sans CJK SC','Microsoft YaHei',sans-serif;
      font-size: 14px; color: #202D2B; background: #F3F5F2; }
    QWidget#header,QWidget#footer { background: white; }
    QLabel { background: transparent; }
    QLabel#brand { font-size: 22px; font-weight: 700; }
    QLabel#title { font-size: 23px; font-weight: 700; padding: 4px 0; }
    QLabel#cardTitle { font-size: 18px; font-weight: 700; }
    QLabel#muted { color: #607269; font-size: 12px; }
    QLabel#banner { background: #E4F0E8; color: #24634E; border-radius: 6px;
      font-size: 23px; padding: 14px; }
    QFrame#card { background: white; border: 1px solid #DCE3DD; border-radius: 20px; }
    QFrame#card QLabel,QFrame#card QRadioButton { background: transparent; border: none; }
    QFrame#card QLabel#banner { background: #E4F0E8; color: #24634E; border-radius: 6px; }
    QComboBox::drop-down { border: none; width: 18px; }
    QToolButton::menu-indicator { image: none; }
    QPushButton,QToolButton { background: #24634E; color: white; border: 0;
      border-radius: 5px; padding: 12px; font-weight: 600; }
    QPushButton:hover,QToolButton:hover { background: #347C62; }
    QPushButton[secondary="true"] { color: #24634E; background: #E4F0E8; }
    QPushButton#tab { background: white; color: #607269; font-size: 12px;
      font-weight: 400; padding: 14px 3px; }
    QPushButton#tab:checked { background: #E4F0E8; color: #24634E; font-weight: 700; }
    QToolButton#more { background: transparent; color: #607269; padding: 6px; }
    QLineEdit,QComboBox,QSpinBox,QDateTimeEdit,QTextEdit { background: white;
      border: 1px solid #DCE3DD; border-radius: 5px; padding: 10px; min-height: 24px; }
    QComboBox { padding-right: 20px; }
    QScrollArea { border: none; }
    QScrollBar:vertical { width: 5px; background: transparent; }
    QScrollBar::handle:vertical { background: #C7D7CD; border-radius: 2px; min-height: 25px; }
    QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical { height: 0; }
    QMenu { background: white; border: 1px solid #DCE3DD; }
    QMenu::item { padding: 10px 14px; }
    QMenu::item:selected { background: #E4F0E8; }
    QDialog { background: #F3F5F2; }
  )");
  LoginPage();
  auto* timer = new QTimer(this);
  connect(timer, &QTimer::timeout, this, [this] {
    try {
      service_.Tick();
    } catch (const std::exception& error) {
      statusBar()->showMessage("后台任务失败：" + Text(error.what()), 15000);
    }
  });
  timer->start(1000);
}

void Window::LoginPage() {
  user_.clear();
  registration_event_.clear();
  registration_session_.clear();
  registration_status_.clear();
  tabs_.clear();
  auto* container = new QWidget;
  auto* root = new QVBoxLayout(container);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);
  auto* header = new QWidget;
  header->setObjectName("header");
  header->setFixedHeight(57);
  auto* top = new QHBoxLayout(header);
  top->setContentsMargins(20, 0, 20, 0);
  auto* brand = new QLabel("校园活动");
  brand->setObjectName("brand");
  top->addWidget(brand);
  root->addWidget(header);
  auto* content = new QWidget;
  auto* layout = new QVBoxLayout(content);
  layout->setContentsMargins(16, 22, 16, 16);
  layout->setSpacing(10);
  auto* title = new QLabel("发现活动，连接校园");
  title->setObjectName("title");
  title->setStyleSheet("color:#24634E;");
  layout->addWidget(title);
  layout->addSpacing(8);
  layout->addWidget(new QLabel("账号"));
  auto* username = new QLineEdit;
  username->setPlaceholderText("请输入账号");
  username->setObjectName("loginUsername");
  layout->addWidget(username);
  layout->addSpacing(12);
  layout->addWidget(new QLabel("密码"));
  auto* password = new QLineEdit;
  password->setPlaceholderText("请输入密码");
  password->setObjectName("loginPassword");
  password->setEchoMode(QLineEdit::Password);
  layout->addWidget(password);
  layout->addSpacing(12);
  layout->addWidget(new QLabel("选择身份"));
  auto* identity = new QComboBox;
  identity->addItem("根据登录账号自动识别");
  layout->addWidget(identity);
  layout->addSpacing(14);
  auto* login = new QPushButton("登录");
  login->setObjectName("loginButton");
  login->setMinimumHeight(49);
  layout->addWidget(login);
  auto* initialize = new QPushButton("首次使用 · 初始化管理员", content);
  initialize->setProperty("secondary", true);
  initialize->hide();
  top->addStretch();
  auto* setup = new QToolButton;
  setup->setObjectName("more");
  setup->setText("⋯");
  auto* setup_menu = new QMenu(setup);
  setup_menu->addAction("首次使用 · 初始化管理员", initialize, &QPushButton::click);
  setup->setMenu(setup_menu);
  setup->setPopupMode(QToolButton::InstantPopup);
  top->addWidget(setup);
  connect(initialize, &QPushButton::clicked, this, [this, username, password] {
    Run([&] { service_.Bootstrap(String(username->text()), password->text().toStdString()); },
        false);
  });
  connect(login, &QPushButton::clicked, this, [this, username, password] {
    Run(
        [&] {
          user_ = service_.Login(String(username->text()), password->text().toStdString());
          Shell();
        },
        false);
  });
  connect(password, &QLineEdit::returnPressed, login, &QPushButton::click);
  layout->addStretch();
  root->addWidget(content, 1);
  auto* footer = new QWidget;
  footer->setObjectName("footer");
  footer->setFixedHeight(83);
  auto* navigation = new QHBoxLayout(footer);
  for (const auto& name : {"发现活动", "我的报名", "操作记录"}) {
    auto* tab = new QPushButton(name);
    tab->setObjectName("tab");
    navigation->addWidget(tab, 1);
    connect(tab, &QPushButton::clicked, this, [username] { username->setFocus(); });
  }
  root->addWidget(footer);
  setCentralWidget(container);
}

void Window::Shell() {
  tabs_.clear();
  auto* container = new QWidget;
  root_layout_ = new QVBoxLayout(container);
  root_layout_->setContentsMargins(0, 0, 0, 0);
  root_layout_->setSpacing(0);
  auto* header = new QWidget;
  header->setObjectName("header");
  header->setFixedHeight(57);
  auto* top = new QHBoxLayout(header);
  top->setContentsMargins(20, 0, 14, 0);
  auto* brand = new QLabel("校园活动");
  brand->setObjectName("brand");
  top->addWidget(brand);
  top->addStretch();
  auto* role_label = new QLabel(Text(RoleText(Value(user_, "role"))));
  role_label->setObjectName("muted");
  top->addWidget(role_label);
  auto* more = new QToolButton;
  more->setObjectName("more");
  more->setText("⋯");
  more_menu_ = new QMenu(more);
  more->setMenu(more_menu_);
  more->setPopupMode(QToolButton::InstantPopup);
  top->addWidget(more);
  root_layout_->addWidget(header);
  auto* panel = new QWidget;
  body_ = new QVBoxLayout(panel);
  body_->setContentsMargins(16, 16, 16, 18);
  body_->setSpacing(14);
  body_->setAlignment(Qt::AlignTop);
  body_->setSizeConstraint(QLayout::SetMinimumSize);
  auto* scroll = new QScrollArea;
  scroll->setWidgetResizable(true);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scroll->setWidget(panel);
  root_layout_->addWidget(scroll, 1);
  const auto role = Value(user_, "role");
  QStringList pages;
  if (role == "participant") {
    pages = QStringList{"发现活动", "我的报名", "操作记录"};
  }
  if (role == "organizer") {
    pages = QStringList{"我的活动", "报名与收退款", "统计与导出", "操作记录"};
  }
  if (role == "admin") {
    pages = QStringList{"活动审核", "账号管理", "检票分配", "操作记录"};
  }
  if (role == "checker") {
    pages = QStringList{"分配场次与检票"};
  }
  auto* footer = new QWidget;
  footer->setObjectName("footer");
  footer->setFixedHeight(83);
  auto* navigation = new QHBoxLayout(footer);
  navigation->setContentsMargins(12, 8, 12, 24);
  navigation->setSpacing(4);
  for (const auto& page : pages) {
    QString label = page;
    if (page == "报名与收退款") {
      label = "报名管理";
    }
    if (page == "统计与导出") {
      label = "统计导出";
    }
    auto* tab = new QPushButton(label);
    tab->setObjectName("tab");
    tab->setProperty("page", page);
    tab->setCheckable(true);
    tabs_.push_back(tab);
    navigation->addWidget(tab, 1);
    connect(tab, &QPushButton::clicked, this,
            [this, page] { Run([&] { ShowPage(page); }, false); });
  }
  root_layout_->addWidget(footer);
  setCentralWidget(container);
  ShowPage(pages.front());
}

void Window::ShowPage(const QString& page) {
  page_ = page;
  while (auto* item = body_->takeAt(0)) {
    if (item->widget()) {
      item->widget()->hide();
      item->widget()->deleteLater();
    }
    delete item;
  }
  records_ = nullptr;
  selected_row_ = -1;
  actions_by_name_.clear();
  more_menu_->clear();
  const auto role = Value(user_, "role");
  if (role == "participant") {
    more_menu_->addAction("消息通知", this, [this] { Run([&] { ShowPage("消息通知"); }, false); });
  }
  if (role == "organizer") {
    more_menu_->addAction("我的申请", this, [this] { Run([&] { ShowPage("我的申请"); }, false); });
  }
  if (role == "admin") {
    more_menu_->addAction("备份与检查", this,
                          [this] { Run([&] { ShowPage("备份与检查"); }, false); });
  }
  more_menu_->addAction("退出登录", this, [this] {
    service_.Logout();
    LoginPage();
  });
  more_menu_->addSeparator();
  for (auto* tab : tabs_) {
    auto target = page;
    if (page == "编辑账号") {
      target = "账号管理";
    }
    if (page == "新建活动" || page == "编辑草稿" || page == "修改已发布活动") {
      target = "我的活动";
    }
    if (page == "活动详情" || page == "选择票种") {
      target = "发现活动";
    }
    tab->setChecked(tab->property("page").toString() == target);
  }
  auto* title = new QLabel(page);
  title->setObjectName("title");
  if (page != "发现活动" && page != "操作记录") {
    body_->addWidget(title);
  } else {
    title->deleteLater();
  }
  if (page == "新建活动" || page == "编辑草稿" || page == "修改已发布活动") {
    CreateEventPage();
  } else if (page == "活动详情") {
    DetailsPage();
  } else if (page == "选择票种") {
    TicketPage();
  } else if (page == "发现活动" || page == "我的活动") {
    EventsPage();
  } else if (page == "我的报名" || page == "报名与收退款" || page == "分配场次与检票") {
    RegistrationsPage();
  } else if (page == "活动审核" || page == "我的申请") {
    ReviewsPage();
  } else if (page == "编辑账号") {
    EditUserPage();
  } else if (page == "账号管理") {
    UsersPage();
  } else if (page == "检票分配") {
    AssignmentPage();
  } else if (page == "统计与导出") {
    StatisticsPage();
  } else if (page == "操作记录") {
    LogsPage();
  } else if (page == "消息通知") {
    NotificationsPage();
  } else {
    Button("备份数据库", [this] {
      const auto path = QFileDialog::getSaveFileName(this, "备份", "campus-backup.db");
      if (!path.isEmpty()) {
        service_.Backup(path.toStdString());
      }
    });
    Button("检查数据完整性", [this] { service_.CheckIntegrity(); });
  }
}

void Window::Run(const std::function<void()>& action, bool refresh) {
  try {
    const auto previous_page = page_;
    action();
    if (refresh && !user_.empty() && page_ == previous_page) {
      ShowPage(page_);
    }
  } catch (const UserCancelled&) {
    return;
  } catch (const std::exception& error) {
    QMessageBox::warning(this, "操作未完成", Text(error.what()));
  }
}

void Window::Button(const QString& text, const std::function<void()>& action) {
  actions_by_name_[text] = action;
  if (text == "新建活动" || text == "创建新账号" || text == "导出当前活动 CSV 名单" ||
      page_ == "备份与检查") {
    auto* button = new QPushButton(text == "新建活动" ? "新建活动 ＋" : text);
    if (text == "新建活动") {
      button->setObjectName("createEventButton");
      body_->insertWidget(1, button);
    } else {
      body_->addWidget(button);
    }
    connect(button, &QPushButton::clicked, this,
            [this, action, text] { Run(action, text != "新建活动"); });
  } else {
    more_menu_->addAction(text, this, [this, action, text] { Run(action, text != "新建活动"); });
  }
}

void Window::CardAction(QVBoxLayout* layout, const QString& label, int row, const QString& action,
                        bool secondary) {
  auto* button = new QPushButton(label);
  if (action == "编辑账号") {
    button->setObjectName("editUserButton_" + Text(Value(displayed_[row], "id")));
  }
  button->setMinimumHeight(46);
  button->setProperty("secondary", secondary);
  layout->addWidget(button);
  connect(button, &QPushButton::clicked, this, [this, row, action] {
    selected_row_ = row;
    const auto found = actions_by_name_.find(action);
    if (found != actions_by_name_.end()) {
      Run(found->second);
    }
  });
}

void Window::SetRecords(const Rows& rows,
                        const std::vector<std::pair<std::string, QString>>& columns) {
  displayed_ = rows;
  selected_row_ = rows.empty() ? -1 : 0;
  if (records_) {
    body_->removeWidget(records_);
    records_->hide();
    records_->deleteLater();
  }
  records_ = new QWidget;
  records_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
  auto* list = new QVBoxLayout(records_);
  list->setContentsMargins(0, 0, 0, 0);
  list->setSpacing(14);
  list->setSizeConstraint(QLayout::SetMinimumSize);
  body_->addWidget(records_);
  if (rows.empty()) {
    auto* empty = new QLabel("暂无记录");
    empty->setObjectName("muted");
    empty->setAlignment(Qt::AlignCenter);
    list->addWidget(empty);
    return;
  }
  Rows pending_reviews;
  if (page_ == "我的活动") {
    for (const auto& review : service_.Reviews()) {
      if (Value(review, "status") == "pending") {
        pending_reviews.push_back(review);
      }
    }
  }
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const auto row = rows[i];
    auto* card = new RecordCard([this, i] { selected_row_ = static_cast<int>(i); });
    card->setObjectName("card");
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(12, 12, 12, 10);
    layout->setSpacing(8);
    auto label = [&](const QString& text, const char* style = "muted") {
      auto* widget = new QLabel(text);
      widget->setWordWrap(true);
      widget->setTextFormat(Qt::PlainText);
      widget->setObjectName(style);
      widget->setMinimumWidth(0);
      layout->addWidget(widget);
      return widget;
    };
    if (page_ == "发现活动" || page_ == "我的活动") {
      if (page_ == "发现活动") {
        auto* banner = label(Text(Value(row, "category")), "banner");
        banner->setFixedHeight(76);
      }
      label(Text(Value(row, "name")), "cardTitle");
      if (page_ == "发现活动") {
        const auto sessions = service_.Sessions(Value(row, "id"));
        std::int64_t minimum = -1;
        if (!sessions.empty()) {
          const auto& session = sessions.front();
          label(Text(TimeText(Number(session, "start")) + "，" + Value(session, "location")));
          for (const auto& ticket : service_.Tickets(Value(session, "id"))) {
            if (minimum < 0 || Number(ticket, "price") < minimum) {
              minimum = Number(ticket, "price");
            }
          }
        }
        auto* footer = new QWidget;
        footer->setStyleSheet("background:transparent;");
        auto* footer_layout = new QHBoxLayout(footer);
        footer_layout->setContentsMargins(0, 0, 0, 0);
        auto* price =
            new QLabel(minimum <= 0 ? "免费" : QString("¥%1 起").arg(minimum / 100.0, 0, 'f', 2));
        price->setStyleSheet("color:#24634E;");
        footer_layout->addWidget(price, 1);
        auto* details = new QPushButton("查看活动  ›");
        details->setProperty("secondary", true);
        details->setStyleSheet("padding:7px 20px; font-weight:400;");
        footer_layout->addWidget(details);
        layout->addWidget(footer);
        connect(details, &QPushButton::clicked, this,
                [this, row] { Run([&] { EventDetails(row); }, false); });
      } else {
        label(Format(row, "status"));
        if (Value(row, "status") == "draft") {
          const auto sessions = service_.DraftSessions(Value(row, "id"));
          std::size_t tickets = 0;
          for (const auto& session : sessions) {
            tickets += session.tickets.size();
          }
          label(QString("已配置 %1 个场次、%2 个票种").arg(sessions.size()).arg(tickets));
        }
        Row pending;
        for (const auto& review : pending_reviews) {
          if (Value(review, "event_id") == Value(row, "id")) {
            pending = review;
          }
        }
        if (!pending.empty()) {
          const Row kinds{{"publish", "发布"}, {"edit", "修改"}, {"cancel", "取消"}};
          label(Text(Value(kinds, Value(pending, "kind"))) + "申请待审核，可主动撤回");
        }
        auto* edit = new QPushButton(pending.empty() ? "编辑活动" : "查看待审核申请");
        edit->setObjectName("editEventButton_" + Text(Value(row, "id")));
        edit->setProperty("secondary", true);
        layout->addWidget(edit);
        connect(edit, &QPushButton::clicked, this, [this, row, pending, i] {
          selected_row_ = static_cast<int>(i);
          Run([&] {
            if (pending.empty()) {
              EditEventDialog(row);
            } else {
              ShowPage("我的申请");
            }
          });
        });
        if (!pending.empty()) {
          auto* withdraw = new QPushButton("撤回待审核申请");
          withdraw->setObjectName("withdrawReviewButton_" + Text(Value(pending, "id")));
          layout->addWidget(withdraw);
          connect(withdraw, &QPushButton::clicked, this, [this, pending] {
            Run([&] { service_.Withdraw(Value(pending, "id"), RequestId()); });
          });
        }
        if (pending.empty() && Value(row, "status") == "draft") {
          CardAction(layout, "提交发布审核", static_cast<int>(i), "提交发布审核");
        }
        if (pending.empty()) {
          CardAction(layout, Value(row, "status") == "draft" ? "删除草稿" : "申请取消",
                     static_cast<int>(i),
                     Value(row, "status") == "draft" ? "删除草稿" : "申请取消活动 / 场次 / 票种",
                     true);
        }
      }
    } else if (page_ == "我的报名" || page_ == "报名与收退款" || page_ == "分配场次与检票") {
      const auto status = Value(row, "status");
      label(page_ == "我的报名" ? Text(Value(row, "event_name"))
                                : Text(Value(row, "id") + "，" + Value(row, "participant")),
            "cardTitle");
      label(status == "pending" ? "待付款" : Text(StatusText(status)));
      label("时间   " + Format(row, "start"));
      label("场次 / 票种   " + Text(Value(row, "session_name") + "，" + Value(row, "ticket_name")));
      if (!Value(row, "seat").empty()) {
        label("座位   " + Text(Value(row, "seat")));
      }
      if (page_ == "我的报名") {
        if (status == "pending") {
          label("付款截止   " + Format(row, "payment_due"));
        }
        label("报名编号   " + Text(Value(row, "id")));
        if (status == "pending" || status == "waiting" || status == "registered") {
          CardAction(layout, "取消报名", static_cast<int>(i), "取消本人报名", true);
        }
      } else if (page_ == "报名与收退款") {
        label("实收 " + Format(row, "received") + " · 待退款 " + Format(row, "refund_pending"));
        if (status == "pending") {
          CardAction(layout, "确认线下收款", static_cast<int>(i), "确认已收到线下付款");
        }
        if (Number(row, "refund_pending") > 0) {
          CardAction(layout, "确认线下退款", static_cast<int>(i), "确认已完成线下全额退款");
        }
      } else if (status == "registered") {
        CardAction(layout, "核验并签到", static_cast<int>(i), "核验并签到");
      }
    } else if (page_ == "账号管理") {
      label(Text(Value(row, "username")), "cardTitle");
      label(Text(Value(row, "name")));
      label(Format(row, "role") + "，" + Format(row, "enabled"));
      CardAction(layout, "编辑 ›", static_cast<int>(i), "编辑账号", true);
    } else if (page_ == "活动审核" || page_ == "我的申请") {
      label(Text(Value(row, "event_name")), "cardTitle");
      const Row kinds{{"publish", "首次发布"}, {"edit", "修改"}, {"cancel", "取消"}};
      auto* badge = label("申请类型：" + Text(Value(kinds, Value(row, "kind"))), "banner");
      badge->setStyleSheet("font-size:16px;");
      label("状态：" +
            (Value(row, "status") == "pending" ? QString("待审核") : Format(row, "status")));
      if (!Value(row, "proposed_name").empty()) {
        label("申请版本：" + Text(Value(row, "proposed_name")));
      }
      if (!Value(row, "reason").empty()) {
        label("审核意见：" + Text(Value(row, "reason")));
      }
      CardAction(layout, "查看原版本与申请内容 ›", static_cast<int>(i), "查看原内容与候选内容",
                 true);
      if (Value(row, "status") == "pending") {
        if (page_ == "活动审核") {
          CardAction(layout, "拒绝申请", static_cast<int>(i), "拒绝申请（必填原因）", true);
          CardAction(layout, "通过审核", static_cast<int>(i), "通过审核");
        } else {
          CardAction(layout, "撤回申请", static_cast<int>(i), "撤回选中申请", true);
        }
      }
    } else if (page_ == "操作记录") {
      const Row names{{"register", "创建报名"},
                      {"receive", "收款确认"},
                      {"cancel_registration", "取消报名"},
                      {"promote", "候补递补"},
                      {"complete_refund", "退款确认"},
                      {"check_in", "签到"},
                      {"create_event", "创建活动"},
                      {"decide_review", "活动审核"},
                      {"allocate_seat", "座位分配"},
                      {"mark_absent", "缺席处理"}};
      const auto action = Value(row, "action");
      label(Text(Value(names, action).empty() ? action : Value(names, action)), "cardTitle");
      label(Format(row, "time"));
      label("活动编号：" + Text(Value(row, "event_id")) + " · 对象编号：" +
            Text(Value(row, "object_id")));
      label("结果：" + Text(StatusText(Value(row, "after_state"))));
    } else {
      for (std::size_t j = 0; j < columns.size(); ++j) {
        const auto& column = columns[j];
        if (Value(row, column.first).empty()) {
          continue;
        }
        if (column.first == "request_id") {
          continue;
        }
        label((j == 0 ? "" : column.second + "：") + Format(row, column.first),
              j == 0 ? "cardTitle" : "muted");
      }
    }
    list->addWidget(card);
  }
}

Row Window::Selected() {
  if (selected_row_ < 0 || static_cast<std::size_t>(selected_row_) >= displayed_.size()) {
    throw Error("请先选择一条记录");
  }
  return displayed_[selected_row_];
}

QString Window::Ask(const QString& title, const QString& label, const QString& initial) {
  bool accepted = false;
  const auto text =
      QInputDialog::getText(this, title, label, QLineEdit::Normal, initial, &accepted);
  if (!accepted) {
    throw UserCancelled();
  }
  return text;
}

QString Window::Choose(const QString& title, const Rows& rows, const std::string& label) {
  if (rows.empty()) {
    throw Error("没有可选记录");
  }
  QStringList items;
  for (const auto& row : rows) {
    items << Text(Value(row, "id") + " · " + Value(row, label));
  }
  bool accepted = false;
  const auto chosen = QInputDialog::getItem(this, title, "请选择", items, 0, false, &accepted);
  if (!accepted) {
    throw UserCancelled();
  }
  return Text(Value(rows[items.indexOf(chosen)], "id"));
}

void Window::EventsPage() {
  const auto columns = std::vector<std::pair<std::string, QString>>{
      {"id", "编号"}, {"name", "活动名称"}, {"category", "类别"}, {"status", "状态"}};
  if (Value(user_, "role") == "participant") {
    auto* search_bar = new QWidget;
    auto* search_layout = new QHBoxLayout(search_bar);
    search_layout->setContentsMargins(0, 0, 0, 0);
    auto* keyword = new QLineEdit;
    keyword->setObjectName("eventKeyword");
    keyword->setPlaceholderText("搜索活动或关键词");
    auto* search = new QPushButton("筛选");
    search->setProperty("secondary", true);
    search_layout->addWidget(keyword, 1);
    search_layout->addWidget(search);
    body_->addWidget(search_bar);
    auto* options = new QWidget;
    auto* option_layout = new QHBoxLayout(options);
    option_layout->setContentsMargins(0, 0, 0, 0);
    auto* category = new QComboBox;
    auto* location = new QComboBox;
    auto* date = new QComboBox;
    category->addItem("类别", "");
    location->addItem("地点", "");
    date->addItem("日期", "");
    for (const auto& event : service_.Events()) {
      const auto kind = Text(Value(event, "category"));
      if (category->findData(kind) < 0) {
        category->addItem(kind, kind);
      }
      for (const auto& session : service_.Sessions(Value(event, "id"))) {
        const auto place = Text(Value(session, "location"));
        const auto day = Text(TimeText(Number(session, "start")).substr(0, 10));
        if (location->findData(place) < 0) {
          location->addItem(place, place);
        }
        if (date->findData(day) < 0) {
          date->addItem(day, day);
        }
      }
    }
    for (auto* box : {category, location, date}) {
      box->setMinimumWidth(0);
      box->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
      option_layout->addWidget(box, 1);
    }
    body_->addWidget(options);
    const auto update = [this, keyword, category, location, date, columns] {
      SetRecords(service_.Events({String(keyword->text()),
                                  category->currentData().toString().toStdString(),
                                  location->currentData().toString().toStdString(),
                                  date->currentData().toString().toStdString(), ""}),
                 columns);
    };
    connect(keyword, &QLineEdit::returnPressed, this, [this, update] { Run(update, false); });
    for (auto* box : {category, location, date}) {
      connect(box, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
              [this, update](int) { Run(update, false); });
    }
    connect(search, &QPushButton::clicked, this, [this, keyword, category, location, date, update] {
      Run(
          [&] {
            QDialog dialog(this);
            dialog.setWindowTitle("筛选活动");
            auto* form = new QFormLayout(&dialog);
            auto* word = Field(form, "关键词", keyword->text());
            auto* kind = Field(form, "活动类别", category->currentData().toString());
            auto* place = Field(form, "地点", location->currentData().toString());
            auto* day = Field(form, "场次日期 YYYY-MM-DD", date->currentData().toString());
            if (Accept(&dialog, form)) {
              keyword->setText(word->text());
              auto set = [](QComboBox* box, const QString& text) {
                int index = box->findData(text);
                if (index < 0) {
                  box->addItem(text, text);
                  index = box->count() - 1;
                }
                box->setCurrentIndex(index);
              };
              // Block intermediate refreshes while applying the complete filter.
              for (auto* box : {category, location, date}) {
                box->blockSignals(true);
              }
              set(category, kind->text().trimmed());
              set(location, place->text().trimmed());
              set(date, day->text().trimmed());
              for (auto* box : {category, location, date}) {
                box->blockSignals(false);
              }
              update();
            }
          },
          false);
    });
    update();
    return;
  }
  SetRecords(service_.Events(), columns);
  Button("新建活动", [this] {
    draft_event_.clear();
    ShowPage("新建活动");
  });
  auto* applications = new QPushButton("我的申请 · 查看 / 撤回");
  applications->setObjectName("myApplicationsButton");
  applications->setProperty("secondary", true);
  body_->insertWidget(2, applications);
  connect(applications, &QPushButton::clicked, this,
          [this] { Run([&] { ShowPage("我的申请"); }, false); });
  Button("编辑活动 / 申请发布后修改", [this] { EditEventDialog(Selected()); });
  Button("添加场次（草稿）", [this] { AddSessionDialog(Selected()); });
  Button("添加票种与座位（草稿）", [this] { AddTicketDialog(Selected()); });
  Button("编辑草稿场次", [this] {
    const auto event = Value(Selected(), "id");
    const auto session = Choose("选择场次", service_.Sessions(event)).toStdString();
    const auto name = String(Ask("编辑场次", "名称"));
    const auto location = String(Ask("编辑场次", "地点"));
    const auto deadline = ParseTime(String(Ask("编辑场次", "截止 YYYY-MM-DD HH:MM")));
    const auto start = ParseTime(String(Ask("编辑场次", "开始 YYYY-MM-DD HH:MM")));
    const auto end = ParseTime(String(Ask("编辑场次", "结束 YYYY-MM-DD HH:MM")));
    bool accepted;
    const auto capacity = QInputDialog::getInt(this, "场次", "容量", 50, 1, 100000, 1, &accepted);
    if (accepted) {
      service_.EditSessionDraft(session, {name, location, deadline, start, end, capacity},
                                RequestId());
    }
  });
  Button("编辑草稿票种与座位", [this] {
    const auto session =
        Choose("选择场次", service_.Sessions(Value(Selected(), "id"))).toStdString();
    const auto ticket = Choose("选择票种", service_.Tickets(session)).toStdString();
    const auto name = String(Ask("票种", "名称"));
    const auto seats = Ask("座位", "座位编号（英文逗号分隔）");
    Params labels;
    for (const auto& label : seats.split(',')) {
      labels.push_back(String(label));
    }
    bool accepted;
    const auto price =
        QInputDialog::getInt(this, "票种", "价格（分）", 0, 0, 10000000, 1, &accepted);
    if (accepted) {
      service_.EditTicketDraft(ticket, name, price, labels, RequestId());
    }
  });
  Button("删除草稿场次 / 票种", [this] {
    const auto scope = String(Ask("对象", "输入 session 或 ticket", "ticket"));
    const auto session =
        Choose("选择场次", service_.Sessions(Value(Selected(), "id"))).toStdString();
    const auto target =
        scope == "session" ? session : Choose("选择票种", service_.Tickets(session)).toStdString();
    service_.DeleteDraftChild(scope, target, RequestId());
  });
  Button("提交发布审核", [this] { service_.Submit(Value(Selected(), "id"), RequestId()); });
  Button("申请取消活动 / 场次 / 票种", [this] {
    const auto event = Value(Selected(), "id");
    const auto scope = String(Ask("取消范围", "输入 event、session 或 ticket", "event"));
    std::string target = event;
    if (scope != "event") {
      const auto session = Choose("选择场次", service_.Sessions(event)).toStdString();
      target = scope == "session" ? session
                                  : Choose("选择票种", service_.Tickets(session)).toStdString();
    }
    service_.RequestCancellation(event, scope, target, RequestId());
  });
  Button("删除草稿", [this] { service_.DeleteDraft(Value(Selected(), "id"), RequestId()); });
}

void Window::EventDetails(const Row& event) {
  detail_event_ = event;
  const auto sessions = service_.Sessions(Value(event, "id"));
  detail_session_ = sessions.empty() ? "" : Value(sessions.front(), "id");
  ShowPage("活动详情");
}

void Window::DetailsPage() {
  auto* back = new QPushButton("‹ 返回发现活动");
  back->setProperty("secondary", true);
  body_->addWidget(back);
  connect(back, &QPushButton::clicked, this, [this] { Run([&] { ShowPage("发现活动"); }, false); });
  auto* title = new QLabel(Text(Value(detail_event_, "name")));
  title->setObjectName("title");
  title->setWordWrap(true);
  body_->addWidget(title);
  body_->addWidget(new QLabel("选择场次"));
  auto* sessions = new QComboBox;
  const auto rows = service_.Sessions(Value(detail_event_, "id"));
  for (const auto& row : rows) {
    sessions->addItem(Text(Value(row, "name") + "，" + TimeText(Number(row, "start"))),
                      Text(Value(row, "id")));
  }
  sessions->setMinimumWidth(0);
  sessions->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  body_->addWidget(sessions);
  auto* location = new QLabel;
  location->setWordWrap(true);
  auto* deadline = new QLabel;
  deadline->setWordWrap(true);
  body_->addWidget(location);
  body_->addWidget(deadline);
  const auto update = [this, sessions, rows, location, deadline] {
    if (sessions->currentIndex() < 0) {
      return;
    }
    const auto& row = rows[sessions->currentIndex()];
    detail_session_ = Value(row, "id");
    location->setText("地点：" + Text(Value(row, "location")));
    deadline->setText("报名截止：" + Text(TimeText(Number(row, "deadline"))));
  };
  connect(sessions, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [update](int) { update(); });
  update();
  auto* note = new QLabel("参与者仅选票种，不支持选座。\n座位将在报名截止后按确认顺序分配。");
  note->setWordWrap(true);
  note->setObjectName("banner");
  note->setStyleSheet("font-size:14px;");
  body_->addWidget(note);
  auto* next = new QPushButton("选择票种");
  next->setEnabled(!rows.empty());
  body_->addWidget(next);
  connect(next, &QPushButton::clicked, this, [this] { Run([&] { ShowPage("选择票种"); }, false); });
  body_->addWidget(new QLabel("活动简介"));
  auto* description = new QLabel(Text(Value(detail_event_, "description")));
  description->setWordWrap(true);
  description->setTextFormat(Qt::PlainText);
  body_->addWidget(description);
}

void Window::TicketPage() {
  auto* back = new QPushButton("‹ 返回活动详情");
  back->setProperty("secondary", true);
  body_->addWidget(back);
  connect(back, &QPushButton::clicked, this, [this] { Run([&] { ShowPage("活动详情"); }, false); });
  auto* choices = new QButtonGroup(this);
  const auto tickets = service_.Tickets(detail_session_);
  auto* amount = new QLabel("请选择票种");
  for (std::size_t i = 0; i < tickets.size(); ++i) {
    const auto ticket = tickets[i];
    auto* card = new QFrame;
    card->setObjectName("card");
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(18, 18, 18, 18);
    auto* choice = new QRadioButton(Text(Value(ticket, "name")));
    choices->addButton(choice, static_cast<int>(i));
    layout->addWidget(choice);
    const auto remaining = Number(ticket, "capacity") - Number(ticket, "occupied");
    layout->addWidget(
        new QLabel(remaining > 0 ? QString("剩余 %1 个名额").arg(remaining) : "已满，可进入候补"));
    layout->addWidget(new QLabel(Format(ticket, "price")));
    body_->addWidget(card);
    connect(choice, &QRadioButton::toggled, this, [ticket, amount](bool checked) {
      if (checked) {
        amount->setText("应付金额：" + Format(ticket, "price"));
      }
    });
  }
  body_->addWidget(amount);
  auto* confirm = new QPushButton("确认报名");
  confirm->setEnabled(!tickets.empty());
  body_->addWidget(confirm);
  if (!tickets.empty()) {
    choices->button(0)->setChecked(true);
  }
  connect(confirm, &QPushButton::clicked, this, [this, choices, tickets] {
    Run(
        [&] {
          const auto index = choices->checkedId();
          if (index < 0) {
            throw Error("请选择票种");
          }
          service_.Register(Value(tickets[index], "id"), RequestId());
          ShowPage("我的报名");
        },
        false);
  });
}

void Window::CreateEventPage() {
  const bool published = Value(draft_event_, "status") == "published";
  auto* form_widget = new QWidget;
  form_widget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  auto* form = new QFormLayout(form_widget);
  form->setContentsMargins(0, 0, 0, 0);
  form->setVerticalSpacing(12);
  auto* name = Field(form, "活动名称", Text(Value(draft_event_, "name")));
  name->setObjectName("createEventName");
  name->setPlaceholderText("请输入活动名称，支持中文");
  auto* category = Field(form, "类别", Text(Value(draft_event_, "category")));
  category->setObjectName("createEventCategory");
  category->setPlaceholderText("例如：文艺演出、校园讲座");
  auto* description = new QTextEdit;
  description->setObjectName("createEventDescription");
  description->setAcceptRichText(false);
  description->setAttribute(Qt::WA_InputMethodEnabled, true);
  description->setInputMethodHints(Qt::ImhNone);
  description->setPlaceholderText("请输入活动简介");
  description->setFixedHeight(110);
  description->setPlainText(Text(Value(draft_event_, "description")));
  form->addRow("简介", description);
  body_->addWidget(form_widget);
  auto* note = new QLabel(
      published ? "可修改活动、场次和票种，支持新增场次及票种。全部改动提交同一条修改审核；通过前保"
                  "留原发布内容。"
                : "填写场次时间、容量和票种座位。保存草稿可暂不完整；提交发布审核前需全部合法。");
  note->setObjectName("muted");
  note->setWordWrap(true);
  body_->addWidget(note);
  auto* session_holder = new QWidget;
  auto* session_layout = new QVBoxLayout(session_holder);
  session_layout->setSizeConstraint(QLayout::SetMinimumSize);
  session_layout->setContentsMargins(0, 0, 0, 0);
  session_layout->setSpacing(16);
  body_->addWidget(session_holder);
  auto editors = std::make_shared<std::vector<SessionEditor*>>();
  const auto add_session = [editors, session_layout, published](const DraftSession& input) {
    auto* editor = new SessionEditor(input, static_cast<int>(editors->size() + 1), published);
    editors->push_back(editor);
    session_layout->addWidget(editor);
  };
  const auto event_id = Value(draft_event_, "id");
  if (!event_id.empty()) {
    for (const auto& input :
         (published ? service_.EventConfiguration(event_id) : service_.DraftSessions(event_id))) {
      add_session(input);
    }
  }
  if (editors->empty()) {
    add_session(DefaultDraftSession(1));
  }
  auto* add = new QPushButton("添加场次 ＋");
  add->setObjectName("addDraftSessionButton");
  add->setProperty("secondary", true);
  body_->addWidget(add);
  connect(add, &QPushButton::clicked, this, [add_session, editors] {
    add_session(DefaultDraftSession(static_cast<int>(editors->size() + 1)));
  });
  const auto persist = [this, event_id, name, category, description, editors,
                        published](bool submit) {
    std::vector<DraftSession> sessions;
    for (const auto* editor : *editors) {
      if (!editor->removed) {
        sessions.push_back(editor->Read());
      }
    }
    if (published) {
      service_.ProposeCompleteEdit(event_id, String(name->text()), String(category->text()),
                                   String(description->toPlainText()), sessions, RequestId());
      draft_event_.clear();
      ShowPage("我的申请");
      return;
    }
    const auto id =
        service_.SaveDraft(event_id, String(name->text()), String(category->text()),
                           String(description->toPlainText()), sessions, submit, RequestId());
    draft_event_.clear();
    ShowPage(submit ? "我的申请" : "我的活动");
    if (!submit) {
      for (std::size_t i = 0; i < displayed_.size(); ++i) {
        if (Value(displayed_[i], "id") == id) {
          selected_row_ = static_cast<int>(i);
        }
      }
    }
  };
  auto* save = new QPushButton("保存活动草稿");
  save->setObjectName("saveEventButton");
  save->setVisible(!published);
  body_->addWidget(save);
  connect(save, &QPushButton::clicked, this,
          [this, persist] { Run([&] { persist(false); }, false); });
  auto* submit = new QPushButton("保存并提交发布审核");
  submit->setObjectName("submitEventButton");
  submit->setVisible(!published);
  body_->addWidget(submit);
  connect(submit, &QPushButton::clicked, this,
          [this, persist] { Run([&] { persist(true); }, false); });
  if (published) {
    auto* propose = new QPushButton("提交修改审核");
    propose->setObjectName("submitPublishedEditButton");
    body_->addWidget(propose);
    connect(propose, &QPushButton::clicked, this,
            [this, persist] { Run([&] { persist(true); }, false); });
  }
  auto* cancel = new QPushButton("取消，返回我的活动");
  cancel->setProperty("secondary", true);
  body_->addWidget(cancel);
  connect(cancel, &QPushButton::clicked, this,
          [this] { Run([&] { ShowPage("我的活动"); }, false); });
  name->setFocus(Qt::OtherFocusReason);
}

void Window::EditEventDialog(const Row& event) {
  if (Value(event, "status") != "draft" && Value(event, "status") != "published") {
    throw Error("活动已取消，不能编辑");
  }
  draft_event_ = event;
  ShowPage(Value(event, "status") == "draft" ? "编辑草稿" : "修改已发布活动");
}

void Window::AddSessionDialog(const Row& event) {
  QDialog dialog(this);
  dialog.setWindowTitle("添加场次");
  auto* form = new QFormLayout(&dialog);
  auto* name = Field(form, "场次名称", "第一场");
  auto* location = Field(form, "地点");
  auto* deadline = Field(form, "截止 YYYY-MM-DD HH:MM", Text(TimeText(Now() + 86400)));
  auto* start = Field(form, "开始 YYYY-MM-DD HH:MM", Text(TimeText(Now() + 90000)));
  auto* end = Field(form, "结束 YYYY-MM-DD HH:MM", Text(TimeText(Now() + 93600)));
  auto* capacity = new QSpinBox;
  capacity->setRange(1, 100000);
  capacity->setValue(50);
  form->addRow("容量", capacity);
  if (Accept(&dialog, form)) {
    service_.AddSession(
        Value(event, "id"),
        {String(name->text()), String(location->text()), ParseTime(String(deadline->text())),
         ParseTime(String(start->text())), ParseTime(String(end->text())), capacity->value()},
        RequestId());
  }
}

void Window::AddTicketDialog(const Row& event) {
  const auto session = Choose("选择场次", service_.Sessions(Value(event, "id"))).toStdString();
  QDialog dialog(this);
  dialog.setWindowTitle("添加票种");
  auto* form = new QFormLayout(&dialog);
  auto* name = Field(form, "名称", "普通票");
  auto* price = new QSpinBox;
  price->setRange(0, 10000000);
  form->addRow("价格（分，免费填0）", price);
  auto* seats = Field(form, "座位编号（英文逗号分隔）", "A01,A02,A03");
  if (Accept(&dialog, form)) {
    Params labels;
    for (const auto& seat : seats->text().split(',')) {
      labels.push_back(String(seat));
    }
    service_.AddTicket(session, String(name->text()), price->value(),
                       static_cast<int>(labels.size()), labels, RequestId());
  }
}

void Window::RegistrationsPage() {
  const bool organizer = Value(user_, "role") == "organizer";
  QComboBox* events = nullptr;
  QComboBox* sessions = nullptr;
  if (organizer) {
    body_->addWidget(new QLabel("活动"));
    events = new QComboBox;
    events->setObjectName("registrationEventFilter");
    events->addItem("全部活动", "");
    for (const auto& event : service_.Events()) {
      events->addItem(Text(Value(event, "name") + "（编号 " + Value(event, "id") + "）"),
                      Text(Value(event, "id")));
    }
    events->setCurrentIndex(std::max(0, events->findData(Text(registration_event_))));
    registration_event_ = String(events->currentData().toString());
    body_->addWidget(events);
    body_->addWidget(new QLabel("场次"));
    sessions = new QComboBox;
    sessions->setObjectName("registrationSessionFilter");
    body_->addWidget(sessions);
  }
  auto* filter = new QComboBox;
  filter->setObjectName("registrationStatusFilter");
  filter->addItem("全部状态", "");
  for (const auto& status :
       {"waiting", "pending", "registered", "cancelled", "checked", "absent"}) {
    filter->addItem(Text(StatusText(status)), status);
  }
  body_->addWidget(filter);
  if (organizer) {
    filter->setCurrentIndex(std::max(0, filter->findData(Text(registration_status_))));
  }
  auto columns = std::vector<std::pair<std::string, QString>>{
      {"id", "报名编号"},        {"participant", "姓名"},
      {"event_name", "活动"},    {"session_name", "场次"},
      {"ticket_name", "票种"},   {"status", "状态"},
      {"seat", "座位"},          {"payment_due", "付款截止"},
      {"received", "实收"},      {"refund_pending", "待退款"},
      {"checked_at", "签到时间"}};
  if (Value(user_, "role") == "checker") {
    columns = {{"id", "报名编号"},       {"participant", "姓名"}, {"event_name", "活动"},
               {"session_name", "场次"}, {"seat", "座位"},        {"status", "状态"}};
  }
  const auto update = [this, filter, columns, events, sessions] {
    Search search;
    search.status = filter->currentData().toString().toStdString();
    std::string event;
    if (events) {
      event = String(events->currentData().toString());
      search.session = String(sessions->currentData().toString());
      registration_event_ = event;
      registration_session_ = search.session;
      registration_status_ = search.status;
    }
    SetRecords(service_.Registrations(event, search), columns);
  };
  if (organizer) {
    const auto populate_sessions = [this, events, sessions] {
      const QSignalBlocker blocker(sessions);
      sessions->clear();
      sessions->addItem("全部场次", "");
      const auto event = String(events->currentData().toString());
      if (!event.empty()) {
        for (const auto& session : service_.Sessions(event)) {
          sessions->addItem(
              Text(Value(session, "name") + "（" + TimeText(Number(session, "start")) + "，编号 " +
                   Value(session, "id") + "）"),
              Text(Value(session, "id")));
        }
      }
      sessions->setEnabled(!event.empty());
      sessions->setCurrentIndex(std::max(0, sessions->findData(Text(registration_session_))));
    };
    populate_sessions();
    connect(events, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, populate_sessions, update](int) {
              Run(
                  [&] {
                    registration_session_.clear();
                    populate_sessions();
                    update();
                  },
                  false);
            });
    connect(sessions, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, update](int) { Run(update, false); });
  }
  connect(filter, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this, update](int) { Run(update, false); });
  update();
  const auto role = Value(user_, "role");
  if (role == "participant") {
    Button("取消本人报名", [this] { service_.Cancel(Value(Selected(), "id"), RequestId()); });
  } else if (role == "checker") {
    Button("核验并签到", [this] { service_.CheckIn(Value(Selected(), "id"), RequestId()); });
  } else {
    Button("确认已收到线下付款", [this] {
      const auto row = Selected();
      const auto answer = QMessageBox::question(
          this, "核对实收", "确认已经在线下实际收到 " + Format(row, "price") + "？");
      if (answer == QMessageBox::Yes) {
        service_.Receive(Value(row, "id"), static_cast<int>(Number(row, "price")), RequestId());
      }
    });
    Button("登记超时 / 取消后的异常实收（全额待退款）", [this] {
      const auto row = Selected();
      bool accepted;
      const auto amount = QInputDialog::getInt(this, "异常实收", "实际收到金额（分）", 1, 1,
                                               10000000, 1, &accepted);
      if (accepted) {
        service_.AbnormalReceipt(Value(row, "id"), amount, RequestId());
      }
    });
    Button("确认已完成线下全额退款", [this] {
      const auto row = Selected();
      Rows pending;
      for (const auto& refund : service_.Refunds(Value(row, "event_id"))) {
        if (Value(refund, "registration_id") == Value(row, "id") &&
            Value(refund, "status") == "pending") {
          pending.push_back(refund);
        }
      }
      const auto refund = Choose("选择已线下全额退还的原收款", pending, "amount").toStdString();
      if (QMessageBox::question(this, "退款确认", "是否已经完成实际线下全额退款？") ==
          QMessageBox::Yes) {
        service_.CompleteRefund(refund, RequestId());
      }
    });
  }
}

void Window::ReviewsPage() {
  SetRecords(service_.Reviews(), {{"id", "申请编号"},
                                  {"event_name", "活动"},
                                  {"kind", "类型"},
                                  {"scope", "范围"},
                                  {"target_id", "目标编号"},
                                  {"status", "状态"},
                                  {"proposed_name", "候选名称"},
                                  {"reason", "审核意见"}});
  Button("查看原内容与候选内容", [this] {
    QDialog dialog(this);
    dialog.setWindowTitle("审核内容对照");
    dialog.resize(361, 650);
    auto* layout = new QVBoxLayout(&dialog);
    auto* details = new QTextEdit;
    details->setReadOnly(true);
    details->setPlainText(Text(service_.ReviewDetails(Value(Selected(), "id"))));
    layout->addWidget(details);
    dialog.exec();
  });
  if (Value(user_, "role") == "organizer") {
    Button("撤回选中申请", [this] { service_.Withdraw(Value(Selected(), "id"), RequestId()); });
    Button("为修改申请修改 / 新增票种", [this] {
      const auto review = Selected();
      const auto session =
          Choose("选择场次", service_.Sessions(Value(review, "event_id"))).toStdString();
      const auto mode = QMessageBox::question(this, "票种", "是否新增票种？选择否可修改原票种。");
      const auto ticket = mode == QMessageBox::Yes
                              ? std::string{}
                              : Choose("选择票种", service_.Tickets(session)).toStdString();
      const auto name = String(Ask("票种", "名称"));
      const auto seats = Ask("座位", "座位编号（英文逗号分隔）");
      Params labels;
      for (const auto& label : seats.split(',')) {
        labels.push_back(String(label));
      }
      bool accepted;
      const auto price =
          QInputDialog::getInt(this, "票种", "价格（分）", 0, 0, 10000000, 1, &accepted);
      if (accepted) {
        service_.ProposeTicket(Value(review, "id"), session, ticket, name, price, labels,
                               RequestId());
      }
    });
    Button("为修改申请添加场次变更", [this] {
      const auto review = Selected();
      const auto session =
          Choose("选择场次", service_.Sessions(Value(review, "event_id"))).toStdString();
      const auto location = String(Ask("场次修改", "新地点"));
      const auto deadline = ParseTime(String(Ask("场次修改", "新截止 YYYY-MM-DD HH:MM")));
      const auto start = ParseTime(String(Ask("场次修改", "新开始 YYYY-MM-DD HH:MM")));
      const auto end = ParseTime(String(Ask("场次修改", "新结束 YYYY-MM-DD HH:MM")));
      bool accepted;
      const auto capacity =
          QInputDialog::getInt(this, "场次修改", "新容量", 50, 1, 100000, 1, &accepted);
      if (accepted) {
        service_.ProposeSession(Value(review, "id"), session,
                                {"场次", location, deadline, start, end, capacity}, RequestId());
      }
    });
  } else {
    Button("通过审核", [this] {
      service_.Decide(Value(Selected(), "id"), true, String(Ask("审核", "审核意见（可空）")),
                      RequestId());
    });
    Button("拒绝申请（必填原因）", [this] {
      service_.Decide(Value(Selected(), "id"), false, String(Ask("拒绝", "原因")), RequestId());
    });
  }
}

void Window::UsersPage() {
  SetRecords(service_.Users(), {{"id", "编号"},
                                {"username", "账号"},
                                {"name", "姓名"},
                                {"role", "角色"},
                                {"enabled", "状态"}});
  Button("创建新账号", [this] {
    QDialog dialog(this);
    dialog.setWindowTitle("创建新账号");
    auto* form = new QFormLayout(&dialog);
    auto* username = Field(form, "账号");
    auto* name = Field(form, "姓名");
    auto* password = Field(form, "初始密码（至少8位）");
    password->setEchoMode(QLineEdit::Password);
    auto* role = new QComboBox;
    for (const auto& value : {"participant", "organizer", "checker", "admin"}) {
      role->addItem(Text(RoleText(value)), value);
    }
    form->addRow("角色", role);
    if (Accept(&dialog, form)) {
      service_.CreateUser(String(username->text()), String(name->text()),
                          role->currentData().toString().toStdString(),
                          password->text().toStdString(), RequestId());
    }
  });
  Button("编辑账号", [this] {
    editing_user_ = Selected();
    ShowPage("编辑账号");
  });
  Button("启用 / 停用选中账号", [this] {
    const auto row = Selected();
    service_.SetEnabled(Value(row, "id"), Value(row, "enabled") != "1", RequestId());
  });
}

void Window::EditUserPage() {
  const auto id = Value(editing_user_, "id");
  Row current;
  for (const auto& user : service_.Users()) {
    if (Value(user, "id") == id) {
      current = user;
    }
  }
  if (current.empty()) {
    throw Error("账号已不存在，请刷新账号列表");
  }
  auto* widget = new QWidget;
  auto* form = new QFormLayout(widget);
  form->setContentsMargins(0, 0, 0, 0);
  auto* username = Field(form, "账号（登录名保持不变）", Text(Value(current, "username")));
  username->setReadOnly(true);
  auto* name = Field(form, "姓名", Text(Value(current, "name")));
  name->setObjectName("editUserName");
  auto* role = new QComboBox;
  role->setObjectName("editUserRole");
  for (const auto& value : {"participant", "organizer", "checker", "admin"}) {
    role->addItem(Text(RoleText(value)), value);
  }
  role->setCurrentIndex(role->findData(Text(Value(current, "role"))));
  form->addRow("身份 / 角色", role);
  auto* enabled = new QCheckBox("启用此账号");
  enabled->setObjectName("editUserEnabled");
  enabled->setChecked(Value(current, "enabled") == "1");
  form->addRow("账号状态", enabled);
  if (id == Value(user_, "id")) {
    role->setEnabled(false);
    enabled->setEnabled(false);
  }
  body_->addWidget(widget);
  auto* note = new QLabel(
      "创建时填错身份，可以在账号尚无业务记录时修改。检票人员若已有分配，请先撤销全部分配；已有活动"
      "、报名、审核或资金记录的账号保留原角色，姓名仍可修改。");
  note->setWordWrap(true);
  note->setObjectName("muted");
  body_->addWidget(note);
  auto* save = new QPushButton("保存账号修改");
  save->setObjectName("saveUserButton");
  body_->addWidget(save);
  connect(save, &QPushButton::clicked, this, [this, id, name, role, enabled] {
    Run(
        [&] {
          service_.EditUser(id, String(name->text()), role->currentData().toString().toStdString(),
                            enabled->isChecked(), RequestId());
          if (id == Value(user_, "id")) {
            user_ = service_.CurrentUser();
          }
          ShowPage("账号管理");
        },
        false);
  });
  auto* cancel = new QPushButton("取消，返回账号管理");
  cancel->setProperty("secondary", true);
  body_->addWidget(cancel);
  connect(cancel, &QPushButton::clicked, this,
          [this] { Run([&] { ShowPage("账号管理"); }, false); });
  name->setFocus();
}

void Window::AssignmentPage() {
  auto* operation = new QComboBox;
  operation->setObjectName("assignmentOperation");
  operation->addItem("新增分配", false);
  operation->addItem("撤销已有分配", true);
  body_->addWidget(operation);
  auto* form_widget = new QWidget;
  auto* form = new QFormLayout(form_widget);
  form->setRowWrapPolicy(QFormLayout::WrapAllRows);
  auto* events = new QComboBox;
  auto* sessions = new QComboBox;
  auto* checkers = new QComboBox;
  events->setObjectName("assignmentEvent");
  sessions->setObjectName("assignmentSession");
  checkers->setObjectName("assignmentChecker");
  for (auto* box : {events, sessions, checkers}) {
    box->setMinimumWidth(0);
    box->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  }
  form->addRow("活动", events);
  form->addRow("场次", sessions);
  form->addRow("检票人员", checkers);
  body_->addWidget(form_widget);
  auto* summary = new QLabel;
  summary->setWordWrap(true);
  body_->addWidget(summary);
  auto* hint = new QLabel;
  hint->setWordWrap(true);
  hint->setObjectName("muted");
  body_->addWidget(hint);
  auto* confirm = new QPushButton("确认分配");
  confirm->setObjectName("assignmentConfirm");
  body_->addWidget(confirm);
  const auto update_people = [this, sessions, checkers, operation, summary, hint, confirm] {
    const auto previous = checkers->currentData();
    checkers->clear();
    const bool remove = operation->currentData().toBool();
    confirm->setText(remove ? "确认撤销分配" : "确认分配");
    QStringList assigned_names;
    if (sessions->currentIndex() >= 0) {
      const auto session = sessions->currentData().toString().toStdString();
      for (const auto& user : service_.AssignmentCheckers(session, true)) {
        assigned_names << Text(Value(user, "name") + "（" + Value(user, "username") +
                               (Value(user, "enabled") == "1" ? "）" : "，已停用）"));
      }
      for (const auto& user : service_.AssignmentCheckers(session, remove)) {
        checkers->addItem(Text(Value(user, "name") + " · " + Value(user, "username") +
                               (Value(user, "enabled") == "1" ? "" : "（已停用）")),
                          Text(Value(user, "id")));
      }
    }
    if (checkers->findData(previous) >= 0) {
      checkers->setCurrentIndex(checkers->findData(previous));
    }
    summary->setText("当前已分配：" +
                     (assigned_names.isEmpty() ? QString("暂无") : assigned_names.join("、")));
    confirm->setEnabled(sessions->currentIndex() >= 0 && checkers->count() > 0);
    if (sessions->currentIndex() < 0) {
      hint->setText(remove ? "当前没有已有检票分配。"
                           : "没有可新增分配的场次。请由组织者创建未来场次并提交管理员审核；已结束"
                             "的演示活动不会自动延期。");
    } else if (checkers->count() == 0) {
      hint->setText(remove ? "此场次没有已分配人员。"
                           : "没有可新增的检票人员。已有分配不会重复列出；请到账号管理创建或启用新"
                             "的检票人员账号。");
    } else {
      hint->setText(
          remove ? "撤销只影响场次授权，不删除账号；已停用账号也可撤销。"
                 : "请选择尚未分配且时间不冲突的检票人员；一场结束时另一场开始允许连续分配。");
    }
  };
  const auto update_sessions = [this, events, sessions, operation, update_people] {
    const auto previous = sessions->currentData();
    sessions->blockSignals(true);
    sessions->clear();
    const bool remove = operation->currentData().toBool();
    for (const auto& row : service_.AssignmentSessions()) {
      if (Text(Value(row, "event_id")) != events->currentData().toString()) {
        continue;
      }
      if (remove ? Number(row, "assigned_count") == 0 : Value(row, "can_assign") != "1") {
        continue;
      }
      sessions->addItem(Text(Value(row, "name") + " · " + TimeText(Number(row, "start")) +
                             " · 已分配" + Value(row, "assigned_count") + "人"),
                        Text(Value(row, "id")));
    }
    if (sessions->findData(previous) >= 0) {
      sessions->setCurrentIndex(sessions->findData(previous));
    }
    sessions->blockSignals(false);
    update_people();
  };
  const auto refresh = [this, events, operation, update_sessions] {
    const auto previous = events->currentData();
    events->blockSignals(true);
    events->clear();
    const bool remove = operation->currentData().toBool();
    for (const auto& row : service_.AssignmentSessions()) {
      if (remove ? Number(row, "assigned_count") == 0 : Value(row, "can_assign") != "1") {
        continue;
      }
      const auto id = Text(Value(row, "event_id"));
      if (events->findData(id) < 0) {
        events->addItem(Text(Value(row, "event_name")), id);
      }
    }
    if (events->findData(previous) >= 0) {
      events->setCurrentIndex(events->findData(previous));
    }
    events->blockSignals(false);
    update_sessions();
  };
  connect(events, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this, update_sessions](int) { Run(update_sessions, false); });
  connect(sessions, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this, update_people](int) { Run(update_people, false); });
  connect(operation, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this, refresh](int) { Run(refresh, false); });
  connect(confirm, &QPushButton::clicked, this, [this, sessions, checkers, operation, refresh] {
    Run(
        [&] {
          if (sessions->currentIndex() < 0 || checkers->currentIndex() < 0) {
            throw Error("请选择场次和检票人员");
          }
          service_.Assign(sessions->currentData().toString().toStdString(),
                          checkers->currentData().toString().toStdString(),
                          operation->currentData().toBool(), RequestId());
          refresh();
        },
        false);
  });
  auto* reload = new QPushButton("刷新活动与人员");
  reload->setProperty("secondary", true);
  body_->addWidget(reload);
  connect(reload, &QPushButton::clicked, this, [this, refresh] { Run(refresh, false); });
  refresh();
}

void Window::StatisticsPage() {
  auto* events = new QComboBox;
  events->setMinimumWidth(0);
  events->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  for (const auto& event : service_.Events()) {
    events->addItem(Text(Value(event, "name")), Text(Value(event, "id")));
  }
  body_->addWidget(events);
  auto* tiles = new QWidget;
  auto* grid = new QGridLayout(tiles);
  grid->setContentsMargins(0, 0, 0, 0);
  std::vector<QLabel*> values;
  const QStringList headings{"容量", "占用", "可分配剩余", "候补", "签到", "缺席"};
  for (int i = 0; i < headings.size(); ++i) {
    auto* card = new QFrame;
    card->setObjectName("card");
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* layout = new QVBoxLayout(card);
    auto* heading = new QLabel(headings[i]);
    heading->setAlignment(Qt::AlignCenter);
    heading->setObjectName("muted");
    auto* value = new QLabel("0");
    value->setObjectName("title");
    value->setAlignment(Qt::AlignCenter);
    values.push_back(value);
    layout->addWidget(heading);
    layout->addWidget(value);
    grid->addWidget(card, i / 2, i % 2);
  }
  body_->addWidget(tiles);
  auto* sessions = new QComboBox;
  auto* tickets = new QComboBox;
  auto* status = new QComboBox;
  for (auto* box : {sessions, tickets, status}) {
    box->setMinimumWidth(0);
    box->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    body_->addWidget(box);
  }
  status->addItem("全部报名状态", "");
  for (const auto& state : {"waiting", "pending", "registered", "cancelled", "checked", "absent"}) {
    status->addItem(Text(StatusText(state)), state);
  }
  const auto update_tickets = [this, events, sessions, tickets] {
    tickets->clear();
    tickets->addItem("全部票种", "");
    if (events->currentIndex() < 0) {
      return;
    }
    for (const auto& session : service_.Sessions(events->currentData().toString().toStdString())) {
      if (!sessions->currentData().toString().isEmpty() &&
          sessions->currentData().toString() != Text(Value(session, "id"))) {
        continue;
      }
      for (const auto& ticket : service_.Tickets(Value(session, "id"))) {
        tickets->addItem(Text(Value(ticket, "name")), Text(Value(ticket, "id")));
      }
    }
  };
  const auto update = [this, events, sessions, tickets, values] {
    std::vector<std::int64_t> totals(6, 0);
    if (events->currentIndex() >= 0) {
      const Params keys{"capacity", "occupied", "remaining", "waiting", "checked", "absent"};
      for (const auto& row : service_.Statistics(events->currentData().toString().toStdString())) {
        if (!sessions->currentData().toString().isEmpty() &&
            Text(Value(row, "session_id")) != sessions->currentData().toString()) {
          continue;
        }
        if (!tickets->currentData().toString().isEmpty() &&
            Text(Value(row, "id")) != tickets->currentData().toString()) {
          continue;
        }
        for (std::size_t i = 0; i < keys.size(); ++i) {
          totals[i] += Number(row, keys[i]);
        }
      }
    }
    for (std::size_t i = 0; i < values.size(); ++i) {
      values[i]->setText(QString::number(totals[i]));
    }
  };
  const auto update_sessions = [this, events, sessions, update_tickets, update] {
    sessions->blockSignals(true);
    sessions->clear();
    sessions->addItem("全部场次", "");
    if (events->currentIndex() >= 0) {
      for (const auto& row : service_.Sessions(events->currentData().toString().toStdString())) {
        sessions->addItem(Text(Value(row, "name")), Text(Value(row, "id")));
      }
    }
    sessions->blockSignals(false);
    update_tickets();
    update();
  };
  connect(events, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this, update_sessions](int) { Run(update_sessions, false); });
  connect(sessions, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this, update_tickets, update](int) {
            Run(
                [&] {
                  update_tickets();
                  update();
                },
                false);
          });
  connect(tickets, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this, update](int) { Run(update, false); });
  update_sessions();
  auto* export_button = new QPushButton("导出 CSV 名单");
  body_->addWidget(export_button);
  connect(export_button, &QPushButton::clicked, this, [this, events, sessions, tickets, status] {
    Run(
        [&] {
          if (events->currentIndex() < 0) {
            throw Error("请先选择活动");
          }
          const auto destination =
              QFileDialog::getSaveFileName(this, "导出名单", "registrations.csv", "CSV (*.csv)");
          if (!destination.isEmpty()) {
            service_.ExportCsv(events->currentData().toString().toStdString(),
                               destination.toStdString(),
                               sessions->currentData().toString().toStdString(),
                               tickets->currentData().toString().toStdString(),
                               status->currentData().toString().toStdString());
          }
        },
        false);
  });
}

void Window::LogsPage() {
  auto* filters = new QWidget;
  auto* form = new QFormLayout(filters);
  auto* event = Field(form, "活动编号（可空）");
  auto* action = Field(form, "操作类型（可空，如 receive）");
  auto* actor = Field(form, "操作者编号（可空）");
  body_->addWidget(filters);
  auto* query = new QPushButton("查询日志");
  body_->addWidget(query);
  const auto columns = std::vector<std::pair<std::string, QString>>{{"time", "时间"},
                                                                    {"actor", "操作者"},
                                                                    {"action", "操作"},
                                                                    {"event_id", "活动"},
                                                                    {"object_id", "对象"},
                                                                    {"before_state", "原状态"},
                                                                    {"after_state", "结果状态"},
                                                                    {"result", "结果/意见"},
                                                                    {"request_id", "请求编号"}};
  SetRecords(service_.Logs(), columns);
  connect(query, &QPushButton::clicked, this, [=] {
    Run(
        [&] {
          SetRecords(
              service_.Logs(String(event->text()), String(action->text()), String(actor->text())),
              columns);
        },
        false);
  });
}

void Window::NotificationsPage() {
  SetRecords(service_.Notifications(),
             {{"time", "时间"}, {"event_id", "活动编号"}, {"message", "通知"}});
}

void Window::SmokeScreenshot(const QString& directory) {
  QDir().mkpath(directory);
  QApplication::processEvents();
  if (!grab().save(directory + "/01-login.png")) {
    throw Error("截图保存失败");
  }
  const std::vector<std::pair<std::string, QString>> accounts{{"student", "02-participant"},
                                                              {"organizer", "03-organizer"},
                                                              {"admin", "04-admin"},
                                                              {"checker", "05-checker"}};
  for (const auto& account : accounts) {
    user_ = service_.Login(account.first, "Campus2026!");
    Shell();
    QApplication::processEvents();
    if (size() != QSize(393, 852)) {
      throw Error("竖屏尺寸发生变化");
    }
    if (account.first == "student") {
      auto* keyword = findChild<QLineEdit*>("eventKeyword");
      if (!keyword || tabs_.size() != 3) {
        throw Error("参与者搜索或底部导航缺失");
      }
      keyword->setFocus();
      QInputMethodEvent input;
      input.setCommitString("音乐会");
      QApplication::sendEvent(keyword, &input);
      if (keyword->text() != "音乐会") {
        throw Error("搜索框未接收中文输入事件");
      }
      QMetaObject::invokeMethod(keyword, "returnPressed", Qt::DirectConnection);
      if (displayed_.empty()) {
        throw Error("中文关键词未匹配演示活动");
      }
      QApplication::processEvents();
      if (!grab().save(directory + "/08-chinese-search.png")) {
        throw Error("截图保存失败");
      }
      keyword->clear();
      QMetaObject::invokeMethod(keyword, "returnPressed", Qt::DirectConnection);
    }
    QApplication::processEvents();
    if (!grab().save(directory + "/" + account.second + ".png")) {
      throw Error("截图保存失败");
    }
    if (account.first == "student") {
      const auto events = service_.Events();
      if (!events.empty()) {
        EventDetails(events.front());
        QApplication::processEvents();
        if (!grab().save(directory + "/06-event-details.png")) {
          throw Error("截图保存失败");
        }
        ShowPage("选择票种");
        QApplication::processEvents();
        if (!grab().save(directory + "/07-ticket-selection.png")) {
          throw Error("截图保存失败");
        }
      }
    }
    if (account.first == "organizer") {
      draft_event_.clear();
      ShowPage("新建活动");
      for (int round = 0; round < 4; ++round) {
        QApplication::processEvents();
      }
      if (!grab().save(directory + "/10-organizer-create-event.png")) {
        throw Error("截图保存失败");
      }
      auto* scroll = centralWidget()->findChild<QScrollArea*>();
      if (scroll) {
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum() / 2);
        QApplication::processEvents();
        if (!grab().save(directory + "/11-organizer-session-fields.png")) {
          throw Error("截图保存失败");
        }
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
        QApplication::processEvents();
        if (!grab().save(directory + "/12-organizer-submit-draft.png")) {
          throw Error("截图保存失败");
        }
      }
    }
    QStringList extra_pages;
    if (account.first == "student") {
      extra_pages = QStringList{"我的报名", "消息通知", "操作记录"};
    }
    if (account.first == "organizer") {
      extra_pages = QStringList{"报名与收退款", "统计与导出", "我的申请", "操作记录"};
    }
    if (account.first == "admin") {
      extra_pages = QStringList{"账号管理", "检票分配", "操作记录", "备份与检查"};
    }
    int page_index = 0;
    for (const auto& page : extra_pages) {
      ShowPage(page);
      QApplication::processEvents();
      if (page == "检票分配") {
        auto* operation = findChild<QComboBox*>("assignmentOperation");
        auto* session = findChild<QComboBox*>("assignmentSession");
        auto* checker = findChild<QComboBox*>("assignmentChecker");
        auto* confirm = findChild<QPushButton*>("assignmentConfirm");
        if (!operation || !session || !checker || !confirm) {
          throw Error("检票分配控件缺失");
        }
        for (int mode : {1, 0}) {
          operation->setCurrentIndex(mode);
          QApplication::processEvents();
          if (session->currentIndex() >= 0) {
            const auto expected = service_.AssignmentCheckers(
                session->currentData().toString().toStdString(), mode == 1);
            if (checker->count() != static_cast<int>(expected.size())) {
              throw Error("新增与撤销人员列表未同步");
            }
          }
          if (confirm->isEnabled() != (session->currentIndex() >= 0 && checker->count() > 0)) {
            throw Error("空分配列表仍允许提交");
          }
          if (mode == 1 && !grab().save(directory + "/09-assignment-remove.png")) {
            throw Error("截图保存失败");
          }
        }
      }
      const auto filename =
          directory + "/" + account.second + "-" + QString::number(++page_index) + ".png";
      if (!grab().save(filename)) {
        throw Error("截图保存失败");
      }
    }
  }
  service_.Logout();
  LoginPage();
}

}  // namespace campus
