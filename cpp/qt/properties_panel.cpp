#include "properties_panel.hpp"

#include "histogram_widget.hpp"

#include <QAction>
#include <QDoubleSpinBox>
#include <QFontDatabase>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace tracto {
namespace {

// A titled card. The cards stack in the panel; QSS styles QGroupBox as the card.
QGroupBox* Card(const QString& title) {
  auto* box = new QGroupBox(title);
  return box;
}

// A button that mirrors a shared QAction (text, enabled state, trigger).
QToolButton* ActionButton(QAction* action) {
  auto* button = new QToolButton;
  button->setDefaultAction(action);
  button->setToolButtonStyle(Qt::ToolButtonTextOnly);
  button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  return button;
}

QLabel* MonoValue(const QString& text) {
  auto* label = new QLabel(text);
  label->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  return label;
}

}  // namespace

PropertiesPanel::PropertiesPanel(const EditActions& actions, QWidget* parent)
    : QWidget(parent) {
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(10, 10, 10, 10);
  root->setSpacing(10);

  // ── Display: how much of the tractogram is drawn (visual only). ───────────
  QGroupBox* display = Card("Display");
  {
    densitySlider_ = new QSlider(Qt::Horizontal);
    densitySlider_->setRange(1, 12000);
    densitySpin_ = new QSpinBox;
    densitySpin_->setRange(1, 12000);
    densitySpin_->setGroupSeparatorShown(true);
    stepSpin_ = new QSpinBox;
    stepSpin_->setRange(1, 20);
    stepSpin_->setKeyboardTracking(false);  // commit once on edit-finish, not per keystroke

    // Slider and spinbox echo each other for display; the value is only
    // committed (a rebuild) on release / edit-finished to avoid thrashing.
    connect(densitySlider_, &QSlider::valueChanged, this, [this](int v) {
      if (syncing_) return;
      syncing_ = true;
      densitySpin_->setValue(v);
      syncing_ = false;
      // Keyboard/wheel slider moves commit immediately; a mouse drag commits on
      // release (sliderReleased) so it doesn't rebuild on every dragged pixel.
      if (!densitySlider_->isSliderDown()) CommitDensity(v);
    });
    connect(densitySpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int v) {
      if (syncing_) return;
      syncing_ = true;
      densitySlider_->setValue(v);
      syncing_ = false;
    });
    connect(densitySlider_, &QSlider::sliderReleased, this,
            [this]() { CommitDensity(densitySlider_->value()); });
    connect(densitySpin_, &QSpinBox::editingFinished, this,
            [this]() { CommitDensity(densitySpin_->value()); });
    connect(stepSpin_, qOverload<int>(&QSpinBox::valueChanged), this,
            [this](int v) { emit stepChanged(v); });

    auto* grid = new QGridLayout(display);
    grid->setColumnStretch(1, 1);
    grid->addWidget(new QLabel("Density"), 0, 0);
    grid->addWidget(densitySlider_, 0, 1);
    grid->addWidget(densitySpin_, 0, 2);
    grid->addWidget(new QLabel("Step"), 1, 0);
    grid->addWidget(stepSpin_, 1, 2);
  }
  root->addWidget(display);

  // ── Contrast: volume intensity histogram with draggable grayscale window, plus
  // editable Low/High fields + a data-range readout so the exact numbers are
  // always visible and directly typeable (FSLeyes/Freeview-style). ──────────────
  contrastCard_ = Card("Contrast");
  {
    auto* layout = new QVBoxLayout(contrastCard_);
    histogram_ = new HistogramWidget;
    layout->addWidget(histogram_);

    contrastLo_ = new QDoubleSpinBox;
    contrastHi_ = new QDoubleSpinBox;
    for (QDoubleSpinBox* s : {contrastLo_, contrastHi_}) {
      s->setKeyboardTracking(false);  // emit on Enter/focus-out, not every keystroke
      s->setMinimumWidth(76);
    }
    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel("Low"));
    row->addWidget(contrastLo_, 1);
    row->addSpacing(10);
    row->addWidget(new QLabel("High"));
    row->addWidget(contrastHi_, 1);
    layout->addLayout(row);

    contrastRangeLabel_ = new QLabel;
    contrastRangeLabel_->setObjectName("hintLabel");  // muted style if the theme has it
    layout->addWidget(contrastRangeLabel_);

    // Drag the histogram -> reflect into the fields (no echo) + bubble the change up.
    connect(histogram_, &HistogramWidget::rangeChanged, this, [this](double lo, double hi) {
      QSignalBlocker bl(contrastLo_), bh(contrastHi_);
      contrastLo_->setValue(lo);
      contrastHi_->setValue(hi);
      emit contrastRangeChanged(lo, hi);
    });
    // Type a field -> move the histogram window + bubble up (keep lo <= hi).
    auto onSpin = [this] {
      double lo = contrastLo_->value(), hi = contrastHi_->value();
      if (hi < lo) std::swap(lo, hi);
      histogram_->SetRange(lo, hi);  // reflects without re-emitting
      emit contrastRangeChanged(lo, hi);
    };
    connect(contrastLo_, &QDoubleSpinBox::editingFinished, this, onSpin);
    connect(contrastHi_, &QDoubleSpinBox::editingFinished, this, onSpin);
  }
  root->addWidget(contrastCard_);

  // ── Selection: editable 3-D box bounds + live readout of what it holds. ────
  selectionCard_ = Card("Selection");
  QGroupBox* selection = selectionCard_;
  {
    auto* grid = new QGridLayout(selection);
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(2, 1);
    grid->addWidget(new QLabel("min"), 0, 1, Qt::AlignHCenter);
    grid->addWidget(new QLabel("max"), 0, 2, Qt::AlignHCenter);
    const char* axes[3] = {"X", "Y", "Z"};
    for (int a = 0; a < 3; ++a) {
      grid->addWidget(new QLabel(axes[a]), a + 1, 0);
      for (int s = 0; s < 2; ++s) {
        auto* spin = new QDoubleSpinBox;
        spin->setRange(-100000.0, 100000.0);  // RAS mm, generous; clamped in viewport
        spin->setDecimals(1);
        spin->setSingleStep(1.0);
        spin->setKeyboardTracking(false);  // emit once on commit, not per keystroke
        // Typing new bounds drives the viewport box (two-way: drags echo back via SetBox).
        connect(spin, &QDoubleSpinBox::editingFinished, this, &PropertiesPanel::EmitBox);
        boxSpin_[a * 2 + s] = spin;
        grid->addWidget(spin, a + 1, 1 + s);
      }
    }
    inBoxValue_ = MonoValue("in box: —");
    grid->addWidget(inBoxValue_, 4, 0, 1, 3);
    grid->addWidget(ActionButton(actions.resetBox), 5, 0, 1, 3);
  }
  root->addWidget(selection);

  // ── Edit: the same actions as the toolbar, as a button cluster. ───────────
  editCard_ = Card("Edit");
  QGroupBox* edit = editCard_;
  {
    auto* grid = new QGridLayout(edit);
    QToolButton* del = ActionButton(actions.del);
    del->setObjectName("dangerButton");  // red-on-hover (destructive)
    grid->addWidget(del, 0, 0);
    grid->addWidget(ActionButton(actions.keep), 0, 1);
    grid->addWidget(ActionButton(actions.undo), 1, 0, 1, 2);
  }
  root->addWidget(edit);

  // ── Info & Statistics: basic facts (always) + on-demand expensive stats. ──
  QGroupBox* stats = Card("Info");
  {
    auto* layout = new QVBoxLayout(stats);
    infoLabel_ = MonoValue("No data loaded.");  // basic volume/tractogram facts (always)
    infoLabel_->setWordWrap(true);
    infoLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statsValue_ = MonoValue("—");                // length / pts-per-line / deleted% (on Refresh)
    statsValue_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* refresh = new QPushButton("Refresh statistics");
    connect(refresh, &QPushButton::clicked, this, &PropertiesPanel::refreshStatsRequested);
    layout->addWidget(infoLabel_);
    layout->addWidget(statsValue_);
    layout->addWidget(refresh);
  }
  root->addWidget(stats);

  root->addStretch(1);
  SetEditMode(false);            // start in view mode: Selection + Edit cards hidden
  contrastCard_->setVisible(false);  // shown once a volume is loaded
}

