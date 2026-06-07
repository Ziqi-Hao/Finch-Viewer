#include "main_window.hpp"

#include "display_geometry.hpp"
#include "nifti_io.hpp"
#include "properties_panel.hpp"
#include "layers_panel.hpp"
#include "selection_backend.hpp"
#include "statistics.hpp"
#include "tract_viewport.hpp"
#include "perf_overlay.hpp"
#include "trk_io.hpp"
#include "utils.hpp"
#include "viewport_hud.hpp"

#include <QAction>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QImage>
#include <QMenuBar>
#include <QMessageBox>
#include <QScrollArea>
#include <QSignalBlocker>
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

  selection_ = CreateGridSelectionBackend();  // O(box) localized queries; built per load

  viewport_ = new TractViewport(this);
  setCentralWidget(viewport_);
  // Drive the live white in-box highlight: the viewport queries the grid backend
  // (fast, localized) as the box moves and lights up the streamlines inside it.
  viewport_->SetSelectionQuery(
      [this](const Bounds& box) { return selection_->SelectInBox(store_, box); });

  // Translucent counts overlay, top-left, over the viewport. Transparent to the
  // mouse so it never eats rotate/pan drags; fixed corner needs no reposition.
  hud_ = new ViewportHud(viewport_);
  hud_->move(12, 12);
  // Self-contained FPS/CPU/GPU diagnostics, top-right (parents to the viewport).
  new PerfOverlay(viewport_);

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
  QAction* openLabelAct = act("Open Label…", "Load a NIfTI label / segmentation", &MainWindow::OpenLabel);
  QAction* saveAct = act("Save As…", "Write the surviving streamlines to .trk",
                         &MainWindow::SaveAs, QKeySequence::Save);
  QAction* deleteAct = act("Delete", "Delete kept streamlines inside the box (D)",
                           &MainWindow::DeleteInBox, Qt::Key_D);
  QAction* keepAct = act("Keep", "Keep only streamlines inside the box (K)",
                         &MainWindow::KeepInBox, Qt::Key_K);
  QAction* undoAct = act("Undo", "Undo the last edit (U)", &MainWindow::Undo, Qt::Key_U);
  QAction* resetBoxAct = act("Reset Box", "Re-place the selection box in the data (B)",
                             &MainWindow::ResetBox, Qt::Key_B);
  QAction* toggleVolumeAct = act("Volume", "Toggle the volume slice planes (H)",
                             &MainWindow::ToggleVolume, Qt::Key_H);  // single source for H

  // Edit is an opt-in mode (View is the default core experience). Toggling it on
  // reveals the selection box + edit tools/cards; off returns to a clean view.
  editAct_ = new QAction("Edit", this);
  editAct_->setCheckable(true);
  editAct_->setToolTip("Edit mode: show the selection box + edit tools (E)");
  editAct_->setShortcut(Qt::Key_E);
  connect(editAct_, &QAction::toggled, this, &MainWindow::SetEditMode);
  editTools_ = {deleteAct, keepAct, undoAct, resetBoxAct};

  QMenu* fileMenu = menuBar()->addMenu("&File");
  fileMenu->addAction(openTrkAct);
  fileMenu->addAction(openVolumeAct);
  fileMenu->addAction(openLabelAct);
  fileMenu->addSeparator();
  fileMenu->addAction(saveAct);
  fileMenu->addSeparator();
  fileMenu->addAction("&Quit", this, &QWidget::close);

  QMenu* editMenu = menuBar()->addMenu("&Edit");
  editMenu->addAction(deleteAct);
  editMenu->addAction(keepAct);
  editMenu->addAction(undoAct);
  editMenu->addSeparator();
  editMenu->addAction(resetBoxAct);

  QMenu* viewMenu = menuBar()->addMenu("&View");
  viewMenu->addAction(toggleVolumeAct);
  viewMenu->addSeparator();
  viewMenu->addAction(editAct_);

  // Compact, always-visible command strip mirroring the actions.
  QToolBar* toolbar = addToolBar("Main");
  toolbar->setMovable(false);
  toolbar->setFloatable(false);
  toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
  toolbar->addAction(openTrkAct);
  toolbar->addAction(openVolumeAct);
  toolbar->addAction(openLabelAct);
  toolbar->addAction(saveAct);
  toolbar->addSeparator();
  // Volume visibility now lives per-layer in the Layers panel, so the toolbar
  // toggle is redundant — keep it only as the View-menu item + H shortcut.
  toolbar->addAction(editAct_);  // the View/Edit toggle
  toolbar->addSeparator();
  toolbar->addAction(deleteAct);
  toolbar->addAction(keepAct);
  toolbar->addAction(undoAct);
  toolbar->addAction(resetBoxAct);
  if (QWidget* deleteButton = toolbar->widgetForAction(deleteAct))
    deleteButton->setObjectName("dangerButton");  // red-on-hover (destructive)

  // Right-dock inspector. Its edit buttons reuse the same QActions as the
  // toolbar; density/step changes route back here to rebuild the display.
  const EditActions editActions{deleteAct, keepAct, undoAct, resetBoxAct};
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
  connect(properties_, &PropertiesPanel::contrastRangeChanged, this, [this](double lo, double hi) {
    for (VolumeLayer& vl : volumes_) {
      if (vl.id != activeVolumeId_) continue;
      vl.winLo = static_cast<float>(lo);  // remember per-volume
      vl.winHi = static_cast<float>(hi);
      break;
    }
    if (viewport_) viewport_->SetVolumeRange(static_cast<float>(lo), static_cast<float>(hi));
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

  // Left-dock Layers list (Volume / Tracts / Label; load many, check to show).
  layers_ = new LayersPanel;
  connect(layers_, &LayersPanel::visibilityChanged, this, &MainWindow::OnLayerVisibility);
  auto* layersDock = new QDockWidget("Layers", this);
  layersDock->setWidget(layers_);
  layersDock->setFeatures(QDockWidget::NoDockWidgetFeatures);
  layersDock->setMinimumWidth(200);
  addDockWidget(Qt::LeftDockWidgetArea, layersDock);

  // The viewport owns the box and exposes no change signal, so poll it a few
  // times a second and mirror it into the panel only when it actually moved.
  auto* boxTimer = new QTimer(this);
  boxTimer->setInterval(100);
  connect(boxTimer, &QTimer::timeout, this, &MainWindow::PollSelectionReadout);
  boxTimer->start();

  SetEditMode(false);  // start in View: edit tools disabled, Selection/Edit cards hidden
  statusBar()->showMessage("No tractogram loaded — File ▸ Open TRK…");
}

void MainWindow::SetEditMode(bool on) {
  editMode_ = on;
  if (viewport_) viewport_->SetEditMode(on);
  if (properties_) properties_->SetEditMode(on);
  if (hud_) hud_->setVisible(on);  // the kept/total overlay is shown only while editing
  for (QAction* a : editTools_) {  // delete/keep/undo/reset: hidden AND disabled in view
    a->setVisible(on);             // hide the toolbar/menu buttons in view mode
    a->setEnabled(on);             // disabled => their shortcuts (D/K/U/B) don't fire either
  }
  if (editAct_ && editAct_->isChecked() != on) {     // keep the toggle in sync if set in code
    QSignalBlocker block(editAct_);
    editAct_->setChecked(on);
  }
  UpdateStatus();  // swap the status line between the view hint and the edit count
}

void MainWindow::LoadTractogram(const QString& path) {
  try {
    // Build a fresh bundle (load many TRKs; each is its own layer row).
    TractBundle b;
    b.store.streamlines = LoadTrk(path.toStdString(), b.store.header);
    BuildSoA(b.store);
    b.alive.assign(b.store.StreamlineCount(), 1);
    b.rasBounds = RasBounds(b.store);
    b.name = QFileInfo(path).fileName();
    b.path = path;
    const Bounds& rb = b.rasBounds;
    std::printf("loaded %s streamlines · RAS x[%.1f,%.1f] y[%.1f,%.1f] z[%.1f,%.1f]\n",
                FormatCount(b.store.StreamlineCount()).c_str(),
                rb.v[0], rb.v[1], rb.v[2], rb.v[3], rb.v[4], rb.v[5]);
    std::fflush(stdout);

    if (layers_) {
      b.id = layers_->AddLayer(LayersPanel::Kind::Tracts, b.name, true);
      for (const TractBundle& other : tracts_) layers_->SetVisible(other.id, false);  // exclusive
    }
    tracts_.push_back(std::move(b));
    ActivateTracts(static_cast<int>(tracts_.size()) - 1);  // archives the previous active
    viewport_->SetTractsVisible(true);
    viewport_->ResetCamera();
  } catch (const std::exception& e) {
    ReportError("Load failed", e.what());
  }
}

void MainWindow::ActivateTracts(int index) {
  if (index < 0 || index >= static_cast<int>(tracts_.size()) || index == activeTracts_) return;
  // Archive the current active working state back into its bundle (move, no copy).
  if (activeTracts_ >= 0 && activeTracts_ < static_cast<int>(tracts_.size())) {
    TractBundle& cur = tracts_[activeTracts_];
    cur.store = std::move(store_);
    cur.alive = std::move(aliveFull_);
    cur.history = std::move(history_);
    cur.rasBounds = tractRasBounds_;
    cur.dirty = tractsDirty_;
  }
  // Swap the requested bundle into the active working members.
  TractBundle& nb = tracts_[index];
  store_ = std::move(nb.store);
  aliveFull_ = std::move(nb.alive);
  history_ = std::move(nb.history);
  tractRasBounds_ = nb.rasBounds;
  tractsDirty_ = nb.dirty;
  args_.trkPath = nb.path.toStdString();
  hasTracts_ = !store_.x.empty();
  activeTracts_ = index;
  selection_->Build(store_);  // rebuild the grid index for the now-active tractogram
  RebuildDisplay();
}

bool MainWindow::AnyTractsDirty() const {
  if (tractsDirty_) return true;  // the active one
  for (int i = 0; i < static_cast<int>(tracts_.size()); ++i)
    if (i != activeTracts_ && tracts_[i].dirty) return true;
  return false;
}

void MainWindow::OpenTrk() {
  const QString path =
      QFileDialog::getOpenFileName(this, "Open tractogram", QString(), "TrackVis (*.trk)");
  if (!path.isEmpty()) {
    LoadTractogram(path);
  }
}

void MainWindow::LoadVolume(const QString& path) { LoadVolumeLayer(path, false); }
void MainWindow::LoadLabel(const QString& path) { LoadVolumeLayer(path, true); }

void MainWindow::LoadVolumeLayer(const QString& path, bool isLabel) {
  // A label is also a scalar NIfTI; for now it renders through the same volume
  // path (grayscale). Per-label colouring is Stage 4. Both share the single
  // volume render slot, so checking one makes it the active rendered image.
  try {
    Volume volume = LoadNifti(path.toStdString());
    const Bounds wb = WorldBounds(volume);
    const int dx = volume.dims[0], dy = volume.dims[1], dz = volume.dims[2];
    std::printf("loaded %s %dx%dx%d · range[%.3f,%.3f] · RAS x[%.1f,%.1f] y[%.1f,%.1f] z[%.1f,%.1f]\n",
                isLabel ? "label" : "volume", dx, dy, dz, volume.valueMin, volume.valueMax,
                wb.v[0], wb.v[1], wb.v[2], wb.v[3], wb.v[4], wb.v[5]);
    std::fflush(stdout);

    args_.volumePath = path.toStdString();
    const QString name = QFileInfo(path).fileName();
    const float vmin = volume.valueMin, vmax = volume.valueMax;  // window default before move

    // Add a layer row in the right group; checking it makes it the active image,
    // so a fresh load auto-unchecks the other volume/label rows (single render).
    const auto kind = isLabel ? LayersPanel::Kind::Label : LayersPanel::Kind::Volume;
    const int id = layers_->AddLayer(kind, name, true);
    for (const VolumeLayer& other : volumes_) layers_->SetVisible(other.id, false);
    VolumeLayer layer{std::move(volume), name, id, isLabel, vmin, vmax};
    ComputeHistogram(layer);  // fills histBins + adaptive [dispMin,dispMax]; sets the window
    volumes_.push_back(std::move(layer));  // keep our copy (data + histogram + window)
    activeVolumeId_ = id;
    viewport_->SetVolume(volumes_.back().vol);  // copies into the viewport texture
    viewport_->SetVolumeVisible(true);
    UpdateInfo();
    UpdateHistogram();

    // Space sanity: an image in a different space from the tractogram lands
    // off-screen (the outputWarped vs SUBG08 mixup). Warn instead of confusing.
    const bool mismatch =
        hasTracts_ &&
        (wb.v[1] < tractRasBounds_.v[0] || wb.v[0] > tractRasBounds_.v[1] ||
         wb.v[3] < tractRasBounds_.v[2] || wb.v[2] > tractRasBounds_.v[3] ||
         wb.v[5] < tractRasBounds_.v[4] || wb.v[4] > tractRasBounds_.v[5]);
    if (mismatch) {
      statusBar()->showMessage(
          "⚠ Image does not overlap the streamlines — likely a different space.", 12000);
    } else {
      statusBar()->showMessage(
          QString("%1 loaded: %2×%3×%4").arg(isLabel ? "Label" : "Volume").arg(dx).arg(dy).arg(dz),
          8000);
    }
  } catch (const std::exception& e) {
    ReportError(isLabel ? "Label load failed" : "Volume load failed", e.what());
  }
}

void MainWindow::OpenVolume() {
  const QString path = QFileDialog::getOpenFileName(this, "Open volume", QString(),
                                                    "NIfTI (*.nii *.nii.gz)");
  if (!path.isEmpty()) LoadVolume(path);
}

void MainWindow::OpenLabel() {
  const QString path = QFileDialog::getOpenFileName(this, "Open label / segmentation", QString(),
                                                    "NIfTI (*.nii *.nii.gz)");
  if (!path.isEmpty()) LoadLabel(path);
}

void MainWindow::ToggleVolume() {
  if (activeVolumeId_ < 0) return;  // no volume to toggle
  const bool visible = !viewport_->VolumeVisible();
  viewport_->SetVolumeVisible(visible);
  if (layers_) layers_->SetVisible(activeVolumeId_, visible);  // keep the layer checkbox in sync
}

void MainWindow::OnLayerVisibility(int id, bool on) {
  // A volume layer: the viewport renders one volume at a time, so checking one
  // makes it active and unchecks the others; unchecking the active one hides it.
  for (const VolumeLayer& vl : volumes_) {
    if (vl.id != id) continue;
    if (on) {
      for (const VolumeLayer& other : volumes_)
        if (other.id != id) layers_->SetVisible(other.id, false);
      activeVolumeId_ = id;
      viewport_->SetVolume(vl.vol);  // copy into the viewport texture
      viewport_->SetVolumeVisible(true);
    } else if (activeVolumeId_ == id) {
      viewport_->SetVolumeVisible(false);
      activeVolumeId_ = -1;
    }
    UpdateInfo();
    UpdateHistogram();
    return;
  }
  // A tractogram layer: one renders at a time, so checking one activates it (and
  // unchecks the others); unchecking the active one hides the streamlines.
  for (int i = 0; i < static_cast<int>(tracts_.size()); ++i) {
    if (tracts_[i].id != id) continue;
    if (on) {
      for (int j = 0; j < static_cast<int>(tracts_.size()); ++j)
        if (j != i) layers_->SetVisible(tracts_[j].id, false);
      if (i != activeTracts_) {
        ActivateTracts(i);
        viewport_->ResetCamera();
      }
      viewport_->SetTractsVisible(true);
    } else if (i == activeTracts_) {
      viewport_->SetTractsVisible(false);
    }
    return;
  }
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
  tractsDirty_ = false;  // edits are now persisted
  statusBar()->showMessage(
      QString("Saved %1 streamlines → %2").arg(QString::fromStdString(FormatCount(kept)), path));
}

void MainWindow::closeEvent(QCloseEvent* event) {
  // Prompt before discarding unsaved edits. (Only the tractogram is editable for
  // now; MRI/label join this check once they become editable.)
  if (!AnyTractsDirty() || !args_.screenshotPath.empty()) {  // no modal in headless runs
    event->accept();
    return;
  }
  const auto choice = QMessageBox::warning(
      this, "Unsaved changes",
      "There are unsaved tractogram edits. Save before closing?",
      QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
  if (choice == QMessageBox::Discard) {
    event->accept();
  } else if (choice == QMessageBox::Save) {
    SaveAs();
    tractsDirty_ ? event->ignore() : event->accept();  // still dirty => save was cancelled
  } else {
    event->ignore();
  }
}

void MainWindow::RebuildDisplay() {
  LineGeometry geo = BuildDisplayLineGeometry(
      store_, aliveFull_, args_.displayN, args_.dispStep, args_.seed);
  const Bounds bounds = geo.bounds;
  viewport_->SetLineGeometry(std::move(geo.vertices), std::move(geo.spans), bounds);
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
  const std::size_t kept =
      static_cast<std::size_t>(std::count(aliveFull_.begin(), aliveFull_.end(), uint8_t{1}));
  // The kept/total count is an editing concern: show it only in Edit mode. View
  // mode keeps a clean navigation hint with no running count.
  if (editMode_) {
    statusBar()->showMessage(
        QString("kept %1 / %2   ·   cap %3 step %4   ·   drag handles · d delete · k keep · u undo · b reset-box")
            .arg(QString::fromStdString(FormatCount(kept)),
                 QString::fromStdString(FormatCount(store_.StreamlineCount())),
                 QString::number(args_.displayN),
                 QString::number(args_.dispStep)));
  } else {
    statusBar()->showMessage(
        "drag rotate · right-drag pan · wheel zoom · double-click pane to maximize · R recenter · E edit");
  }

  const QString name = args_.trkPath.empty()
                           ? QStringLiteral("untitled")
                           : QFileInfo(QString::fromStdString(args_.trkPath)).fileName();
  if (hud_) hud_->SetInfo(name, kept, store_.StreamlineCount());  // HUD is shown only in Edit

  if (properties_) {
    const int total = static_cast<int>(std::max<std::size_t>(1, store_.StreamlineCount()));
    properties_->SetDensity(args_.displayN, total);
    properties_->SetStep(args_.dispStep);
    // Force the next poll to recompute the in-box count against the new alive set
    // (the box may not have moved, but the edit changed what it holds).
    lastBox_.v[0] = std::nan("");
  }
  UpdateInfo();
}

void MainWindow::UpdateInfo() {
  if (!properties_) return;
  QString s;
  if (activeTracts_ >= 0 && activeTracts_ < static_cast<int>(tracts_.size())) {
    s += QStringLiteral("Tracts:  %1\n         %2 streamlines\n\n")
             .arg(tracts_[activeTracts_].name,
                  QString::fromStdString(FormatCount(store_.StreamlineCount())));
  }
  for (const VolumeLayer& vl : volumes_) {
    if (vl.id != activeVolumeId_) continue;
    s += QStringLiteral("%1:  %2\n         %3×%4×%5   [%6, %7]")
             .arg(vl.isLabel ? "Label" : "Volume", vl.name)
             .arg(vl.vol.dims[0]).arg(vl.vol.dims[1]).arg(vl.vol.dims[2])
             .arg(vl.vol.valueMin, 0, 'g', 3).arg(vl.vol.valueMax, 0, 'g', 3);
    break;
  }
  properties_->SetInfo(s.isEmpty() ? QStringLiteral("No data loaded.") : s.trimmed());
}

void MainWindow::ComputeHistogram(VolumeLayer& vl) {
  // Adaptive intensity axis: a few bright outliers shouldn't squash the bulk of
  // the data to the left, so cap the displayed max at the 99.5th percentile. The
  // default window uses that same robust range (FSLeyes-style auto contrast).
  const Volume& v = vl.vol;
  const float dmin = v.valueMin;
  const float dmax = (v.valueMax > v.valueMin) ? v.valueMax : v.valueMin + 1.0f;
  constexpr int kFine = 1024;
  std::vector<double> fine(kFine, 0.0);
  const double finv = static_cast<double>(kFine - 1) / (static_cast<double>(dmax) - dmin);
  for (float s : v.data) fine[std::clamp(static_cast<int>((static_cast<double>(s) - dmin) * finv),
                                         0, kFine - 1)] += 1.0;
  const double total = std::max(1.0, static_cast<double>(v.data.size()));
  double cum = 0.0;
  int robBin = kFine - 1;
  for (int i = 0; i < kFine; ++i) { cum += fine[i]; if (cum >= 0.995 * total) { robBin = i; break; } }

  vl.dispMin = dmin;
  vl.dispMax = dmin + static_cast<float>(robBin + 1) / kFine * (dmax - dmin);
  if (vl.dispMax <= vl.dispMin) vl.dispMax = dmax;
  vl.winLo = vl.dispMin;
  vl.winHi = vl.dispMax;

  // 160 display bins over [dispMin, dispMax], aggregated from the fine bins.
  constexpr int kDisp = 160;
  vl.histBins.assign(kDisp, 0.0f);
  const float span = std::max(1e-9f, vl.dispMax - vl.dispMin);
  for (int j = 0; j < kFine; ++j) {
    const double cj = dmin + (j + 0.5) / kFine * (static_cast<double>(dmax) - dmin);
    const int db = std::clamp(static_cast<int>((cj - vl.dispMin) / span * kDisp), 0, kDisp - 1);
    vl.histBins[db] += static_cast<float>(fine[j]);
  }
}

void MainWindow::UpdateHistogram() {
  if (!properties_) return;
  for (const VolumeLayer& vl : volumes_) {
    if (vl.id != activeVolumeId_) continue;
    properties_->SetHistogram(true, vl.histBins, vl.dispMin, vl.dispMax, vl.winLo, vl.winHi);
    if (viewport_) viewport_->SetVolumeRange(vl.winLo, vl.winHi);
    return;
  }
  properties_->SetHistogram(false, {}, 0.0, 0.0, 0.0, 0.0);  // no active volume
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
      QStringLiteral("kept    %1 / %2  (%3% deleted)\nlength  %4 mm\npts/ln  %5")
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
  tractsDirty_ = true;  // an edit is about to happen -> unsaved changes
}

void MainWindow::DeleteInBox() {
  if (!hasTracts_ || !viewport_->HasSelectionBox()) return;
  const std::vector<uint8_t> inBox = selection_->SelectInBox(store_, viewport_->SelectionBox());
  const std::size_t nKill = CountInBox(inBox);
  if (nKill == 0) {
    statusBar()->showMessage("Box holds no kept streamlines.", 4000);
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
    statusBar()->showMessage("Box holds no kept streamlines.", 4000);
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
  tractsDirty_ = true;  // state changed from the last save
  RebuildDisplay();
  statusBar()->showMessage("Undo.", 3000);
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
