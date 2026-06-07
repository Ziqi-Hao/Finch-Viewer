#include "layers_panel.hpp"

#include <QCheckBox>
#include <QGroupBox>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace tracto {

LayersPanel::LayersPanel(QWidget* parent) : QWidget(parent) {
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(10, 10, 10, 10);
  root->setSpacing(10);

  const char* titles[3] = {"Volume", "Tracts", "Label"};
  for (int k = 0; k < 3; ++k) {
    auto* box = new QGroupBox(titles[k]);
    groupLayout_[k] = new QVBoxLayout(box);
    groupLayout_[k]->setContentsMargins(8, 6, 8, 6);
    groupLayout_[k]->setSpacing(2);
    root->addWidget(box);
  }
  root->addStretch(1);
}

int LayersPanel::AddLayer(Kind kind, const QString& name, bool visible) {
  const int id = nextId_++;
  auto* cb = new QCheckBox(name);
  cb->setChecked(visible);
  cb->setToolTip(name);
  connect(cb, &QCheckBox::toggled, this, [this, id](bool on) { emit visibilityChanged(id, on); });
  groupLayout_[static_cast<int>(kind)]->addWidget(cb);
  checks_.insert(id, cb);
  return id;
}

void LayersPanel::SetVisible(int id, bool on) {
  if (QCheckBox* cb = checks_.value(id, nullptr)) {
    QSignalBlocker block(cb);  // reflect state without echoing visibilityChanged
    cb->setChecked(on);
  }
}

void LayersPanel::RemoveLayer(int id) {
  if (QCheckBox* cb = checks_.take(id)) delete cb;
}

}  // namespace tracto