void PropertiesPanel::SetDensity(int value, int maxValue) {
  syncing_ = true;  // setters update the controls without re-emitting
  const int hi = std::max(1, maxValue);
  densitySlider_->setRange(1, hi);
  densitySpin_->setRange(1, hi);
  densitySlider_->setValue(value);
  densitySpin_->setValue(value);
  syncing_ = false;
  lastDensity_ = value;  // baseline: a later identical user commit is a no-op
}

void PropertiesPanel::CommitDensity(int displayN) {
  if (displayN == lastDensity_) return;  // unchanged -> no rebuild
  lastDensity_ = displayN;
  emit densityChanged(displayN);
}

void PropertiesPanel::SetStep(int value) {
  QSignalBlocker block(stepSpin_);  // avoid echoing a stepChanged back out
  stepSpin_->setValue(value);
}

void PropertiesPanel::SetBox(bool hasBox, const Bounds& box, qulonglong inBoxAlive, double pct) {
  for (QDoubleSpinBox* s : boxSpin_) s->setEnabled(hasBox);
  if (!hasBox) {
    inBoxValue_->setText("in box: —");
    return;
  }
  // Mirror the live box into the spinboxes, but never fight a field being typed
  // in, and block signals so this echo doesn't loop back as a boxChanged.
  for (int i = 0; i < 6; ++i) {
    if (boxSpin_[i]->hasFocus()) continue;
    QSignalBlocker block(boxSpin_[i]);
    boxSpin_[i]->setValue(box.v[i]);
  }
  inBoxValue_->setText(QStringLiteral("in box: %1 (%2%)")
                           .arg(QLocale().toString(inBoxAlive))
                           .arg(pct, 0, 'f', 1));
}

