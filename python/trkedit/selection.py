"""Box selection over the streamline point cloud.  Pure numpy, no VTK.

Two interchangeable backends with the same API:

    in_box(bounds) -> bool array of length n_streamlines

A streamline is selected if ANY of its points lies in the axis-aligned box
(matching the delete/keep rule).  `LinearSelector` scans every point; the
default `GridSelector` buckets points into a uniform voxel grid and visits only
the buckets overlapping the box, so queries cost O(box) instead of O(all points)
-- fast enough to drive live highlight on the full set at interactive rates.
"""
from __future__ import annotations

import numpy as np


class LinearSelector:
    """One vectorized pass over all points.  No build cost, no extra memory."""

    def __init__(self, tg):
        self.X, self.Y, self.Z = tg.X, tg.Y, tg.Z
        self.sid = tg.sid
        self.n = tg.n

    def in_box(self, b):
        xm, xM, ym, yM, zm, zM = b
        m = self.X >= xm; m &= self.X <= xM
        m &= self.Y >= ym; m &= self.Y <= yM
        m &= self.Z >= zm; m &= self.Z <= zM
        flags = np.zeros(self.n, dtype=bool)
        flags[self.sid[m]] = True
        return flags


class GridSelector:
    """Uniform voxel grid index over the points (CSR bucket layout).

    Build: bin points into `cell`-mm voxels, sort point indices by bucket.
    Query: for each (ix,iy) column overlapping the box, the wanted iz range is a
    *contiguous* bucket span, so its points are one slice of the sorted order;
    gather those candidates and test them exactly.

    INVARIANT: the index is built ONCE over the full point cloud. Editing mutates
    only the alive mask -- it never moves or removes points -- so the index stays
    valid across edits and is deliberately NOT rebuilt on edit (the caller ANDs
    query flags with the alive mask). Kept in lockstep with the C++ backend.
    """

    def __init__(self, tg, cell=3.0):
        self.X, self.Y, self.Z = tg.X, tg.Y, tg.Z
        self.sid = tg.sid
        self.n = tg.n
        self.cell = float(cell)
        self.lo = np.array([tg.X.min(), tg.Y.min(), tg.Z.min()], dtype=np.float64)
        hi = np.array([tg.X.max(), tg.Y.max(), tg.Z.max()], dtype=np.float64)
        self.dims = np.maximum(1, np.floor((hi - self.lo) / self.cell).astype(np.int64) + 1)
        ix = self._cell_index(tg.X, 0)
        iy = self._cell_index(tg.Y, 1)
        iz = self._cell_index(tg.Z, 2)
        bucket = (ix * self.dims[1] + iy) * self.dims[2] + iz
        self.order = np.argsort(bucket, kind="stable").astype(np.int32)
        nb = int(self.dims[0] * self.dims[1] * self.dims[2])
        self.start = np.zeros(nb + 1, dtype=np.int64)
        np.cumsum(np.bincount(bucket, minlength=nb), out=self.start[1:])

    def _cell_index(self, coord, axis):
        i = np.floor((coord - self.lo[axis]) / self.cell).astype(np.int64)
        return np.clip(i, 0, self.dims[axis] - 1)

    def _range(self, lo_v, hi_v, axis):
        i0 = int(np.floor((lo_v - self.lo[axis]) / self.cell))
        i1 = int(np.floor((hi_v - self.lo[axis]) / self.cell))
        return max(0, i0), min(int(self.dims[axis]) - 1, i1)

    def in_box(self, b):
        xm, xM, ym, yM, zm, zM = b
        flags = np.zeros(self.n, dtype=bool)
        ix0, ix1 = self._range(xm, xM, 0)
        iy0, iy1 = self._range(ym, yM, 1)
        iz0, iz1 = self._range(zm, zM, 2)
        if ix0 > ix1 or iy0 > iy1 or iz0 > iz1:
            return flags
        dy, dz = int(self.dims[1]), int(self.dims[2])
        cand = []
        for ix in range(ix0, ix1 + 1):
            for iy in range(iy0, iy1 + 1):
                base = (ix * dy + iy) * dz
                s = self.start[base + iz0]
                e = self.start[base + iz1 + 1]      # contiguous iz span
                if e > s:
                    cand.append(self.order[s:e])
        if not cand:
            return flags
        c = np.concatenate(cand)
        cx, cy, cz = self.X[c], self.Y[c], self.Z[c]
        m = (cx >= xm) & (cx <= xM) & (cy >= ym) & (cy <= yM) & (cz >= zm) & (cz <= zM)
        flags[self.sid[c[m]]] = True
        return flags


def make_selector(name, tg):
    return GridSelector(tg) if name == "grid" else LinearSelector(tg)
