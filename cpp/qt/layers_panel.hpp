#pragma once

// Left-dock Layers panel (Freeview / FSLeyes style): every loaded file is a row
// with a visibility checkbox, grouped into Volume / Tracts / Label. Volume and
// label rows also expose opacity because the viewport can fade those layers now.
// Tract rows stay visibility-only until the renderer supports tract alpha.

#include <QHash>
#include <QWidget>

class QCheckBox;
class QSlider;
class QVBoxLayout;

namespace tracto {

class LayersPanel : public QWidget {
  Q_OBJECT
 public:
  enum class Kind { Volume = 0, Tracts = 1, Label = 2, Odf = 3, Peaks = 4, Stat = 5 };

  explicit LayersPanel(QWidget* parent = nullptr);

  int AddLayer(Kind kind, const QString& name, bool visible);  // returns a layer id
  void SetVisible(int id, bool on);        // set the checkbox without emitting
  void SetOpacity(int id, double opacity);  // set the slider (0..1) without emitting
  void RemoveLayer(int id);

 signals:
  void visibilityChanged(int id, bool on);
  void opacityChanged(int id, double opacity);  // 0..1

 private:
  struct Row { QWidget* widget; QCheckBox* check; QSlider* slider; };
  static constexpr int kKinds = 6;                       // Volume/Tracts/Label/ODF/Peaks/Stat
  QVBoxLayout* groupLayout_[kKinds] = {nullptr};         // per Kind
  QHash<int, Row> rows_;
  int nextId_ = 1;
};

}  // namespace tracto
