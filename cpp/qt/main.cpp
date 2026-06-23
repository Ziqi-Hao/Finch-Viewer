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
// their defaults. Returns the --trk paths (--trk is repeatable: load several
// tractograms, blended) to load after show().
QStringList ParseQtArgs(const QStringList& argv, tracto::Args& args) {
  QStringList trks;
  for (int i = 1; i < argv.size(); ++i) {
    const QString key = argv[i];
    auto next = [&]() -> QString { return (i + 1 < argv.size()) ? argv[++i] : QString(); };
    if (key == "--trk") {
      const QString p = next();
      if (!p.isEmpty()) { trks << p; args.trkPath = p.toStdString(); }
    } else if (key == "--volume" || key == "--fa") {  // --fa: deprecated alias
      args.volumePath = next().toStdString();
    } else if (key == "--label") {  // integer-label / segmentation NIfTI
      args.labelPath = next().toStdString();
    } else if (key == "--odf") {  // 4-D SH-coefficient ODF, rendered as glyphs
      args.odfPath = next().toStdString();
    } else if (key == "--discrete-odf") {  // sphere-sampled ODF via embedded sphere (opt-in)
      args.discreteOdfPath = next().toStdString();
    } else if (key == "--peaks") {  // 4-D peaks field, rendered as DEC line segments
      args.peaksPath = next().toStdString();
    } else if (key == "--open") {  // any file: exercise the unified auto-detect router
      args.openPath = next().toStdString();
    } else if (key == "--display-n") {
      args.displayN = next().toInt();
    } else if (key == "--disp-step") {
      args.dispStep = next().toInt();
    } else if (key == "--seed") {
      args.seed = next().toULongLong();
    } else if (key == "--screenshot") {
      args.screenshotPath = next().toStdString();
    } else if (key == "--window-shot") {  // full window (toolbar + docks), for UI checks
      args.windowShotPath = next().toStdString();
    } else if (key == "--slice-z") {  // jump the slice focus before the screenshot
      args.sliceZ = next().toFloat();
      args.hasSliceZ = true;
    } else if (key == "--help" || key == "-h") {
      std::cout << "Usage: local_editor_qt [--trk in.trk]... [--volume vol.nii.gz]\n"
                << "                       [--label seg.nii.gz] [--odf odf.nii.gz]\n"
                << "                       [--peaks peaks.nii.gz] [--open any.{trk,nii,nii.gz}]\n"
                << "                       [--display-n N] [--disp-step N] [--seed N]\n"
                << "                       [--screenshot out.png]\n"
                << "--trk is repeatable (blend several tractograms). --open auto-detects\n"
                << "the file type. All optional; open/save files from the File menu too.\n";
      std::exit(0);
    }
  }
  // Clamp to a sane floor without re-spelling the defaults (those live in Args).
  args.displayN = std::max(1, args.displayN);
  args.dispStep = std::max(1, args.dispStep);
  return trks;
}

}  // namespace

int main(int argc, char** argv) {
  // The viewport is a QRhiWidget (Metal on macOS); it configures the graphics
  // API on the widget itself, so no QSurfaceFormat / GL-context request here.
  QApplication app(argc, argv);
  tracto::ApplyTheme(app);  // dark "pro" theme: Fusion base + palette + QSS

  tracto::Args args;
  const QStringList trks = ParseQtArgs(app.arguments(), args);

  tracto::MainWindow window(args);
  window.resize(1280, 900);
  window.show();
  for (const QString& trk : trks) {
    window.LoadTractogram(trk);  // repeatable --trk: each is its own (blended) layer
  }
  if (!args.volumePath.empty()) {
    window.LoadVolume(QString::fromStdString(args.volumePath));
  }
  if (!args.labelPath.empty()) {
    window.LoadLabel(QString::fromStdString(args.labelPath));
  }
  if (!args.odfPath.empty()) {
    window.LoadOdf(QString::fromStdString(args.odfPath));
  }
  if (!args.discreteOdfPath.empty()) {
    window.LoadDiscreteOdf(QString::fromStdString(args.discreteOdfPath));
  }
  if (!args.peaksPath.empty()) {
    window.LoadPeaks(QString::fromStdString(args.peaksPath));
  }
  if (!args.openPath.empty()) {
    window.DetectAndLoad(QString::fromStdString(args.openPath));  // unified auto-detect
  }

  // Headless edit smoke test: delete the box and check the alive counts (CPU
  // only — no event loop needed), then exit with the verdict.
  if (app.arguments().contains("--selftest-edit")) {
    return window.RunEditSelfTest() ? 0 : 2;
  }

  // Headless verification: after the first frames settle, grab the viewport (and/or
  // the whole window) and exit. Lets us confirm rendering/UI without a human watching.
  if (!args.screenshotPath.empty() || !args.windowShotPath.empty()) {
    const QString shot = QString::fromStdString(args.screenshotPath);
    const QString winShot = QString::fromStdString(args.windowShotPath);
    const bool hasSliceZ = args.hasSliceZ;
    const float sliceZ = args.sliceZ;
    QTimer::singleShot(600, &window, [&window, shot, winShot, hasSliceZ, sliceZ]() {
      if (hasSliceZ) window.DebugScrubToZ(sliceZ);  // jump the slice, then grab
      if (!shot.isEmpty()) {
        const bool ok = window.SaveScreenshot(shot);
        std::cout << (ok ? "screenshot -> " : "screenshot FAILED -> ") << shot.toStdString() << "\n";
      }
      if (!winShot.isEmpty()) {
        const bool ok = window.SaveWindowShot(winShot);
        std::cout << (ok ? "window-shot -> " : "window-shot FAILED -> ") << winShot.toStdString() << "\n";
      }
      QApplication::quit();
    });
  }
  return app.exec();
}
