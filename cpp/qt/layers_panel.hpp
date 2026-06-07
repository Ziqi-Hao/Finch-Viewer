#pragma once

// Left-dock Layers panel (Freeview / FSLeyes style): every loaded file is a row
// with a visibility checkbox, grouped into Volume / Tracts / Label. Load as many
// as you like; check to show, uncheck to hide. A passive view — it emits
// visibilityChanged(id, on) and is driven by MainWindow, which owns the data.

#include <QHash>
#include <QWidget>

class QCheckBox;
class QVBoxLayout;

namespace tracto {

class LayersPanel : public QWidget {
  Q_OBJECT
 public:
  enum class Kind { Volume = 0, Tracts = 1, Label = 2 };

  explicit LayersPanel(QWidget* parent = nullptr);

  int AddLayer(Kind kind, const QString& name, bool visible);  // returns a layer id
  void SetVisible(int id, bool on);  // set a checkbox without emitting (sync from code)
  void RemoveLayer(int id);

 signals:
  void visibilityChanged(int id, bool on);

 private:
  QVBoxLayout* groupLayout_[3] = {nullptr, nullptr, nullptr};  // per Kind
  QHash<int, QCheckBox*> checks_;
  int nextId_ = 1;
};

}  // namespace tracto
