"""Streamline storage as a contiguous Structure-of-Arrays.  No VTK here.

Every point of every streamline lives in three contiguous per-axis arrays
(X/Y/Z) plus a per-point streamline id (sid); per-streamline (m,3) arrays are
reconstructed on demand via `offsets`.  This SoA layout makes selection and FA
sampling single vectorized passes (see CLAUDE.md: optimize from the data layout
outward) and lets editing/saving stay exact on the full set.
"""
from __future__ import annotations

import numpy as np
from dipy.io.streamline import load_tractogram, save_tractogram
from dipy.io.stateful_tractogram import StatefulTractogram, Space


class Tractogram:
    def __init__(self, X, Y, Z, sid, offsets, lengths):
        self.X = X                  # (T,) contiguous float32, per axis
        self.Y = Y
        self.Z = Z
        self.sid = sid              # (T,) int32, streamline id per point
        self.offsets = offsets      # (N+1,) int64, streamline i = [offsets[i]:offsets[i+1])
        self.lengths = lengths      # (N,) int64, points per streamline
        self.n = len(lengths)
        self.total_pts = len(X)
        self._arclen = None

    @classmethod
    def load(cls, path, reference):
        """Load a .trk into RASMM world space. Returns None if it has no lines."""
        sft = load_tractogram(path, reference)
        sft.to_rasmm()
        sl = sft.streamlines
        n = len(sl)
        if n == 0:
            return None
        lengths = np.array([len(s) for s in sl], dtype=np.int64)
        pts = np.concatenate([np.asarray(s, dtype=np.float32) for s in sl])  # (T,3) temp
        X = np.ascontiguousarray(pts[:, 0])
        Y = np.ascontiguousarray(pts[:, 1])
        Z = np.ascontiguousarray(pts[:, 2])
        sid = np.repeat(np.arange(n, dtype=np.int32), lengths)
        offsets = np.zeros(n + 1, dtype=np.int64)
        np.cumsum(lengths, out=offsets[1:])
        return cls(X, Y, Z, sid, offsets, lengths)

    def stream_pts(self, fi, step=1):
        """Reconstruct streamline `fi` as a contiguous (m,3) array."""
        a, b = self.offsets[fi], self.offsets[fi + 1]
        return np.column_stack((self.X[a:b:step], self.Y[a:b:step], self.Z[a:b:step]))

    @property
    def arclen(self):
        """Per-streamline arc length (mm), vectorized and cached."""
        if self._arclen is None:
            dx = np.diff(self.X); dy = np.diff(self.Y); dz = np.diff(self.Z)
            seg = np.sqrt(dx * dx + dy * dy + dz * dz)
            same = self.sid[1:] == self.sid[:-1]      # drop cross-streamline segments
            self._arclen = np.bincount(self.sid[1:][same], weights=seg[same],
                                       minlength=self.n)
        return self._arclen

    def extent(self):
        lo = np.array([self.X.min(), self.Y.min(), self.Z.min()])
        hi = np.array([self.X.max(), self.Y.max(), self.Z.max()])
        return lo, hi

    def save(self, path, alive_mask, reference):
        """Write the surviving streamlines exactly (no approximation)."""
        keep = np.where(alive_mask)[0]
        sft = StatefulTractogram([self.stream_pts(i) for i in keep], reference,
                                 space=Space.RASMM)
        save_tractogram(sft, path, bbox_valid_check=False)
        return len(keep)
