#include <QApplication>
#include <QDir>
#include <QMessageBox>
#include <QStandardPaths>
#include <QTimer>
#include <iostream>
#include <string>

#include "campus/window.h"
int main(int argc, char* argv[]) {
  QApplication application(argc, argv);
  application.setApplicationName("CampusTickets");
  application.setOrganizationName("CampusLab");
  QString path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  QDir().mkpath(path);
  std::string database = (path + "/campus.db").toStdString();
  QString screenshots;
  bool demo = false;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--db" && i + 1 < argc) {
      database = argv[++i];
    } else if (argument == "--demo") {
      demo = true;
    } else if (argument == "--smoke" && i + 1 < argc) {
      screenshots = argv[++i];
    } else {
      std::cerr << "Usage: campus_gui [--db PATH] [--demo] [--smoke DIRECTORY]\n";
      return 2;
    }
  }
  try {
    if (demo) {
      campus::Service seed(database);
      seed.SeedDemo();
    }
    campus::Window window(database);
    window.show();
    if (!screenshots.isEmpty()) {
      QTimer::singleShot(100, &window, [&] {
        try {
          window.SmokeScreenshot(screenshots);
          application.exit(0);
        } catch (const std::exception& error) {
          std::cerr << error.what() << '\n';
          application.exit(1);
        }
      });
    }
    return application.exec();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
