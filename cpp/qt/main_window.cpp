#include "main_window.hpp"

#include "display_geometry.hpp"
#include "nifti_io.hpp"
#include "properties_panel.hpp"
#include "scene_panel.hpp"
#include "selection_backend.hpp"
#include "statistics.hpp"
#include "tract_viewport.hpp"
#include "trk_io.hpp"
#include "utils.hpp"
#include "viewport_hud.hpp"

#include <QAction>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QImage>
#include <QMenuBar>
#include <QMessageBox>
#include <QScrollArea>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>

#include <cmath>
#include <cstdio>

#include <algorithm>
#include <exception>
#include <utility>

namespace tracto {

MainWindow::MainWindow(Args args, QWidget* parent)
    : QMainWindow(parent), args_(std::move(args)) {
  setWindowTitle("Finch-Viewer — tractography editor (Qt/OpenGL)");

  selection_ = CreateCpuSelectionBackend();

  viewport_ = new TractViewport(this);
  setCentralWidget(viewport_);

  // Translucent counts overlay, top-left, over the viewport. Transparent to the
  // mouse so it never eats rotate/pan drags; fixed corner needs no reposition.
  hud_ = new ViewportHud(viewport_);
  hud_->move(12, 12);

  // One QAction per command, shared by the menu bar, the toolbar, and its
  // shortcut — Qt's single-source-of-truth idiom. Slots are defined below.
  auto act = [this](const QString& text, const QString& tip,
                    void (MainWindow::*slot)(), const QKeySequence& sc = {}) {
    auto* a = new QAction(text, this);
    a->setToolTip(tip);
    a->setStatusTip(tip);
    if (!sc.isEmpty()) a->setShortcut(sc);
    connect(a, &QAction::triggered, this, slot);
    return a;
  };

  QAction* openTrkAct = act("Open TRK…", "Open a .trk tractogram", &MainWindow::OpenTrk);
  QAction* openVolumeAct = act("Open Volume…", "Load a NIfTI background volume", &MainWindow::OpenVolume);
  QAction* saveAct = act("Save As…", "Write the surviving streamlines to .trk",
                         &MainWindow::SaveAs, QKeySequence::Save);
  QAction* deleteAct = act("Delete", "Delete alive streamlines inside the box (D)",
                           &MainWindow::DeleteInBox, Qt::Key_D);
  QAction* keepAct = act("Keep", "Keep only streamlines inside the box (K)",
                         &MainWindow::KeepInBox, Qt::Key_K);
  QAction* undoAct = act("Undo", "Undo the last edit (U)", &MainWindow::Undo, Qt::Key_U);
  QAction* resetBoxAct = act("Reset Box", "Re-place the selection box in the data (B)",
                             &MainWindow::ResetBox, Qt::Key_B);
  QAction* previewAct = act("Preview", "Report how many alive streamlines the box holds (P)",
                            &MainWindow::PreviewBox, Qt::Key_P);
  QAction* toggleVolumeAct = act("Volume", "Toggle the volume slice planes (H)",
                             &MainWindow::ToggleVolume, Qt::Key_H);  // single source for H

  QMenu* fileMenu = menuBar()->addMenu("&File");
  fileMenu->addAction(openTrkAct);
  fileMenu->addAction(openVolumeAct);
  fileMenu->addSeparator();
  fileMenu->addAction(saveAct);
  fileMenu->addSeparator();
  fileMenu->addAction("&Quit", this, &QWidget::close);

  QMenu* editMenu = menuBar()->addMenu("&Edit");
  editMenu->addAction(deleteAct);
  editMenu->addAction(keepAct);
  editMenu->addAction(undoAct);
  editMenu->addSeparator();
  editMenu->addAction(previewAct);
  editMenu->addAction(resetBoxAct);

  QMenu* viewMenu = menuBar()->addMenu("&View");
  viewMenu->addAction(toggleVolumeAct);

  // Compact, always-visible command strip mirroring the actions.
  QToolBar* toolbar = addToolBar("Main");
  toolbar->setMovable(false);
  toolbar->setFloatable(false);
  toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
  toolbar->addAction(openTrkAct);
  toolbar->addAction(openVolumeAct);
  toolbar->addAction(saveAct);
  toolbar->addSeparator();
  toolbar->addAction(deleteAct);
  toolbar->addAction(keepAct);
  toolbar->addAction(undoAct);
  toolbar->addAction(resetBoxAct);
  toolbar->addSeparator();
  toolbar->addAction(previewAct);
  toolbar->addAction(toggleVolumeAct);
  if (QWidget* deleteButton = toolbar->widgetForAction(deleteAct))
    deleteButton->setObjectName("dangerButton");  // red-on-hover (destructive)

  // Right-dock inspector. Its edit buttons reuse the same QActions as the
  // toolbar; density/step changes route back here to rebuild the display.
  const EditActions editActions{deleteAct, keepAct, undoAct, previewAct, resetBoxAct};
  properties_ = new PropertiesPanel(editActions);
  properties_->SetDensity(args_.displayN, args_.displayN);
  properties_->SetStep(args_.dispStep);
  connect(properties_, &PropertiesPanel::densityChanged, this, [this](int n) {
    args_.displayN = std::max(1, n);
    if (hasTracts_) RebuildDisplay();
  });
  connect(properties_, &PropertiesPanel::stepChanged, this, [this](int s) {
    args_.dispStep = std::max(1, s);
    if (hasTracts_) RebuildDisplay();
  });
  connect(properties_, &PropertiesPanel::refreshStatsRequested, this, &MainWindow::RefreshStats);
  connect(properties_, &PropertiesPanel::boxChanged, this, [this](const Bounds& box) {
    if (viewport_) viewport_->SetSelectionBox(box);  // typed bounds -> viewport box
  });

  auto* scroll = new QScrollArea;
  scroll->setWidget(properties_);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  auto* dock = new QDockWidget("Properties", this);
  dock->setWidget(scroll);
  dock->setFeatures(QDockWidget::NoDockWidgetFeatures);  // a fixed pro-tool panel
  dock->setMinimumWidth(248);
  addDockWidget(Qt::RightDockWidgetArea, dock);

  // Left-dock Scene / Layers list.
  scene_ = new ScenePanel;
  connect(scene_, &ScenePanel::volumeVisibilityChanged, this, [this](bool visible) {
    if (viewport_) viewport_->SetVolumeVisible(visible);
  });
  auto* sceneDock = new QDockWidget("Scene", this);
  sceneDock->setWidget(scene_);
  sceneDock->setFeatures(QDockWidget::NoDockWidgetFeatures);
  sceneDock->setMinimumWidth(200);
  addDockWidget(Qt::LeftDockWidgetArea, sceneDock);

  // The viewport owns the box and exposes no change signal, so poll it a few
  // times a second and mirror it into the panel only when it actually moved.
  auto* boxTimer = new QTimer(this);
  boxTimer->setInterval(100);
  connect(boxTimer, &QTimer::timeout, this, &MainWindow::PollSelectionReadout);
  boxTimer->start();

  statusBar()->showMessage("No tractogram loaded — File ▸ Open TRK…");
}

void MainWindow::LoadTractogram(const QString& path) {
  try {
    TractogramStore store;
    store.streamlines = LoadTrk(path.toStdString(), store.header);
    BuildSoA(store);
    store_ = std::move(store);
    aliveFull_.assign(store_.StreamlineCount(), 1);
    args_.trkPath = path.toStdString();
    tractRasBounds_ = RasBounds(store_);
    hasTracts_ = !store_.x.empty();
    if (hasTracts_) {
      const Bounds& b = tractRasBounds_;
      std::printf("loaded %s streamlines · RAS x[%.1f,%.1f] y[%.1f,%.1f] z[%.1f,%.1f]\n",
                  FormatCount(store_.StreamlineCount()).c_str(),
                  b.v[0], b.v[1], b.v[2], b.v[3], b.v[4], b.v[5]);
      std::fflush(stdout);
    }
    RebuildDisplay();
    viewport_->ResetCamera();
  } catch (const std::exception& e) {
    ReportError("Load failed", e.what());
  }
}

void MainWindow::OpenTrk() {
  const QString path =
      QFileDialog::getOpenFileName(this, "Open tractogram", QString(), "TrackVis (*.trk)");
  if (!path.isEmpty()) {
    LoadTractogram(path);
  }
}

void MainWindow::LoadVolume(const QString& path) {
  try {
    Volume volume = LoadNifti(path.toStdString());
    const Bounds wb = WorldBounds(volume);
    const int dx = volume.dims[0], dy = volume.dims[1], dz = volume.dims[2];
    std::printf("loaded volume %dx%dx%d · range[%.3f,%.3f] · RAS x[%.1f,%.1f] y[%.1f,%.1f] z[%.1f,%.1f]\n",
                dx, dy, dz, volume.valueMin, volume.valueMax,
                wb.v[0], wb.v[1], wb.v[2], wb.v[3], wb.v[4], wb.v[5]);
    std::fflush(stdout);

    args_.volumePath = path.toStdString();
    const float vmin = volume.valueMin, vmax = volume.valueMax;  // capture before move
    viewport_->SetVolume(std::move(volume));  // move: no multi-MB voxel copy

    volumeName_ = QFileInfo(path).fileName();
    volumeInfo_ = QStringLiteral("%1×%2×%3 · [%4, %5]")
                      .arg(dx).arg(dy).arg(dz).arg(vmin, 0, 'g', 3).arg(vmax, 0, 'g', 3);
    if (scene_) scene_->SetVolume(true, volumeName_, volumeInfo_, viewport_->VolumeVisible());

    // Space sanity: a volume in a different space from the tractogram lands
    // off-screen (the outputWarped vs SUBG08 mixup). Warn instead of confusing.
    // Timed messages auto-revert to the persistent alive/controls status line.
    const bool mismatch =
        hasTracts_ &&
        (wb.v[1] < tractRasBounds_.v[0] || wb.v[0] > tractRasBounds_.v[1] ||
         wb.v[3] < tractRasBounds_.v[2] || wb.v[2] > tractRasBounds_.v[3] ||
         wb.v[5] < tractRasBounds_.v[4] || wb.v[4] > tractRasBounds_.v[5]);
    if (mismatch) {
      statusBar()->showMessage(
          "⚠ Volume does not overlap the streamlines — likely a different space.", 12000);
    } else {
      statusBar()->showMessage(QString("Volume loaded: %1×%2×%3").arg(dx).arg(dy).arg(dz), 8000);
    }
  } catch (const std::exception& e) {
    ReportError("Volume load failed", e.what());
  }
}

void MainWindow::OpenVolume() {
  const QString path = QFileDialog::getOpenFileName(this, "Open volume", QString(),
                                                    "NIfTI (*.nii *.nii.gz)");
  if (!path.isEmpty()) {
    LoadVolume(path);
  }
}

void MainWindow::ToggleVolume() {
  const bool visible = !viewport_->VolumeVisible();
  viewport_->SetVolumeVisible(visible);
  if (scene_) scene_->SetVolumeVisibleState(visible);  // keep the Scene checkbox in sync
}

bool MainWindow::SaveScreenshot(const QString& path) {
  // Viewport (RHI) contents only (lines, cage, FA slices, box). The HUD/panel are
  // separate QWidgets and are NOT in this image — use window()->grab() for a full
  // capture. QRhiWidget::grabFramebuffer() renders a frame offscreen and returns it.
  const QImage image = viewport_->grabFramebuffer();
  return image.save(path);
}

void MainWindow::SaveAs() {
  if (store_.Empty()) {
    QMessageBox::information(this, "Nothing to save", "Load a tractogram first.");
    return;
  }
  const QString path =
      QFileDialog::getSaveFileName(this, "Save surviving streamlines", QString(), "TrackVis (*.trk)");
  if (path.isEmpty()) {
    return;
  }
  // Save is exact: it writes the full-set survivors, not the sampled display.
  if (!WriteTrkSubset(path.toStdString(), store_.header, store_.streamlines, aliveFull_)) {
    QMessageBox::critical(this, "Save failed", "Could not write the output .trk.");
    return;
  }
  const std::size_t kept =
      static_cast<std::size_t>(std::count(aliveFull_.begin(), aliveFull_.end(), uint8_t{1}));
  statusBar()->showMessage(
      QString("Saved %1 streamlines → %2").arg(QString::fromStdString(FormatCount(kept)), path));
}

void MainWindow::RebuildDisplay() {
  LineGeometry geo = BuildDisplayLineGeometry(
      store_, aliveFull_, args_.displayN, args_.dispStep, args_.seed);
  const Bounds bounds = geo.bounds;
  viewport_->SetLineGeometry(std::move(geo.vertices), bounds);  // move: no multi-MB copy
  UpdateStatus();
}

void MainWindow::ReportError(const QString& title, const QString& message) {
  if (args_.screenshotPath.empty()) {
    QMessageBox::critical(this, title, message);
  } else {
    std::fprintf(stderr, "%s: %s\n", title.toStdString().c_str(), message.toStdString().c_str());
  }
}

void MainWindow::UpdateStatus() {
  const std::size_t alive =
      static_cast<std::size_t>(std::count(aliveFull_.begin(), aliveFull_.end(), uint8_t{1}));
  statusBar()->showMessage(
      QString("alive %1 / %2   ·   cap %3 step %4   ·   drag box handles · d=delete k=keep u=undo p=preview b=reset-box · R=recenter H=volume")
          .arg(QString::fromStdString(FormatCount(alive)),
               QString::fromStdString(FormatCount(store_.StreamlineCount())),
               QString::number(args_.displayN),
               QString::number(args_.dispStep)));

  const QString name = args_.trkPath.empty()
                           ? QStringLiteral("untitled")
                           : QFileInfo(QString::fromStdString(args_.trkPath)).fileName();
  if (hud_) hud_->SetInfo(name, alive, store_.StreamlineCount());
  if (scene_) scene_->SetTractogram(name, alive, store_.StreamlineCount());

  if (properties_) {
    const int total = static_cast<int>(std::max<std::size_t>(1, store_.StreamlineCount()));
    properties_->SetDensity(args_.displayN, total);
    properties_->SetStep(args_.dispStep);
    // Force the next poll to recompute the in-box count against the new alive set
    // (the box may not have moved, but the edit changed what it holds).
    lastBox_.v[0] = std::nan("");
  }
}

void MainWindow::RefreshStats() {
  if (!properties_) return;
  if (!hasTracts_) {
    properties_->SetStats("Load a tractogram first.");
    return;
  }
  const BasicStats s = ComputeBasicStats(store_, aliveFull_);
  auto summary = [](const NumericSummary& n) {
    return n.valid ? QStringLiteral("%1 ± %2  [%3, %4]")
                         .arg(n.mean, 0, 'f', 1)
                         .arg(n.stddev, 0, 'f', 1)
                         .arg(n.min, 0, 'f', 1)
                         .arg(n.max, 0, 'f', 1)
                   : QStringLiteral("—");
  };
  properties_->SetStats(
      QStringLiteral("alive   %1 / %2  (%3% deleted)\nlength  %4 mm\npts/ln  %5")
          .arg(QString::fromStdString(FormatCount(s.aliveCount)),
               QString::fromStdString(FormatCount(s.fullCount)))
          .arg(s.deletedPercent, 0, 'f', 1)
          .arg(summary(s.lengthMm))
          .arg(summary(s.pointsPerLine)));
}

void MainWindow::PollSelectionReadout() {
  if (!properties_) return;
  if (!hasTracts_ || viewport_ == nullptr || !viewport_->HasSelectionBox()) {
    properties_->SetBox(false, Bounds{}, 0, 0.0);
    return;
  }
  const Bounds box = viewport_->SelectionBox();
  bool changed = false;
  for (int i = 0; i < 6; ++i)
    if (box.v[i] != lastBox_.v[i]) { changed = true; break; }
  if (changed) {
    // Still moving (a drag mutates the box every tick) — record it but defer the
    // O(all-points) selection scan until the box settles, so dragging doesn't run
    // a full-set scan 10x/second on the GUI thread.
    lastBox_ = box;
    boxStableTicks_ = 0;
    return;
  }
  if (boxStableTicks_ >= 1) return;  // already mirrored this resting box
  boxStableTicks_ = 1;

  const std::size_t inBox = CountInBox(selection_->SelectInBox(store_, box));
  const std::size_t alive = std::max<std::size_t>(
      1, static_cast<std::size_t>(std::count(aliveFull_.begin(), aliveFull_.end(), uint8_t{1})));
  const double pct = 100.0 * static_cast<double>(inBox) / static_cast<double>(alive);
  properties_->SetBox(true, box, inBox, pct);
}

std::size_t MainWindow::CountInBox(const std::vector<uint8_t>& inBox) const {
  std::size_t n = 0;
  for (std::size_t i = 0; i < inBox.size(); ++i)
    if (inBox[i] && aliveFull_[i]) ++n;
  return n;
}

void MainWindow::PushHistory() {
  constexpr std::size_t kMaxUndo = 50;
  history_.push_back(aliveFull_);
  if (history_.size() > kMaxUndo) history_.erase(history_.begin());
}

void MainWindow::DeleteInBox() {
  if (!hasTracts_ || !viewport_->HasSelectionBox()) return;
  const std::vector<uint8_t> inBox = selection_->SelectInBox(store_, viewport_->SelectionBox());
  const std::size_t nKill = CountInBox(inBox);
  if (nKill == 0) {
    statusBar()->showMessage("Box holds no alive streamlines.", 4000);
    return;
  }
  PushHistory();
  for (std::size_t i = 0; i < inBox.size(); ++i)
    if (inBox[i]) aliveFull_[i] = 0;
  RebuildDisplay();
  statusBar()->showMessage(
      QString("Deleted %1 streamlines.").arg(QString::fromStdString(FormatCount(nKill))), 4000);
}

void MainWindow::KeepInBox() {
  if (!hasTracts_ || !viewport_->HasSelectionBox()) return;
  const std::vector<uint8_t> inBox = selection_->SelectInBox(store_, viewport_->SelectionBox());
  const std::size_t nKeep = CountInBox(inBox);
  if (nKeep == 0) {
    statusBar()->showMessage("Box holds no alive streamlines.", 4000);
    return;
  }
  PushHistory();
  for (std::size_t i = 0; i < inBox.size(); ++i)
    aliveFull_[i] = static_cast<uint8_t>(aliveFull_[i] && inBox[i]);
  RebuildDisplay();
  statusBar()->showMessage(
      QString("Kept %1 streamlines.").arg(QString::fromStdString(FormatCount(nKeep))), 4000);
}

void MainWindow::Undo() {
  if (history_.empty()) {
    statusBar()->showMessage("Nothing to undo.", 3000);
    return;
  }
  aliveFull_ = std::move(history_.back());
  history_.pop_back();
  RebuildDisplay();
  statusBar()->showMessage("Undo.", 3000);
}

void MainWindow::PreviewBox() {
  if (!hasTracts_ || !viewport_->HasSelectionBox()) return;
  const std::vector<uint8_t> inBox = selection_->SelectInBox(store_, viewport_->SelectionBox());
  const std::size_t n = CountInBox(inBox);
  const std::size_t alive = std::max<std::size_t>(
      1, static_cast<std::size_t>(std::count(aliveFull_.begin(), aliveFull_.end(), uint8_t{1})));
  const double pct = 100.0 * static_cast<double>(n) / static_cast<double>(alive);
  statusBar()->showMessage(
      QString("Box holds %1 alive streamlines (%2%) — d deletes them, k keeps only them.")
          .arg(QString::fromStdString(FormatCount(n)))
          .arg(pct, 0, 'f', 1),
      6000);
}

void MainWindow::ResetBox() {
  viewport_->ResetSelectionBox();
  statusBar()->showMessage("Selection box reset.", 3000);
}

bool MainWindow::RunEditSelfTest() {
  if (!hasTracts_) {
    std::printf("selftest-edit: no tractogram\n");
    return false;
  }
  const std::size_t total = store_.StreamlineCount();
  const Bounds& b = tractRasBounds_;
  auto countSel = [&](const Bounds& box) {
    const auto f = selection_->SelectInBox(store_, box);
    return static_cast<std::size_t>(std::count(f.begin(), f.end(), uint8_t{1}));
  };

  // Geometry-agnostic checks (no assumption about where streamlines sit):
  //   inclusion — the exact data AABB selects every streamline;
  //   exclusion — a box translated far outside the data selects none.
  const std::size_t nFull = countSel(b);
  Bounds outside = b;
  const double shift = (b.v[1] - b.v[0]) + 1000.0;
  outside.v[0] += shift;
  outside.v[1] += shift;
  const std::size_t nOut = countSel(outside);

  // Strict subset + count equivalence: delete the left half (x <= centre);
  // alive must drop by exactly the alive-in-box count.
  Bounds half = b;
  half.v[1] = 0.5 * (b.v[0] + b.v[1]);
  const std::vector<uint8_t> inHalf = selection_->SelectInBox(store_, half);
  const std::size_t nHalf = CountInBox(inHalf);
  const std::vector<uint8_t> saved = aliveFull_;
  for (std::size_t i = 0; i < inHalf.size(); ++i)
    if (inHalf[i]) aliveFull_[i] = 0;
  const std::size_t after =
      static_cast<std::size_t>(std::count(aliveFull_.begin(), aliveFull_.end(), uint8_t{1}));
  aliveFull_ = saved;  // restore (no display rebuild needed)

  const bool ok = (nFull == total) && (nOut == 0) && (after == total - nHalf);
  std::printf(
      "selftest-edit: total=%zu · full-box=%zu(==total? %s) · outside-box=%zu(==0? %s) · "
      "left-half in=%zu, delete->%zu (countOk %s) — %s\n",
      total, nFull, nFull == total ? "y" : "N", nOut, nOut == 0 ? "y" : "N", nHalf, after,
      (after == total - nHalf) ? "y" : "N", ok ? "OK" : "FAIL");
  std::fflush(stdout);
  return ok;
}

}  // namespace tracto
