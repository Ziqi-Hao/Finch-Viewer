#include "histogram_widget.hpp"

#include "theme.hpp"

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace tracto {
namespace {
constexpr int kMargin = 6;       // left/right padding for the plot
constexpr int kGrabPx = 6;       // handle grab tolerance in pixels
}  // namespace

HistogramWidget::HistogramWidget(QWidget* parent) : QWidget(parent) {
  setMinimumHeight(84);
  setCursor(Qt::SizeHorCursor);
}

void HistogramWidget::SetHistogram(std::vector<float> bins, double dataMin, double dataMax) {
  bins_ = std::move(bins);
  dataMin_ = dataMin;
  dataMax_ = (dataMax > dataMin) ? dataMax : dataMin + 1.0;
  lo_ = std::clamp(lo_, dataMin_, dataMax_);
  hi_ = std::clamp(hi_, dataMin_, dataMax_);
  update();
}

void HistogramWidget::SetRange(double lo, double hi) {
  lo_ = std::clamp(lo, dataMin_, dataMax_);
  hi_ = std::clamp(hi, dataMin_, dataMax_);
  if (hi_ < lo_) std::swap(lo_, hi_);
  update();
}

double HistogramWidget::ValToX(double v) const {
  const double w = std::max(1, width() - 2 * kMargin);
  const double t = (v - dataMin_) / std::max(1e-9, dataMax_ - dataMin_);
  return kMargin + std::clamp(t, 0.0, 1.0) * w;
}

double HistogramWidget::XToVal(double x) const {
  const double w = std::max(1, width() - 2 * kMargin);
  const double t = std::clamp((x - kMargin) / w, 0.0, 1.0);
  return dataMin_ + t * (dataMax_ - dataMin_);
}

QString HistogramWidget::FormatVal(double v) const {
  // Precision suited to the data range so the readout is informative but not noisy
  // (e.g. FA [0,1] -> 3 dp, a T1 [0,5000] -> 0 dp).
  const double range = dataMax_ - dataMin_;
  const int dec = range >= 1000 ? 0 : range >= 100 ? 1 : range >= 10 ? 2 : 3;
  return QString::number(v, 'f', dec);
}

void HistogramWidget::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, false);
  const int h = height(), w = width();
  p.fillRect(rect(), QColor(theme::kBg1));

  // Bars (sqrt-scaled so the non-background tail is visible past the zero spike).
  if (!bins_.empty()) {
    float peak = 0.0f;
    for (float b : bins_) peak = std::max(peak, b);
    if (peak > 0.0f) {
      p.setPen(Qt::NoPen);
      p.setBrush(QColor(theme::kTextMuted));
      const int n = static_cast<int>(bins_.size());
      const double plotW = std::max(1, w - 2 * kMargin);
      for (int i = 0; i < n; ++i) {
        const double frac = std::sqrt(bins_[i] / peak);
        const int bh = static_cast<int>(frac * (h - 4));
        const int x0 = kMargin + static_cast<int>(plotW * i / n);
        const int x1 = kMargin + static_cast<int>(plotW * (i + 1) / n);
        if (bh > 0) p.fillRect(x0, h - bh, std::max(1, x1 - x0), bh, QColor(theme::kTextMuted));
      }
    }
  }

  // Shaded window band + the two handles.
  const int xlo = static_cast<int>(ValToX(lo_)), xhi = static_cast<int>(ValToX(hi_));
  QColor band(theme::kAccent);
  band.setAlpha(48);
  p.fillRect(xlo, 0, std::max(1, xhi - xlo), h, band);
  p.setPen(QPen(QColor(theme::kAccent), 2));
  p.drawLine(xlo, 0, xlo, h);
  p.drawLine(xhi, 0, xhi, h);

  // Live numeric readout at each handle so the exact window is visible while
  // dragging (FSLeyes-style). lo at the top, hi at the bottom so a narrow band
  // never overlaps them; each clamped to stay inside the widget.
  QFont f = p.font();
  f.setPointSizeF(std::max(7.5, f.pointSizeF() - 1.0));
  p.setFont(f);
  const QFontMetrics fm(f);
  p.setPen(QColor(theme::kText));
  const QString loTxt = FormatVal(lo_), hiTxt = FormatVal(hi_);
  const int loX = std::clamp(xlo + 3, kMargin, w - kMargin - fm.horizontalAdvance(loTxt));
  const int hiX = std::clamp(xhi - 3 - fm.horizontalAdvance(hiTxt), kMargin,
                             w - kMargin - fm.horizontalAdvance(hiTxt));
  p.drawText(loX, fm.ascent() + 1, loTxt);          // top
  p.drawText(hiX, h - fm.descent() - 1, hiTxt);     // bottom
}

void HistogramWidget::mousePressEvent(QMouseEvent* event) {
  const double x = event->position().x();
  const double xlo = ValToX(lo_), xhi = ValToX(hi_);
  if (std::abs(x - xlo) <= kGrabPx) {
    drag_ = 1;
  } else if (std::abs(x - xhi) <= kGrabPx) {
    drag_ = 2;
  } else if (x > xlo && x < xhi) {  // grab the band -> translate the whole window
    drag_ = 3;
    dragStartVal_ = XToVal(x);
    dragLo_ = lo_;
    dragHi_ = hi_;
  } else {
    drag_ = 0;
  }
}

void HistogramWidget::mouseMoveEvent(QMouseEvent* event) {
  if (drag_ == 0) return;
  const double v = XToVal(event->position().x());
  if (drag_ == 1) {
    lo_ = std::min(v, hi_ - 1e-6 * (dataMax_ - dataMin_));
  } else if (drag_ == 2) {
    hi_ = std::max(v, lo_ + 1e-6 * (dataMax_ - dataMin_));
  } else {  // translate band, keeping width and staying in range
    const double width = dragHi_ - dragLo_;
    double nlo = dragLo_ + (v - dragStartVal_);
    nlo = std::clamp(nlo, dataMin_, dataMax_ - width);
    lo_ = nlo;
    hi_ = nlo + width;
  }
  update();
  emit rangeChanged(lo_, hi_);
}

void HistogramWidget::mouseReleaseEvent(QMouseEvent*) { drag_ = 0; }

}  // namespace tracto
