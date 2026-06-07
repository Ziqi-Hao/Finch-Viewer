#pragma once

// Translucent diagnostics overlay pinned to the viewport's top-right: the RHI
// device + backend, FPS, CPU%, and (only where it can be probed) GPU%. The GPU
// segment is OMITTED entirely when no probe is available — never shown as "n/a".
//
// Self-contained: it parents to and tracks the viewport and needs no viewport
// API beyond QRhiWidget's public rhi() + frameSubmitted() signal, so it adds
// nothing to the render path. The (potentially slow) GPU probe runs on a
// background thread; the cheap CPU counter is read on the 1 Hz GUI tick.

#include <QElapsedTimer>
#include <QFrame>
#include <QString>

#include <memory>

class QLabel;

namespace tracto {

class TractViewport;

class PerfOverlay : public QFrame {
  Q_OBJECT
 public:
  explicit PerfOverlay(TractViewport* viewport);
  ~PerfOverlay() override;

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;  // reposition on viewport resize

 private:
  void OnFrame();   // QRhiWidget::frameSubmitted -> update the fps EMA
  void Tick();      // 1 Hz: sample cpu/gpu, read device name + fps, format, reposition
  void Reposition();

  TractViewport* viewport_;
  QLabel* label_;
  QString device_;            // cached once rhi() is available
  QElapsedTimer frameClock_;  // inter-frame timer for the fps EMA
  double fpsEma_ = 0.0;
  bool printedOnce_ = false;  // one-shot stdout line (headless verification)

  struct Sys;                 // platform CPU sampler + nvidia-smi GPU thread (pimpl)
  std::unique_ptr<Sys> sys_;
};

}  // namespace tracto
