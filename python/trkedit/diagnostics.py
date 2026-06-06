"""Live GPU / CPU / FPS overlay (top-left), refreshed every second.

The slow probes (nvidia-smi, cpu sampling) run on a background daemon thread so
they never block the render/interaction thread; the VTK timer only formats the
cached values and updates one text actor.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import threading
import time

from .render import CornerText


class PerfOverlay:
    def __init__(self, plotter):
        self.plotter = plotter
        self.text = CornerText(plotter, position="upper_left", color="lime", font_size=9)
        self._state = {"cpu": "CPU --", "gpu": None, "renderer": None}
        try:
            import psutil
            self.psutil = psutil
        except ImportError:
            self.psutil = None
            print("(tip: `pip install psutil` to also see live CPU%)")
        nv = shutil.which("nvidia-smi") or "/usr/lib/wsl/lib/nvidia-smi"
        self.nvsmi = nv if os.path.exists(nv) else None
        if self.nvsmi is None:
            print("(nvidia-smi not found -> GPU% unavailable; renderer name still shown)")

    def start(self):
        threading.Thread(target=self._worker, daemon=True).start()
        self.plotter.add_timer_event(max_steps=10 ** 9, duration=1000, callback=self._tick)

    def _gpu(self):
        if not self.nvsmi:
            return None
        try:
            r = subprocess.run(
                [self.nvsmi, "--query-gpu=utilization.gpu,memory.used,memory.total",
                 "--format=csv,noheader,nounits"],
                capture_output=True, text=True, timeout=2)
            u, used, tot = (x.strip() for x in r.stdout.strip().splitlines()[0].split(","))
            return f"GPU {float(u):.0f}%  {float(used):.0f}/{float(tot):.0f} MB"
        except Exception:
            return "GPU n/a"

    def _worker(self):
        if self.psutil is not None:
            self.psutil.cpu_percent(None)               # prime
        while True:
            self._state["cpu"] = (f"CPU {self.psutil.cpu_percent(interval=1.0):.0f}%"
                                  if self.psutil is not None else "CPU n/a")
            self._state["gpu"] = self._gpu()
            if self.psutil is None:
                time.sleep(1.0)

    def _tick(self, step=None):
        try:
            dt = self.plotter.renderer.GetLastRenderTimeInSeconds()
            fps = f"{1.0/dt:.0f} fps" if dt and dt > 0 else None
        except Exception:
            fps = None
        if self._state["renderer"] is None:             # query once, after GL exists
            try:
                for ln in self.plotter.render_window.ReportCapabilities().splitlines():
                    if "OpenGL renderer string" in ln:
                        self._state["renderer"] = ln.split(":", 1)[1].strip()
                        break
                print(f"\n[GPU CHECK] rendering on: {self._state['renderer']}\n")
            except Exception:
                self._state["renderer"] = "?"
        bits = [b for b in (f"render: {self._state['renderer']}", fps,
                            self._state["cpu"], self._state["gpu"]) if b]
        self.text.set("   |   ".join(bits))
        self.plotter.render()
