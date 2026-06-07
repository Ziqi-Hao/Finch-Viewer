#include "scene_panel.hpp"

#include <QCheckBox>
#include <QFontDatabase>
#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>

namespace tracto {
namespace {

QLabel* DimLabel(const QString& text) {
  auto* label = new QLabel(text);
  label->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  return label;
}

}  // namespace

ScenePanel::ScenePanel(QWidget* parent) : QWidget(parent) {
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(10, 10, 10, 10);
  root->setSpacing(10);

  // ── Tractogram layer ──────────────────────────────────────────────────────
  auto* tract = new QGroupBox("Tractogram");
  {
    auto* layout = new QVBoxLayout(tract);
    layout->setSpacing(2);
    tractName_ = new QLabel("—");
    tractInfo_ = DimLabel("not loaded");
    layout->addWidget(tractName_);
    layout->addWidget(tractInfo_);
  }
  root->addWidget(tract);

  // ── Volume layer (any modality; FA, MD, T1, …) ────────────────────────────
  auto* volume = new QGroupBox("Volume");
  {
    auto* layout = new QVBoxLayout(volume);
    layout->setSpacing(2);
    volumeCheck_ = new QCheckBox("Show slices");
    volumeCheck_->setChecked(true);
    volumeCheck_->setEnabled(false);  // until a volume is loaded
    connect(volumeCheck_, &QCheckBox::toggled, this, &ScenePanel::volumeVisibilityChanged);
    volumeInfo_ = DimLabel("not loaded");
    layout->addWidget(volumeCheck_);
    layout->addWidget(volumeInfo_);
  }
  root->addWidget(volume);

  root->addStretch(1);
}

void ScenePanel::SetTractogram(const QString& name, qulonglong alive, qulonglong total) {
  tractName_->setText(name);
  tractInfo_->setText(QStringLiteral("%1 / %2 alive").arg(alive).arg(total));
}

void ScenePanel::SetVolumeVisibleState(bool visible) {
  QSignalBlocker block(volumeCheck_);
  volumeCheck_->setChecked(visible);
}

void ScenePanel::SetVolume(bool loaded, const QString& name, const QString& info, bool visible) {
  volumeCheck_->setEnabled(loaded);
  {  // reflect state without emitting a visibility-changed back to MainWindow
    QSignalBlocker block(volumeCheck_);
    volumeCheck_->setText(loaded ? name : QStringLiteral("Show slices"));
    volumeCheck_->setChecked(visible);
  }
  volumeInfo_->setText(loaded ? info : QStringLiteral("not loaded"));
}

}  // namespace tracto
