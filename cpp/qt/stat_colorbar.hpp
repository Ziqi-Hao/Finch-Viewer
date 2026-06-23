#pragma once

// Read-only legend for a signed statistical overlay (fMRI z/t/r). It draws the
// SAME diverging ramp the slice shader uses — cyan→blue for the negative lobe, a
// transparent sub-threshold band in the middle, red→yellow for the positive lobe —
// annotated with the ±cap values and the display threshold + statistic units. It
// mirrors slice.frag's stat mode so the legend always matches what is rendered.
// Pure view: the histogram above is what sets threshold/cap.

#include <QString>
#include <QWidget>

namespace tracto {

class StatColorbar : public QWidget {
  Q_OBJECT
 public:
  explicit StatColorbar(QWidget* parent = nullptr);

  // threshold = |stat| below which voxels are transparent; cap = |stat| that
  // saturates the ramp; units = "z" / "t" / "r" / … ("" falls back to "stat").
  void SetStat(double threshold, double cap, const QString& units);

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  double thr_ = 0.0, cap_ = 1.0;
  QString units_ = QStringLiteral("stat");
};

}  // namespace tracto
