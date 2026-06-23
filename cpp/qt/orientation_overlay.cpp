#include "orientation_overlay.hpp"

#include "theme.hpp"
#include "tract_viewport.hpp"

#include <QEvent>
#include <QFontMetricsF>
#include <QPainter>

namespace tracto {
namespace {
constexpr double kPad = 5.0;  // inset of each letter from its pane edge

// The four edge letters for an ortho pane, fixed by the OrthoSliceCamera basis
// (RAS: +X=R −X=L, +Y=A −Y=P, +Z=S −Z=I): axial right=+X up=+Y; coronal right=−X
// up=+Z; sagittal right=+Y up=+Z. Kept in lockstep with render_math.hpp's Right()/Up().
struct Edges { const char* top; const char* bottom; const char* left; const char* right; };
Edges EdgesFor(int axis) {
  switch (axis) {
    case 2:  return {"A", "P", "L", "R"};   // axial    (looking down +Z)
    case 1:  return {"S", "I", "R", "L"};   // coronal  (looking down +Y)
    default: return {"S", "I", "P", "A"};   // sagittal (looking down +X)
  }
}
}  // namespace

OrientationOverlay::OrientationOverlay(TractViewport* viewport)
    : QWidget(viewport), viewport_(viewport) {
  setAttribute(Qt::WA_TransparentForMouseEvents);  // never eats camera drags
  // It covers the WHOLE viewport, so its background must be transparent or it would
  // hide the RHI slices; only the painted letters show.
  setAttribute(Qt::WA_NoSystemBackground);
  setAttribute(Qt::WA_TranslucentBackground);
  setGeometry(viewport->rect());
  viewport->installEventFilter(this);  // resize with the viewport
}

bool OrientationOverlay::eventFilter(QObject* watched, QEvent* event) {
  if (watched == viewport_ && event->type() == QEvent::Resize) {
    setGeometry(viewport_->rect());
    update();
  }
  return QWidget::eventFilter(watched, event);
}

void OrientationOverlay::paintEvent(QPaintEvent*) {
  const std::vector<TractViewport::OrthoPaneInfo> panes = viewport_->OrthoPaneLayout();
  if (panes.empty()) return;  // nothing loaded -> no labels (keep the empty viewer clean)

  QPainter p(this);
  QFont f = p.font();
  f.setPointSizeF(std::max(8.0, f.pointSizeF()));
  f.setBold(true);
  p.setFont(f);
  const QFontMetricsF fm(f);

  // Draw each letter with a 1px dark shadow first so it stays legible over both
  // dark background and bright slice content.
  auto drawTag = [&](const char* s, double cx, double cy) {
    const QString t(s);
    const double x = cx - fm.horizontalAdvance(t) * 0.5;
    const double y = cy + fm.ascent() * 0.5 - fm.descent() * 0.5;
    p.setPen(QColor(0, 0, 0, 160));
    p.drawText(QPointF(x + 1.0, y + 1.0), t);
    p.setPen(QColor(theme::kTextDim));
    p.drawText(QPointF(x, y), t);
  };

  for (const TractViewport::OrthoPaneInfo& pi : panes) {
    const Edges e = EdgesFor(pi.axis);
    const QRectF& r = pi.rect;
    const double cx = r.center().x(), cy = r.center().y();
    drawTag(e.top, cx, r.top() + fm.ascent() + kPad);
    drawTag(e.bottom, cx, r.bottom() - fm.descent() - kPad);
    drawTag(e.left, r.left() + fm.horizontalAdvance(QString(e.left)) * 0.5 + kPad, cy);
    drawTag(e.right, r.right() - fm.horizontalAdvance(QString(e.right)) * 0.5 - kPad, cy);
  }
}

}  // namespace tracto
