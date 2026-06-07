"""Rendering layer (PyVista/VTK): volume backdrop, the streamline line layer, HUD.

This is the only place streamline data meets VTK.  Performance notes that live
here on purpose (see CLAUDE.md, comment the *why*):
  - geometry is packed ONCE per resample; edits only blank dead points to NaN
    (the GPU discards NaN segments) -> positions re-upload, no re-pack;
  - highlight only rewrites the colour array (cheap) -> in-box lines turn white;
  - displayed points are decimated (every k-th) and per-streamline geometry is
    cached, so rebuilds are concatenation, not recomputation.
"""
from __future__ import annotations

import os

import numpy as np
import pyvista as pv


def _resolve_font(name="DejaVuSans.ttf"):
    """A clean TTF for HUD text (DejaVu ships with matplotlib); None if absent."""
    try:
        import matplotlib
        p = os.path.join(os.path.dirname(matplotlib.__file__),
                         "mpl-data", "fonts", "ttf", name)
        return p if os.path.exists(p) else None
    except Exception:
        return None


FONT_FILE = _resolve_font("DejaVuSans.ttf")            # UI labels / titles
MONO_FONT_FILE = _resolve_font("DejaVuSansMono.ttf")   # HUD readouts (aligned numbers)

# ── Theme ────────────────────────────────────────────────────────────────────
# One cohesive dark palette; tweak here, not scattered through the code.
BG_BOTTOM = (0.035, 0.045, 0.065)     # deep slate (window gradient bottom)
BG_TOP = (0.10, 0.12, 0.16)           # slightly lifted slate (gradient top)
ACCENT = (0.36, 0.82, 0.80)           # teal — title + selection box
TEXT = (0.86, 0.90, 0.96)             # primary readout text
TEXT_DIM = (0.55, 0.61, 0.70)         # secondary / hints
GOOD = (0.50, 0.85, 0.60)             # perf overlay (calm green, not harsh lime)
BOX = (1.0, 0.78, 0.25)               # selection box outline (warm gold)


def direction_rgb(streamline):
    """Per-segment direction colour (mrtrix-style |unit tangent|)."""
    if len(streamline) < 2:
        return np.zeros((len(streamline), 3), dtype=np.float32)
    d = np.empty_like(streamline)
    d[1:-1] = (streamline[2:] - streamline[:-2]) * 0.5
    d[0] = streamline[1] - streamline[0]
    d[-1] = streamline[-1] - streamline[-2]
    nrm = np.linalg.norm(d, axis=1, keepdims=True); nrm[nrm == 0] = 1.0
    return np.abs(d / nrm).astype(np.float32)