void PropertiesPanel::EmitBox() {
  Bounds box;
  for (int i = 0; i < 6; ++i) box.v[i] = boxSpin_[i]->value();
  emit boxChanged(box);
}

void PropertiesPanel::SetStats(const QString& text) { statsValue_->setText(text); }

void PropertiesPanel::SetInfo(const QString& text) { infoLabel_->setText(text); }

void PropertiesPanel::SetHistogram(bool hasVolume, std::vector<float> bins, double dataMin,
                                   double dataMax, double lo, double hi) {
  contrastCard_->setVisible(hasVolume);
  if (!hasVolume) return;
  histogram_->SetHistogram(std::move(bins), dataMin, dataMax);
  histogram_->SetRange(lo, hi);

  // Match the spinbox precision/step/range to the data, then reflect the window.
  const double range = dataMax - dataMin;
  const int dec = range >= 1000 ? 0 : range >= 100 ? 1 : range >= 10 ? 2 : 3;
  const double step = std::max(1e-6, range / 100.0);
  for (QDoubleSpinBox* s : {contrastLo_, contrastHi_}) {
    QSignalBlocker block(s);
    s->setDecimals(dec);
    s->setRange(dataMin, dataMax);
    s->setSingleStep(step);
  }
  { QSignalBlocker bl(contrastLo_); contrastLo_->setValue(lo); }
  { QSignalBlocker bh(contrastHi_); contrastHi_->setValue(hi); }
  contrastRangeLabel_->setText(QString("data range  %1 – %2")
                                  .arg(QString::number(dataMin, 'f', dec),
                                       QString::number(dataMax, 'f', dec)));
}

void PropertiesPanel::SetEditMode(bool on) {
  selectionCard_->setVisible(on);
  editCard_->setVisible(on);
}

}  // namespace tracto
