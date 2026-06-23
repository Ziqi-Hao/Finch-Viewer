#pragma once

// Right-dock inspector: Display (density / step), Selection (box readout), Edit
// (buttons that reuse the toolbar actions), and Statistics. A passive view — it
// emits intent signals and is refreshed via setters; all display/edit logic
// stays in MainWindow, per the module boundary in CLAUDE.md.

#include "bounds.hpp"

#include <QWidget>

#include <vector>

class QAction;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QSlider;
class QSpinBox;

namespace tracto {

class HistogramWidget;

// The edit commands, shared with the menu/toolbar so the panel buttons are the
// same single-source-of-truth QActions (enabled state etc. stays in sync).
struct EditActions {
  QAction* del = nullptr;
  QAction* keep = nullptr;
  QAction* undo = nullptr;
  QAction* resetBox = nullptr;
};

class PropertiesPanel : public QWidget {
  Q_OBJECT
 public:
  explicit PropertiesPanel(const EditActions& actions, QWidget* parent = nullptr);

  void SetDensity(int value, int maxValue);  // current display cap + total (slider max)
  void SetStep(int value);
  void SetBox(bool hasBox, const Bounds& box, qulonglong inBoxAlive, double pct);
  void SetStats(const QString& text);
  void SetInfo(const QString& text);  // basic active volume/tractogram info (always shown)
  // Volume intensity histogram + current grayscale window (data units). hasVolume
  // false hides the Contrast card.
  void SetHistogram(bool hasVolume, std::vector<float> bins, double dataMin, double dataMax,
                    double lo, double hi);
  void SetEditMode(bool on);  // show/hide the Selection + Edit cards (view vs edit)

 signals:
  void densityChanged(int displayN);
  void stepChanged(int dispStep);
  void boxChanged(const Bounds& box);  // user typed new box bounds
  void contrastRangeChanged(double lo, double hi);  // grayscale window dragged
  void refreshStatsRequested();

 private:
  void EmitBox();  // gather the 6 spinboxes -> boxChanged
  // Emit densityChanged only when the value actually changed since the last
  // commit (so a focus-out with no edit, or recommitting the same value, does
  // not trigger a redundant rebuild).
  void CommitDensity(int displayN);

  QLabel* infoLabel_ = nullptr;         // top: basic volume/tractogram info
  QGroupBox* contrastCard_ = nullptr;   // hidden when no volume is active
  HistogramWidget* histogram_ = nullptr;
  QDoubleSpinBox* contrastLo_ = nullptr;   // editable window low (data units)
  QDoubleSpinBox* contrastHi_ = nullptr;   // editable window high (data units)
  QLabel* contrastRangeLabel_ = nullptr;   // "data range  min – max"
  QGroupBox* selectionCard_ = nullptr;  // hidden in view mode
  QGroupBox* editCard_ = nullptr;       // hidden in view mode
  QSlider* densitySlider_;
  QSpinBox* densitySpin_;
  QSpinBox* stepSpin_;
  QDoubleSpinBox* boxSpin_[6];  // xmin, xmax, ymin, ymax, zmin, zmax (RAS mm)
  QLabel* inBoxValue_;
  QLabel* statsValue_;
  bool syncing_ = false;  // guards the slider<->spinbox echo
  int lastDensity_ = -1;  // last committed density (de-dupes commits)
};

}  // namespace tracto
