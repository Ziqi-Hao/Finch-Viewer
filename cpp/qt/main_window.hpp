#pragma once

// Qt main window: owns the authoritative tractogram + full-set alive mask,
// hosts the viewport, and drives load / (re)build display / save. Editing
// semantics live here (not in the viewport), per the module boundary rules.

#include "args.hpp"
#include "bounds.hpp"
#include "nifti_io.hpp"   // Volume (stored per loaded volume layer)
#include "selection_backend.hpp"
#include "tractogram_store.hpp"

#include <QMainWindow>
#include <QString>

#include <cstdint>
#include <memory>
#include <vector>

class QAction;  // global-namespace Qt type (member pointers only)

namespace tracto {

class TractViewport;
class ViewportHud;
class PropertiesPanel;
class LayersPanel;

class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  explicit MainWindow(Args args, QWidget* parent = nullptr);

  // Load files now (used by main() for --trk / --volume on the command line).
  void LoadTractogram(const QString& path);
  void LoadVolume(const QString& path);
  void LoadLabel(const QString& path);

  // Render the viewport offscreen and write it to a PNG (for headless verify).
  bool SaveScreenshot(const QString& path);

  // Headless edit smoke test: delete the viewport's current box and report the
  // alive counts (verifies the box -> selection -> aliveFull pipeline).
  bool RunEditSelfTest();

 protected:
  void closeEvent(QCloseEvent* event) override;  // prompt if there are unsaved edits

 private slots:
  void OpenTrk();
  void OpenVolume();
  void OpenLabel();
  void SaveAs();
  void ToggleVolume();
  void DeleteInBox();   // delete alive streamlines passing through the box
  void KeepInBox();     // keep only those; delete the rest
  void Undo();          // restore the previous kept mask
  void ResetBox();      // re-place the box inside the data

 private:
  // A loaded volume/label layer (kept so it can be re-activated + its histogram
  // reused). Declared before the methods that take it by reference.
  struct VolumeLayer {
    Volume vol; QString name; int id = 0; bool isLabel = false;
    float winLo = 0.0f, winHi = 1.0f;   // grayscale window (contrast), per volume
    float opacity = 1.0f;               // layer opacity (0..1)
    std::vector<float> histBins;        // precomputed display histogram
    float dispMin = 0.0f, dispMax = 1.0f;  // adaptive intensity axis (robust max)
    std::vector<float> lut;             // label mode: RGBA per label index
    int lutWidth = 0;                   // label mode: number of label entries
  };

  void RebuildDisplay();   // active full-set alive mask -> sampled GPU line buffer
  void RebuildTractOverlays();  // visible non-active bundles -> read-only viewport overlays
  void RebuildDensityMap();     // active tractogram -> track-density heatmap in the 2-D panes
  void UpdateStatus();
  void RefreshStats();             // compute BasicStats -> Properties panel
  void PollSelectionReadout();     // mirror the viewport box into the panel when it moves
  void SetEditMode(bool on);       // View (default) <-> Edit: gates box + edit tools/cards
  void OnLayerVisibility(int id, bool on);  // a layer checkbox toggled in the Layers panel
  void OnLayerOpacity(int id, double opacity);  // a layer opacity slider moved
  void ActivateTracts(int index);           // archive the active TRK, swap in tracts_[index]
  bool AnyTractsDirty() const;              // any loaded tractogram with unsaved edits
  void LoadVolumeLayer(const QString& path, bool isLabel);  // shared volume/label loader
  void UpdateInfo();                        // refresh the Properties basic-info readout
  void UpdateHistogram();                   // active volume -> Contrast histogram + window
  void ComputeHistogram(VolumeLayer& vl);   // bins + adaptive (robust) intensity range
  void ComputeLabelLut(VolumeLayer& vl);    // integer label -> per-label RGBA colour table
  void PushHistory();      // snapshot aliveFull_ for undo (bounded depth)
  std::size_t CountInBox(const std::vector<uint8_t>& inBox) const;  // alive & in box
  // Modal dialog when interactive; stderr in headless --screenshot runs (a modal
  // before app.exec() would hang the process with no one to dismiss it).
  void ReportError(const QString& title, const QString& message);

  Args args_;
  // ── Active tractogram working state (the rendered/editable one) ────────────
  // These mirror tracts_[activeTracts_]; ActivateTracts() archives them back into
  // the bundle and swaps a different bundle in, so all the edit code below keeps
  // using store_/aliveFull_/… unchanged while supporting many loaded TRKs.
  TractogramStore store_;
  std::vector<uint8_t> aliveFull_;  // authoritative per-streamline survive flag
  std::vector<std::vector<uint8_t>> history_;        // undo snapshots of aliveFull_
  std::unique_ptr<SelectionBackend> selection_;      // box -> per-streamline in-box (one grid,
                                                     // rebuilt on each tractogram switch)
  Bounds tractRasBounds_;           // cached RAS AABB of the loaded streamlines
  bool hasTracts_ = false;
  bool tractsDirty_ = false;        // unsaved edits to the active tractogram

  // Loaded tractograms (load many; render several at once. One is "active" —
  // editable, archive-swapped into store_/aliveFull_; the rest are read-only
  // overlays in the viewport).
  struct TractBundle {
    TractogramStore store;
    std::vector<uint8_t> alive;
    std::vector<std::vector<uint8_t>> history;
    Bounds rasBounds;
    QString name;   // display name (filename)
    QString path;   // source path (for the HUD + save default)
    int id = 0;     // Layers-panel row id
    bool dirty = false;
    bool visible = true;  // checkbox state (drawn = active lines or an overlay)
    // Cached display geometry for when this bundle is a non-active overlay: built
    // when it's archived (from its full SoA), so its SoA can then be slimmed and
    // the overlay still draws without a rebuild. Empty while active or hidden.
    std::vector<float> overlayVerts;
    // Track-density map, computed ONCE from the full original streamlines at load
    // and reused thereafter (it does not track edits or the display-density slider).
    Volume densityMap;
  };
  std::vector<TractBundle> tracts_;
  int activeTracts_ = -1;
  // Reserved viewport image id for the auto-generated track-density map (the 2-D
  // slice panes show it instead of the streamlines). Negative so it can't collide
  // with a Layers-panel row id.
  static constexpr int kDensityLayerId = -1000;
  TractViewport* viewport_ = nullptr;
  ViewportHud* hud_ = nullptr;            // translucent counts overlay over the viewport
  PropertiesPanel* properties_ = nullptr; // right-dock inspector
  LayersPanel* layers_ = nullptr;         // left-dock Layers list (Volume/Tracts/Label)
  bool editMode_ = false;                 // false = View (the default core experience)
  QAction* editAct_ = nullptr;            // checkable View/Edit toggle
  std::vector<QAction*> editTools_;        // actions enabled only in edit mode

  // Loaded volume/label layers (Freeview/FSLeyes-style: load many, blend the
  // visible ones). Every checked layer renders; `selectedVolumeId_` is just the
  // one whose histogram/window the Contrast panel currently edits.
  std::vector<VolumeLayer> volumes_;
  int selectedVolumeId_ = -1;

  Bounds lastBox_{};                      // last box mirrored into the panel (change detection)
  int boxStableTicks_ = 0;                // debounce: scan only after the box stops moving
};

}  // namespace tracto
