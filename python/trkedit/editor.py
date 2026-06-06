"""Editor: owns the edit state and wires data / selection / render / interaction.

Editing acts on the FULL set (`alive`); the display is only a sampled view, so
saving is exact.  Per-edit refresh is cheap (NaN-hide + recolour, no re-pack);
only density changes and file loads resample the displayed geometry.
"""
from __future__ import annotations

import os
import sys
import time

import numpy as np
import pyvista as pv

from .tractogram import Tractogram
from .fa import FaVolume
from .selection import make_selector
from . import render as R
from . import interaction as I
from . import ui
from .diagnostics import PerfOverlay

HISTORY_MAX = 100


class Editor:
    def __init__(self, args):
        self.args = args
        self.fa = None
        self.tg = None
        self.selector = None
        self.plotter = None
        self.line = None
        self.status = None
        self.perf = None
        self.bg_actors = []
        self.box_widget = None
        self.box_bounds = None
        self.alive = None
        self.history = []
        self.dirty = False
        self.show_fa = True
        self.display_n = int(args.display_n)
        self.n_shown = 0
        self._fa_pp = None        # lazy per-point FA cache (stats only)
        self._last_hl = 0.0

    # ── data ────────────────────────────────────────────────────────────────
    def _load_tractogram(self, path):
        tg = Tractogram.load(path, self.fa.img)
        if tg is None:
            print("  !! no streamlines in that file -- keeping current set")
            return False
        self.tg = tg
        self.selector = make_selector(self.args.selector, tg)
        self.alive = np.ones(tg.n, dtype=bool)
        self.history = []
        self.dirty = False
        self._fa_pp = None
        print(f"  {tg.n:,} streamlines, {tg.total_pts:,} points "
              f"(selector: {self.args.selector})")
        return True

    def default_box_bounds(self):
        lo, hi = self.tg.extent()
        c = (lo + hi) / 2.0; h = (hi - lo) * 0.30 / 2.0
        return (c[0]-h[0], c[0]+h[0], c[1]-h[1], c[1]+h[1], c[2]-h[2], c[2]+h[2])

    # ── display refresh ───────────────────────────────────────────────────────
    def _selected(self):
        return None if self.box_bounds is None else self.selector.in_box(self.box_bounds)

    def refresh_highlight(self):
        self.line.set_highlight(self._selected(), self.alive)

    def on_box(self, box):
        self.box_bounds = box.bounds
        now = time.time()
        if now - self._last_hl >= 0.05:        # ~20 Hz live highlight; keeps drag smooth
            self._last_hl = now
            self.refresh_highlight()

    def _refresh(self):
        """After an edit: NaN-hide dead lines, update count, recolour (renders)."""
        self.line.apply_alive(self.alive)
        self.n_shown = self.line.n_shown(self.alive)
        self._update_status()
        self.refresh_highlight()

    def _rebuild(self, reset_camera=False):
        """On density change / load: resample and rebuild the displayed geometry."""
        self.line.build(self.alive, self.display_n, reset_camera=reset_camera)
        self.n_shown = self.line.n_shown(self.alive)
        self._update_status()
        self.refresh_highlight()

    def _update_status(self):
        na = int(self.alive.sum())
        self.status.set(
            f"alive: {na:,}/{self.tg.n:,}    shown: {self.n_shown:,} "
            f"(cap {self.display_n:,})    [white = inside box]\n"
            "d=delete  k=keep  p=preview  t=stats  +/-=density  n=set#  l=load  "
            "u=undo  r=reset  s=save  h=FA  q=quit")

    # ── edit operations (act on the FULL set) ────────────────────────────────
    def _push(self):
        self.history.append(self.alive.copy())
        if len(self.history) > HISTORY_MAX:
            self.history.pop(0)

    def delete_in_box(self):
        if self.box_bounds is None:
            print("Move the box first."); return
        kill = self.selector.in_box(self.box_bounds) & self.alive
        n = int(kill.sum())
        if n == 0:
            print("box contains 0 alive streamlines"); return
        self._push(); self.alive[kill] = False; self.dirty = True
        print(f"deleted {n:,}  (alive {int(self.alive.sum()):,}/{self.tg.n:,})")
        self._refresh()

    def keep_only_in_box(self):
        if self.box_bounds is None:
            print("Move the box first."); return
        inb = self.selector.in_box(self.box_bounds)
        n = int((inb & self.alive).sum())
        if n == 0:
            print("box contains 0 alive streamlines"); return
        self._push(); self.alive &= inb; self.dirty = True
        print(f"kept {n:,}  (alive {int(self.alive.sum()):,}/{self.tg.n:,})")
        self._refresh()

    def undo(self):
        if not self.history:
            print("nothing to undo"); return
        self.alive = self.history.pop()
        print(f"undo -> alive {int(self.alive.sum()):,}/{self.tg.n:,}")
        self._refresh()

    def reset(self):
        if self.alive.all():
            print("already full"); return
        self._push(); self.alive[:] = True; self.dirty = False
        print("reset"); self._refresh()

    def toggle_fa(self):
        self.show_fa = not self.show_fa
        for a in self.bg_actors:
            a.SetVisibility(self.show_fa)
        self.plotter.render()

    # ── display density ───────────────────────────────────────────────────────
    def density_up(self):
        self.display_n = min(self.tg.n, int(self.display_n * 1.5) + 1)
        print(f"display cap -> {self.display_n:,}"); self._rebuild()

    def density_down(self):
        self.display_n = max(200, int(self.display_n / 1.5))
        print(f"display cap -> {self.display_n:,}"); self._rebuild()

    def density_set(self):
        try:
            raw = input("display how many streamlines (number or %, e.g. 8000 or 25%): ").strip()
        except EOFError:
            return
        if not raw:
            return
        try:
            val = (int(round(float(raw[:-1]) / 100.0 * self.tg.n))
                   if raw.endswith("%") else int(float(raw)))
        except ValueError:
            print("could not parse:", raw); return
        self.display_n = max(1, min(self.tg.n, val))
        print(f"display cap -> {self.display_n:,}"); self._rebuild()

    # ── load / save ───────────────────────────────────────────────────────────
    def load_dialog(self):
        if self.dirty:
            print("note: discarding unsaved edits on the current file.")
        path = I.ask_open_file("Open .trk tractogram",
                               [("TRK tractograms", "*.trk"), ("All files", "*.*")])
        if not path:
            print("load cancelled"); return
        print(f"Loading TRK  : {path}")
        if not self._load_tractogram(path):
            return
        self.line.tg = self.tg
        self.line.reset_cache()
        nb = self.default_box_bounds(); self.box_bounds = nb
        try:
            self.box_widget.PlaceWidget(nb)
        except Exception as e:
            print("box reposition skipped:", e)
        self._rebuild(reset_camera=True)
        print(f"loaded {os.path.basename(path)}")

    def load_fa(self):
        """Load a different FA/image volume and rebuild the slice backdrop."""
        path = I.ask_open_file("Open FA / image (NIfTI)",
                               [("NIfTI", "*.nii*"), ("All files", "*.*")])
        if not path:
            print("load cancelled"); return
        print(f"Loading FA   : {path}")
        self.fa = FaVolume.load(path)
        self._fa_pp = None                      # stats FA cache is stale now
        for a in self.bg_actors:
            self.plotter.remove_actor(a)
        self.bg_actors = [self._add_fa_slice(s) for s in R.make_fa_slices(self.fa)]
        for a in self.bg_actors:
            a.SetVisibility(self.show_fa)
        self.plotter.render()
        print(f"  shape={self.fa.shape}  loaded FA {os.path.basename(path)}")

    def save(self):
        keep = int(self.alive.sum())
        if keep == 0:
            print("nothing alive to save"); return
        print(f"saving {keep:,}/{self.tg.n:,} surviving streamlines -> {self.args.out} ...")
        self.tg.save(self.args.out, self.alive, self.fa.img)
        self.dirty = False
        print(f"saved -> {self.args.out}")

    def quit(self):
        if self.dirty:
            print("WARNING: unsaved edits.  Press s to save, or Ctrl-C to abandon.")
        self.plotter.close()

    # ── statistics ────────────────────────────────────────────────────────────
    def preview_box(self):
        if self.box_bounds is None:
            print("Move the box first."); return
        n = int((self.selector.in_box(self.box_bounds) & self.alive).sum())
        alive = max(1, int(self.alive.sum()))
        print(f"box holds {n:,} alive streamlines ({100*n/alive:.1f}% of alive)  "
              f"-> 'd' deletes them, 'k' keeps only them")

    def _fa_pool(self):
        if self._fa_pp is None:                            # sample whole cloud once
            self._fa_pp = self.fa.sample(self.tg.X, self.tg.Y, self.tg.Z)
        vals = self._fa_pp[self.alive[self.tg.sid]]
        return vals[~np.isnan(vals)]

    def print_stats(self):
        idx = np.where(self.alive)[0]
        na = len(idx); nd = self.tg.n - na
        if na == 0:
            print("no alive streamlines"); return
        L = self.tg.arclen[idx]; npts = self.tg.lengths[idx]; fa = self._fa_pool()
        print("\n──────── STATISTICS  (surviving full set) ────────")
        print(f"  streamlines : alive {na:,}/{self.tg.n:,}   "
              f"deleted {nd:,} ({100*nd/self.tg.n:.1f}%)")
        print(f"  length (mm) : mean {L.mean():6.1f}  median {np.median(L):6.1f}  "
              f"sd {L.std():5.1f}  min {L.min():5.1f}  max {L.max():6.1f}")
        print(f"  points/line : mean {npts.mean():6.1f}  min {int(npts.min())}  "
              f"max {int(npts.max())}")
        if fa.size:
            print(f"  FA on tract : mean {fa.mean():.3f}  median {np.median(fa):.3f}  "
                  f"sd {fa.std():.3f}")
        if self.box_bounds is not None:
            nb = int((self.selector.in_box(self.box_bounds) & self.alive).sum())
            print(f"  in curr box : {nb:,} alive streamlines")
        print(f"  display cap : {self.display_n:,}  (shown now: {self.n_shown:,})")
        print("──────────────────────────────────────────────────\n")

    # ── run ───────────────────────────────────────────────────────────────────
    def run(self):
        print(f"Loading FA   : {self.args.fa}")
        self.fa = FaVolume.load(self.args.fa)
        print(f"  shape={self.fa.shape}  voxel={self.fa.zooms}")
        path = self.args.trk or I.ask_open_file()
        if not path:
            sys.exit("no tractogram loaded")
        print(f"Loading TRK  : {path}")
        if not self._load_tractogram(path):
            sys.exit("no tractogram loaded")

        self.plotter = pv.Plotter(window_size=(1280, 900))
        self.plotter.set_background("black")
        self.bg_actors = [self._add_fa_slice(s) for s in R.make_fa_slices(self.fa)]
        self.status = R.CornerText(self.plotter, "lower_left", "white", 10)
        self.line = R.LineLayer(self.plotter, self.tg,
                                disp_step=self.args.disp_step, seed=self.args.seed)
        I.setup_interaction(self)
        ui.setup_menu(self)
        self._rebuild(reset_camera=True)
        self.perf = PerfOverlay(self.plotter); self.perf.start()

        print("\nLaunching viewer ...")
        print("Hint: position the yellow box (white = selected), then 'd' or 'k'.")
        print("Use the on-screen buttons (top-left) to load tracts / FA or save.")
        self.plotter.show(title="tractography editor (local GPU)")

    def _add_fa_slice(self, mesh):
        return self.plotter.add_mesh(mesh, cmap="gray", opacity=0.55,
                                     show_scalar_bar=False, lighting=False,
                                     reset_camera=False)
