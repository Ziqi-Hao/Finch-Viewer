#include "perf_overlay.hpp"

#include "tract_viewport.hpp"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QRhiWidget>
#include <QTimer>

#include <cstdint>
#include <cstdio>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace tracto {
namespace {

// ── CPU% sampler: reads OS counters (fast, no subprocess) and returns the busy
//    fraction since the previous call. -1 means "unavailable on this platform". ─
struct CpuSampler {
  uint64_t prevBusy = 0, prevTotal = 0;
  bool primed = false;

  double Sample() {
    uint64_t busy = 0, total = 0;
    if (!Read(busy, total)) return -1.0;
    if (!primed) {  // first call only establishes the baseline
      prevBusy = busy; prevTotal = total; primed = true;
      return -1.0;
    }
    const uint64_t db = busy - prevBusy, dt = total - prevTotal;
    prevBusy = busy; prevTotal = total;
    return dt > 0 ? 100.0 * static_cast<double>(db) / static_cast<double>(dt) : 0.0;
  }

 private:
  static bool Read(uint64_t& busy, uint64_t& total) {
#if defined(__APPLE__)
    host_cpu_load_info_data_t info;
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
                        reinterpret_cast<host_info_t>(&info), &count) != KERN_SUCCESS)
      return false;
    busy = info.cpu_ticks[CPU_STATE_USER] + info.cpu_ticks[CPU_STATE_SYSTEM] +
           info.cpu_ticks[CPU_STATE_NICE];
    total = busy + info.cpu_ticks[CPU_STATE_IDLE];
    return true;
#elif defined(__linux__)
    std::FILE* f = std::fopen("/proc/stat", "r");
    if (!f) return false;
    unsigned long long u = 0, n = 0, s = 0, i = 0, io = 0, irq = 0, sirq = 0;
    const int got = std::fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &s, &i,
                                &io, &irq, &sirq);
    std::fclose(f);
    if (got < 4) return false;
    busy = u + n + s + irq + sirq;
    total = busy + i + io;
    return true;
#elif defined(_WIN32)
    FILETIME idle, kernel, user;
    if (!GetSystemTimes(&idle, &kernel, &user)) return false;
    auto q = [](FILETIME t) {
      return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
    };
    const uint64_t i = q(idle), k = q(kernel), us = q(user);  // kernel includes idle
    total = k + us;
    busy = total - i;
    return true;
#else
    (void)busy; (void)total;
    return false;
#endif
  }
};

// ── GPU% sampler: nvidia-smi on a background thread (POSIX only). Produces a
//    value only when nvidia-smi exists, so it stays silent on macOS/Windows and
//    any machine without an NVIDIA driver — i.e. "no GPU found -> no GPU shown". ─
struct GpuSampler {
  std::thread th;
  std::atomic<bool> stop{false};
  std::mutex m;
  std::optional<std::string> cached;

  void Start() {
#if !defined(_WIN32)
    if (!HasNvidiaSmi()) return;  // no probe -> never produces a value
    th = std::thread([this] { Loop(); });
#endif
  }
  ~GpuSampler() {
    stop = true;
    if (th.joinable()) th.join();
  }
  std::optional<std::string> Read() {
    std::lock_guard<std::mutex> lk(m);
    return cached;
  }

#if !defined(_WIN32)
 private:
  static bool HasNvidiaSmi() {
    std::FILE* p = popen("command -v nvidia-smi 2>/dev/null", "r");  // empty on macOS
    if (!p) return false;
    char buf[256] = {};
    const bool found = std::fgets(buf, sizeof(buf), p) != nullptr && buf[0] != '\0';
    pclose(p);
    return found;
  }
  void Loop() {
    while (!stop) {
      if (auto r = RunOnce()) {
        std::lock_guard<std::mutex> lk(m);
        cached = r;
      }
      for (int i = 0; i < 10 && !stop; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));  // ~1s, stop-aware
    }
  }
  static std::optional<std::string> RunOnce() {
    // `timeout 2` caps the subprocess so a slow/wedged nvidia-smi can never stall
    // the dtor's join() for more than ~2s (timeout is present wherever nvidia-smi is).
    std::FILE* p = popen(
        "timeout 2 nvidia-smi --query-gpu=utilization.gpu,memory.used,memory.total "
        "--format=csv,noheader,nounits 2>/dev/null",
        "r");
    if (!p) return std::nullopt;
    char buf[256] = {};
    const char* got = std::fgets(buf, sizeof(buf), p);
    pclose(p);
    if (!got) return std::nullopt;
    double util = 0, used = 0, tot = 0;
    if (std::sscanf(buf, "%lf , %lf , %lf", &util, &used, &tot) < 3) return std::nullopt;
    char out[80];
    std::snprintf(out, sizeof(out), "GPU %.0f%%  %.1f/%.1f GB", util, used / 1024.0, tot / 1024.0);
    return std::string(out);
  }
