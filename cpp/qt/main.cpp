// Entry point for the Qt/OpenGL tractography editor (the pure-Qt replacement
// for the VTK editor). Unlike the strict CLI editors, every argument is
// optional: the window can open empty and load via File ▸ Open TRK….

#include "args.hpp"
#include "main_window.hpp"
#include "theme.hpp"

#include <QApplication>
#include <QString>
#include <QTimer>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

// Lenient parse: fills an Args with whatever was provided; missing values keep
// their defaults. Returns the --trk path (empty if none) to load after show().
QString ParseQtArgs(const QStringList& argv, tracto::Args& args) {
  QString trk;
  for (int i = 1; i < argv.size(); ++i) {
    const QString key = argv[i];
    auto next = [&]() -> QString { return (i + 1 < argv.size()) ? argv[++i] : QString(); };
    if (key == "--trk") {
      trk = next();
      args.trkPath = trk.toStdString();
    } else if (key == "--volume" || key == "--fa") {  // --fa: deprecated alias
      args.volumePath = next().toStdString();
    } else if (key == "--display-n") {
      args.displayN = next().toInt();
    } else if (key == "--disp-step") {
      args.dispStep = next().toInt();
    } else if (key == "--seed") {
      args.seed = next().toULongLong();
    } else if (key == "--screenshot") {
      args.screenshotPath = next().toStdString();
    } else if (key == "--help" || key == "-h") {
      std::cout << "Usage: local_editor_qt [--trk in.trk] [--volume vol.nii.gz]\n"
                << "                       [--display-n N] [--disp-step N] [--seed N]\n"
                << "                       [--screenshot out.png]\n"
                << "All arguments optional; open/save files from the File menu too.\n";
      std::exit(0);
    }
  }
  // Clamp to a sane floor without re-spelling the defaults (those live in Args).
  args.displayN = std::max(1, args.displayN);
  args.dispStep = std::max(1, args.dispStep);
  return trk;
}

}  // namespace

int main(int argc, char** argv) {
  // The viewport is a QRhiWidget (Metal on macOS); it configures the graphics
  // API on the widget itself, so no QSurfaceFormat / GL-context request here.
  QApplication app(argc, argv);
  tracto::ApplyTheme(app);  // dark "pro" theme: Fusion base + palette + QSS

  tracto::Args args;
  const QString trk = ParseQtArgs(app.arguments(), args);

  tracto::MainWindow window(args);
  window.resize(1280, 900);
  window.show();
  if (!trk.isEmpty()) {
    window.LoadTractogram(trk);
  }
  if (!args.volumePath.empty()) {
    window.LoadVolume(QString::fromStdString(args.volumePath));
  }

  // Headless edit smoke test: delete the box and check the alive counts (CPU
  // only — no event loop needed), then exit with the verdict.
  if (app.arguments().contains("--selftest-edit")) {
    return window.RunEditSelfTest() ? 0 : 2;
  }

  // Headless verification: after the first frames settle, grab the viewport and
  // exit. Lets us confirm rendering without a human watching the window.
  if (!args.screenshotPath.empty()) {
    const QString shot = QString::fromStdString(args.screenshotPath);
    QTimer::singleShot(600, &window, [&window, shot]() {
      const bool ok = window.SaveScreenshot(shot);
      std::cout << (ok ? "screenshot -> " : "screenshot FAILED -> ")
                << shot.toStdString() << "\n";
      QApplication::quit();
    });
  }
  return app.exec();
}
