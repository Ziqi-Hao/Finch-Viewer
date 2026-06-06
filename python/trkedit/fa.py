"""FA background volume: NIfTI data + affine + world-point sampling.  No VTK.

Slice *geometry* for the backdrop is built in the render layer (it needs VTK);
this module is pure data so it can be reused by stats and any compute backend.
"""
from __future__ import annotations

import numpy as np
import nibabel as nib


class FaVolume:
    def __init__(self, img):
        self.img = img
        self.arr = img.get_fdata().astype(np.float32)
        self.affine = img.affine
        self.inv_aff = np.linalg.inv(img.affine)
        self.shape = self.arr.shape
        self.zooms = img.header.get_zooms()[:3]

    @classmethod
    def load(cls, path):
        return cls(nib.load(path))

    def sample(self, X, Y, Z):
        """FA at each world point (X,Y,Z); NaN outside the volume.  Vectorized
        over the SoA axes -- no (T,3) temporary, no per-streamline loop."""
        m = self.inv_aff
        vx = np.round(m[0, 0] * X + m[0, 1] * Y + m[0, 2] * Z + m[0, 3]).astype(np.int64)
        vy = np.round(m[1, 0] * X + m[1, 1] * Y + m[1, 2] * Z + m[1, 3]).astype(np.int64)
        vz = np.round(m[2, 0] * X + m[2, 1] * Y + m[2, 2] * Z + m[2, 3]).astype(np.int64)
        nx, ny, nz = self.shape
        good = (vx >= 0) & (vx < nx) & (vy >= 0) & (vy < ny) & (vz >= 0) & (vz < nz)
        out = np.full(len(X), np.nan, np.float32)
        out[good] = self.arr[vx[good], vy[good], vz[good]]
        return out