def make_volume_slices(vals):
    """Three orthogonal volume slices, placed in true world space.

    Build the grid in voxel-index space, slice it, then push each slice through
    the real affine -- a naive origin+spacing grid would drop the affine's x
    flip and place the backdrop ~120 mm off the tracts.
    """
    grid = pv.ImageData(dimensions=np.array(vals.shape) + 1,
                        spacing=(1.0, 1.0, 1.0), origin=(0.0, 0.0, 0.0))
    grid.cell_data["Volume"] = vals.arr.flatten(order="F")
    mid = [s // 2 for s in vals.shape]
    sx = grid.slice(normal="x", origin=(mid[0], 0, 0)).transform(vals.affine, inplace=False)
    sy = grid.slice(normal="y", origin=(0, mid[1], 0)).transform(vals.affine, inplace=False)
    sz = grid.slice(normal="z", origin=(0, 0, mid[2])).transform(vals.affine, inplace=False)
    return [sx, sy, sz]


class CornerText:
    """A single text actor pinned to a window corner, replaced on update."""

    def __init__(self, plotter, position="lower_left", color="white", font_size=10,
                 font_file=FONT_FILE):
        self.plotter = plotter
        self.position = position
        self.color = color
        self.font_size = font_size
        self.font_file = font_file
        self.actor = None

    def set(self, text):
        if self.actor is not None:
            self.plotter.remove_actor(self.actor)
        self.actor = self.plotter.add_text(text, position=self.position,
                                           color=self.color, font_size=self.font_size,
                                           font_file=self.font_file)


class LineLayer:
    """The displayed streamline actor: build once, then cheap NaN-hide + recolour."""

    def __init__(self, plotter, tg, disp_step=2, seed=0, line_width=1.6):
        self.plotter = plotter
        self.tg = tg
        self.step = max(1, int(disp_step))
        self.seed = seed
        self.line_width = line_width
        self._cache = {}            # fi -> (decimated pts float32, RGB uint8)
        self.actor = None
        self.hl_actor = None        # white "selected" overlay drawn on top of base
        self.pd = None
        self.pts0 = None            # (T_disp,3) "all visible" positions of the sample
        self.ptfid = None           # (T_disp,) streamline id per displayed point
        self.cellfid = None         # (n_cells,) streamline id per displayed line
        self.base_rgb = None        # (T_disp,3) base direction colours

    def reset_cache(self):
        self._cache.clear()

    def _geom(self, fi):
        g = self._cache.get(fi)
        if g is None:
            sd = self.tg.stream_pts(fi, self.step)
            if len(sd) < 2 and self.tg.lengths[fi] >= 2:   # keep the two endpoints
                sd = self.tg.stream_pts(fi)[[0, -1]]
            g = (np.ascontiguousarray(sd, np.float32),
                 (direction_rgb(sd) * 255).astype(np.uint8))
            self._cache[fi] = g
        return g

    def build(self, alive_mask, display_n, reset_camera=False):
        """Resample the surviving streamlines and (re)create the actor once."""
        alive_idx = np.where(alive_mask)[0]
        if len(alive_idx) > display_n:
            rng = np.random.default_rng(self.seed)        # deterministic resample
            sub = alive_idx[np.sort(rng.choice(len(alive_idx), display_n, replace=False))]
        else:
            sub = alive_idx
        pts = []; cols = []; lns = []; pf = []; cf = []; cur = 0
        for fi in sub:
            sd, rgb = self._geom(fi); m = len(sd)
            if m < 2:
                continue
            pts.append(sd); cols.append(rgb)
            pf.append(np.full(m, fi, np.int32)); cf.append(fi)
            ln = np.empty(m + 1, np.int64); ln[0] = m; ln[1:] = cur + np.arange(m)
            lns.append(ln); cur += m
        if self.actor is not None:
            self.plotter.remove_actor(self.actor); self.actor = None
        if self.hl_actor is not None:        # geometry changed -> drop stale overlay
            self.plotter.remove_actor(self.hl_actor); self.hl_actor = None
        if pts:
            self.pts0 = np.concatenate(pts).astype(np.float32)
            self.ptfid = np.concatenate(pf)
            self.cellfid = np.array(cf, dtype=np.int64)
            self.base_rgb = np.concatenate(cols)
            self.pd = pv.PolyData(self.pts0.copy(), lines=np.concatenate(lns))
            self.pd["RGB"] = self.base_rgb.copy()
            self.actor = self.plotter.add_mesh(self.pd, scalars="RGB", rgb=True,
                                               line_width=self.line_width,
                                               lighting=False, reset_camera=False)
        else:
            self.pd = None
            self.pts0 = self.ptfid = self.cellfid = self.base_rgb = None
        if reset_camera:
            self.plotter.reset_camera()

    def n_shown(self, alive_mask):
        return 0 if self.cellfid is None else int(alive_mask[self.cellfid].sum())

    def apply_alive(self, alive_mask):
        """Blank dead streamlines' points to NaN (positions re-upload, no re-pack)."""
        if self.pd is None or self.pts0 is None:
            return
        dead = ~alive_mask[self.ptfid]
        self.pd.points = (np.where(dead[:, None], np.nan, self.pts0).astype(np.float32)
                          if dead.any() else self.pts0)

    def _selected_polydata(self, selected, alive_mask):
        """PolyData of the in-box ALIVE displayed streamlines, or None."""
        if selected is None or self.cellfid is None:
            return None
        ids = self.cellfid[selected[self.cellfid] & alive_mask[self.cellfid]]
        if len(ids) == 0:
            return None
        pts = []; lns = []; cur = 0
        for fi in ids:
            sd, _ = self._geom(int(fi)); m = len(sd)
            if m < 2:
                continue
            pts.append(sd)
            ln = np.empty(m + 1, np.int64); ln[0] = m; ln[1:] = cur + np.arange(m)
            lns.append(ln); cur += m
        if not pts:
            return None
        return pv.PolyData(np.concatenate(pts), lines=np.concatenate(lns))

    def set_highlight(self, selected, alive_mask, render=True):
        """White overlay of the in-box ALIVE streamlines, drawn on top of the base.

        Reuse ONE overlay actor (swap its input) rather than recreating it every
        frame -- only the first highlight pays actor-creation cost, so dragging
        the box stays cheap.  The overlay lines are coincident with the base, so
        bias them toward the camera (polygon offset) to win the depth test.
        """
        pd = self._selected_polydata(selected, alive_mask)
        if pd is None:
            if self.hl_actor is not None:
                self.hl_actor.SetVisibility(False)
        elif self.hl_actor is None:
            self.hl_actor = self.plotter.add_mesh(
                pd, color="white", line_width=self.line_width + 1.6,
                lighting=False, reset_camera=False)
            try:
                mp = self.hl_actor.mapper
                mp.SetResolveCoincidentTopologyToPolygonOffset()
                mp.SetRelativeCoincidentTopologyLineOffsetParameters(-1.0, -1.0)
            except Exception:
                pass
        else:
            try:
                self.hl_actor.mapper.dataset = pd
            except Exception:
                self.hl_actor.GetMapper().SetInputData(pd)
            self.hl_actor.SetVisibility(True)
        if render:
            self.plotter.render()
