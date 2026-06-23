#pragma once

// Intensity histogram with two draggable handles that set the grayscale window
// (contrast / display range) of the active volume — FSLeyes/Freeview-style. The
// shaded band between the handles is what maps to black..white; drag a handle (or
// the band) to adjust. Emits rangeChanged(lo, hi) in data units. Pure view.

#include <QString>
#include <QWidget>

#include <vector>

namespace tracto {

class HistogramWidget : public QWidget {
  Q_OBJECT
 public:
  explicit HistogramWidget(QWidget* parent = nullptr);

  void SetHistogram(std::vector<float> bins, double dataMin, double dataMax);
  void SetRange(double lo, double hi);  // reflect a window without emitting

 signals:
  void rangeChanged(double lo, double hi);

 protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;

 private:
  double ValToX(double v) const;  // data value -> pixel x
  double XToVal(double x) const;  // pixel x -> data value (clamped to data range)
  QString FormatVal(double v) const;  // value -> string at a precision suited to the range

  std::vector<float> bins_;
  double dataMin_ = 0.0, dataMax_ = 1.0;
  double lo_ = 0.0, hi_ = 1.0;   // current window in data units
  int drag_ = 0;                 // 0 none, 1 lo handle, 2 hi handle, 3 whole band
  double dragStartVal_ = 0.0, dragLo_ = 0.0, dragHi_ = 0.0;  // band-drag anchors
};

}  // namespace tracto
