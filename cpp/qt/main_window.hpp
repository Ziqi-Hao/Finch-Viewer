#pragma once

// Qt main window: owns the authoritative tractogram + full-set alive mask,
// hosts the viewport, and drives load / (re)build display / save. Editing
// semantics live here (not in the viewport), per the module boundary rules.

#include "args.hpp"
#include "bounds.hpp"
#include "selection_backend.hpp"
#include "tractogram_store.hpp"

#include <QMainWindow>
#include <QString>

#include <cstdint>
#include <memory>
#include <vector>

namespace tracto {

class TractViewport;
class ViewportHud;
class PropertiesPanel;
class ScenePanel;

class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  explicit MainWindow(Args args, QWidget* parent = nullptr);

  // Load files now (used by main() for --trk / --volume on the command line).
  void LoadTractogram(const QString& path);
  void LoadVolume(const QString& path);

  // Render the viewport offscreen and write it to a PNG (for headless verify).
  bool SaveScreenshot(const QString& path);

  // Headless edit smoke test: delete the viewport's current box and report the
  // alive counts (verifies the box -> selection -> aliveFull pipeline).
  bool RunEditSelfTest();

 private slots:
  void OpenTrk();
  void OpenVolume();
  void SaveAs();
  void ToggleVolume();
  void DeleteInBox();   // delete alive streamlines passing through the box
  void KeepInBox();     // keep only those; delete the rest
  void Undo();          // restore the previous alive mask
  void PreviewBox();    // report how many alive streamlines the box holds
  void ResetBox();      // re-place the box inside the data

 private:
  void RebuildDisplay();   // full-set alive mask -> sampled GPU line buffer
  void UpdateStatus();
  void RefreshStats();             // compute BasicStats -> Properties panel
  void PollSelectionReadout();     // mirror the viewport box into the panel when it moves
  void PushHistory();      // snapshot aliveFull_ for undo (bounded depth)
  std::size_t CountInBox(const std::vector<uint8_t>& inBox) const;  // alive & in box
  // Modal dialog when interactive; stderr in headless --screenshot runs (a modal
  // before app.exec() would hang the process with no one to dismiss it).
  void ReportError(const QString& title, const QString& message);

  Args args_;
  TractogramStore store_;
  std::vector<uint8_t> aliveFull_;  // authoritative per-streamline survive flag
  std::vector<std::vector<uint8_t>> history_;        // undo snapshots of aliveFull_
  std::unique_ptr<SelectionBackend> selection_;      // box -> per-streamline in-box
  Bounds tractRasBounds_;           // cached RAS AABB of the loaded streamlines
  bool hasTracts_ = false;
  TractViewport* viewport_ = nullptr;
  ViewportHud* hud_ = nullptr;            // translucent counts overlay over the viewport
  PropertiesPanel* properties_ = nullptr; // right-dock inspector
  ScenePanel* scene_ = nullptr;           // left-dock Scene / Layers list
  QString volumeName_;                    // loaded volume's display name + info (for the
  QString volumeInfo_;                    //   Scene panel; cached so toggles needn't recompute)
  Bounds lastBox_{};                      // last box mirrored into the panel (change detection)
  int boxStableTicks_ = 0;                // debounce: scan only after the box stops moving
};

}  // namespace tracto
