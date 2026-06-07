#pragma once

// Left-dock Scene / Layers list (Blender Outliner-style): the loaded data layers
// — the tractogram and the background volume (any modality) — with visibility and
// a one-line summary each. A passive view: it emits intent signals and is updated
// via setters; MainWindow owns the state. ODF/peaks layers can join later.

#include <QWidget>

class QCheckBox;
class QLabel;

namespace tracto {

class ScenePanel : public QWidget {
  Q_OBJECT
 public:
  explicit ScenePanel(QWidget* parent = nullptr);

  void SetTractogram(const QString& name, qulonglong alive, qulonglong total);
  void SetVolume(bool loaded, const QString& name, const QString& info, bool visible);
  void SetVolumeVisibleState(bool visible);  // reflect a toggle from the H key / menu

 signals:
  void volumeVisibilityChanged(bool visible);

 private:
  QLabel* tractName_;
  QLabel* tractInfo_;
  QCheckBox* volumeCheck_;
  QLabel* volumeInfo_;
};

}  // namespace tracto
