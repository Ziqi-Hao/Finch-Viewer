#include "stat_colorbar.hpp"

#include "theme.hpp"

#include <QFontMetrics>
#include <QLinearGradient>
#include <QPainter>

#include <algorithm>

namespace tracto {
namespace {
constexpr int kMargin = 6;   // left/right padding (matches the histogram above)
constexpr int kBarTop = 3;
constexpr int kBarH = 15;
}  // namespace

StatColorbar::StatColorbar(QWidget* parent) : QWidget(parent) { setMinimumHeight(40); }

void StatColorbar::SetStat(double threshold, double cap, const QString& units) {
  cap_ = std::max(1e-6, cap);
  thr_ = std::clamp(threshold, 0.0, cap_);
  units_ = units.isEmpty() ? QStringLiteral("stat") : units;
  update();
}

void StatColorbar::paintEvent(QPaintEvent*) {
  QPainter p(this);
  const int w = width();
  const double plotW = std::max(1, w - 2 * kMargin);
  // Map a signed value in [-cap, +cap] to a pixel x.
  auto xOf = [&](double v) { return kMargin + (v + cap_) / (2.0 * cap_) * plotW; };
  const double xNegCap = xOf(-cap_), xNegThr = xOf(-thr_);
  const double xPosThr = xOf(thr_), xPosCap = xOf(cap_);

  // The colour stops are the EXACT shader endpoints (slice.frag stat mode): the
  // negative lobe runs cyan(|v|=cap) → blue(|v|=thr), the positive lobe red(|v|=thr)
  // → yellow(|v|=cap), and the sub-threshold middle is left as background (the
  // "transparent, anatomy shows through" zone).
  if (xNegThr > xNegCap) {
    QLinearGradient g(xNegCap, 0, xNegThr, 0);
    g.setColorAt(0.0, QColor(0, 255, 255));   // cyan   (shader (0,1,1))
    g.setColorAt(1.0, QColor(0, 0, 255));     // blue   (0,0,1)
    p.fillRect(QRectF(xNegCap, kBarTop, xNegThr - xNegCap, kBarH), g);
  }
  if (xPosCap > xPosThr) {
    QLinearGradient g(xPosThr, 0, xPosCap, 0);
    g.setColorAt(0.0, QColor(255, 0, 0));     // red    (1,0,0)
    g.setColorAt(1.0, QColor(255, 255, 0));   // yellow (1,1,0)
    p.fillRect(QRectF(xPosThr, kBarTop, xPosCap - xPosThr, kBarH), g);
  }

  p.setBrush(Qt::NoBrush);
  p.setPen(QColor(theme::kHairline));
  p.drawRect(QRectF(xNegCap, kBarTop, xPosCap - xNegCap, kBarH));  // frame the full range
  p.setPen(QColor(theme::kTextMuted));                            // threshold-band edges
  p.drawLine(QPointF(xNegThr, kBarTop), QPointF(xNegThr, kBarTop + kBarH));
  p.drawLine(QPointF(xPosThr, kBarTop), QPointF(xPosThr, kBarTop + kBarH));

  // Labels: −cap / +cap at the ends, "|units| ≥ thr" centred (it names both the
  // threshold and the statistic, and explains the transparent middle band).
  // Precision adapts to the cap so small-but-valid ranges (β / small-unit maps) stay
  // legible instead of collapsing to ±0.00.
  const int dec = cap_ >= 10.0 ? 1 : cap_ >= 0.5 ? 2 : cap_ >= 0.05 ? 3 : 4;
  QFont f = p.font();
  f.setPointSizeF(std::max(7.5, f.pointSizeF() - 1.0));
  p.setFont(f);
  const QFontMetrics fm(f);
  const int ty = kBarTop + kBarH + fm.ascent() + 3;
  const QString lo = QString("−%1").arg(cap_, 0, 'f', dec);
  const QString hi = QString("+%1").arg(cap_, 0, 'f', dec);
  const QString mid = QString("|%1| ≥ %2").arg(units_, QString::number(thr_, 'f', dec));
  p.setPen(QColor(theme::kTextDim));
  p.drawText(kMargin, ty, lo);
  p.drawText(w - kMargin - fm.horizontalAdvance(hi), ty, hi);
  p.setPen(QColor(theme::kText));
  p.drawText((w - fm.horizontalAdvance(mid)) / 2, ty, mid);
}

}  // namespace tracto
