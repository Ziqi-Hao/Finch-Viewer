#include "layers_panel.hpp"

#include <QCheckBox>
#include <QGroupBox>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>

namespace tracto {

LayersPanel::LayersPanel(QWidget* parent) : QWidget(parent) {
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(10, 10, 10, 10);
  root->setSpacing(10);

  const char* titles[kKinds] = {"Volume", "Tracts", "Label", "ODF", "Peaks"};
  for (int k = 0; k < kKinds; ++k) {
    auto* box = new QGroupBox(titles[k]);
    groupLayout_[k] = new QVBoxLayout(box);
    groupLayout_[k]->setContentsMargins(8, 6, 8, 6);
    groupLayout_[k]->setSpacing(4);
    root->addWidget(box);
  }
  root->addStretch(1);
}

int LayersPanel::AddLayer(Kind kind, const QString& name, bool visible) {
  const int id = nextId_++;

  auto* row = new QWidget;
  auto* col = new QVBoxLayout(row);
  col->setContentsMargins(0, 0, 0, 0);
  col->setSpacing(1);

  auto* cb = new QCheckBox(name);
  cb->setChecked(visible);
  cb->setToolTip(name);
  connect(cb, &QCheckBox::toggled, this, [this, id](bool on) { emit visibilityChanged(id, on); });

  col->addWidget(cb);

  QSlider* slider = nullptr;
  if (kind == Kind::Volume || kind == Kind::Label) {  // only image layers fade; tracts/ODF/peaks are opaque
    slider = new QSlider(Qt::Horizontal);
    slider->setRange(0, 100);
    slider->setValue(100);
    slider->setToolTip("Opacity");
    connect(slider, &QSlider::valueChanged, this,
            [this, id](int v) { emit opacityChanged(id, v / 100.0); });
    col->addWidget(slider);
  }
  groupLayout_[static_cast<int>(kind)]->addWidget(row);

  rows_.insert(id, Row{row, cb, slider});
  return id;
}

void LayersPanel::SetVisible(int id, bool on) {
  auto it = rows_.find(id);
  if (it == rows_.end()) return;
  QSignalBlocker block(it->check);  // reflect state without echoing visibilityChanged
  it->check->setChecked(on);
}

void LayersPanel::SetOpacity(int id, double opacity) {
  auto it = rows_.find(id);
  if (it == rows_.end() || it->slider == nullptr) return;
  QSignalBlocker block(it->slider);
  it->slider->setValue(static_cast<int>(opacity * 100.0 + 0.5));
}

void LayersPanel::RemoveLayer(int id) {
  auto it = rows_.find(id);
  if (it == rows_.end()) return;
  delete it->widget;  // deletes the row and its child checkbox + slider
  rows_.erase(it);
}

}  // namespace tracto
