#pragma once

// Anatomical orientation labels (L/R · A/P · S/I) drawn at the edges of each 2-D
// ortho pane, like every clinical viewer. A transparent, mouse-passthrough child
// of the viewport (same pattern as the perf/HUD overlays): it pulls the visible
// ortho-pane rects + axes from the viewport and paints the four edge letters. The
// letters are fixed per axis by the ortho cameras' screen→world basis, so the
// overlay needs no camera state — only the pane geometry.

#include <QWidget>

namespace tracto {

class TractViewport;

class OrientationOverlay : public QWidget {
 public:
  explicit OrientationOverlay(TractViewport* viewport);

 protected:
  void paintEvent(QPaintEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;  // track viewport resize

 private:
  TractViewport* viewport_;
};

}  // namespace tracto