#endif
};

}  // namespace

struct PerfOverlay::Sys {
  CpuSampler cpu;
  GpuSampler gpu;
};

PerfOverlay::PerfOverlay(TractViewport* viewport)
    : QFrame(viewport), viewport_(viewport), sys_(std::make_unique<Sys>()) {
  setObjectName("perfHud");
  setAttribute(Qt::WA_TransparentForMouseEvents);  // never eats viewport drags
  // Self-contained styling (so it needs nothing from theme.cpp): translucent
  // slate card + calm green monospace, matching the Python perf overlay.
  setStyleSheet(
      "#perfHud { background-color: rgba(15,17,21,0.66); border: 1px solid #262B33;"
      " border-radius: 6px; }"
      "#perfHud QLabel { background: transparent; color: rgb(128,217,153);"
      " font-family: monospace; font-size: 11px; }");

  label_ = new QLabel(this);
  label_->setAttribute(Qt::WA_TransparentForMouseEvents);
  auto* layout = new QHBoxLayout(this);
  layout->setContentsMargins(10, 5, 10, 5);
  layout->addWidget(label_);
  label_->setText(QStringLiteral("…"));

  connect(viewport_, &QRhiWidget::frameSubmitted, this, &PerfOverlay::OnFrame);
  viewport_->installEventFilter(this);  // reposition when the viewport resizes

  auto* timer = new QTimer(this);
  timer->setInterval(1000);
  connect(timer, &QTimer::timeout, this, &PerfOverlay::Tick);
  timer->start();

  sys_->gpu.Start();
  frameClock_.start();
  adjustSize();
  Reposition();
  show();
}

PerfOverlay::~PerfOverlay() = default;  // ~Sys joins the GPU thread

bool PerfOverlay::eventFilter(QObject* watched, QEvent* event) {
  if (watched == viewport_ && event->type() == QEvent::Resize) Reposition();
  return QFrame::eventFilter(watched, event);
}

void PerfOverlay::OnFrame() {
  // Smooth inter-frame FPS; ignore >0.5 s gaps so resuming from idle doesn't dip it.
  const qint64 ns = frameClock_.nsecsElapsed();
  if (ns > 0 && ns < 500'000'000) {
    const double f = 1.0e9 / static_cast<double>(ns);
    fpsEma_ = fpsEma_ > 0.0 ? 0.85 * fpsEma_ + 0.15 * f : f;
  }
  frameClock_.restart();
}

void PerfOverlay::Tick() {
  if (device_.isEmpty()) device_ = viewport_->RendererName();  // empty until the rhi exists

  QStringList parts;
  if (!device_.isEmpty()) parts << device_;
  parts << QStringLiteral("%1 fps").arg(fpsEma_, 0, 'f', 0);
  const double cpu = sys_->cpu.Sample();
  if (cpu >= 0.0) parts << QStringLiteral("CPU %1%").arg(cpu, 0, 'f', 0);
  if (auto gpu = sys_->gpu.Read()) parts << QString::fromStdString(*gpu);  // omitted if no probe

  const QString line = parts.join(QStringLiteral("   ·   "));
  label_->setText(line);
  adjustSize();
  Reposition();

  if (!printedOnce_ && !device_.isEmpty() && cpu >= 0.0) {  // one-shot, for headless verification
    std::printf("perf: %s\n", line.toUtf8().constData());
    std::fflush(stdout);
    printedOnce_ = true;
  }
}

void PerfOverlay::Reposition() {
  move(viewport_->width() - width() - 12, 12);  // top-right corner
}

}  // namespace tracto
