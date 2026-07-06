#pragma once

// VTK-free NIfTI-1 reader for the scalar background volume. This replaces the
// vtkNIFTIImageReader path the VTK editor used: it returns the voxels as float
// plus the voxel->RAS affine (sform, else qform, else pixdim scaling), so the
// Qt renderer can place orthogonal slices in the same RAS mm space as the
// streamlines. io module: no VTK, no Qt — only zlib for .nii.gz.

#include "bounds.hpp"    // Bounds (WorldBounds)
#include "mat_math.hpp"  // Mat4 (row-major affine)

#include <cstddef>
#include <string>
#include <vector>

namespace tracto {

struct Volume {
  int dims[3] = {0, 0, 0};      // nx, ny, nz
  std::vector<float> data;      // nx*ny*nz, x fastest (i + nx*(j + ny*k))
  Mat4 voxelToWorld;            // voxel (i,j,k) -> RAS mm (row-major)
  float valueMin = 0.0f;
  float valueMax = 1.0f;

  bool Empty() const { return data.empty(); }
  std::size_t VoxelCount() const { return data.size(); }
};

// Read a NIfTI-1 ".nii" / ".nii.gz" volume. Throws std::runtime_error on a
// malformed file or an unsupported variant (NIfTI-2, .hdr/.img pairs).
Volume LoadNifti(const std::string& path);

// Lightweight header-only peek: parses just the 348-byte NIfTI-1 header (no voxel
// read) to expose the shape + datatype. Used by the unified "Open…" router to tell
// a 3-D scalar volume from a 4-D SH-coefficient ODF or a peaks field WITHOUT loading
// the (possibly huge) data. Never throws — `ok` is false if the file is not a
// parseable NIfTI-1. dim[0] is the dimensionality; dim[1..3] are spatial; dim[4] is
// the 4th-axis length (SH coefficient count for ODFs, 3·Npeaks for peaks).
struct NiftiInfo {
  bool ok = false;
  int ndim = 0;          // dim[0]
  int dim[8] = {0};      // dim[0..7] in header order
  int datatype = 0;      // NIfTI datatype code (DT_FLOAT32 = 16, etc.)
  int intentCode = 0;    // NIFTI_INTENT_* (5=z, 3=t, 2=correl, 4=F…); 0 = none/unknown
};
NiftiInfo PeekNifti(const std::string& path);

// World-space (RAS mm) axis-aligned bounds of the volume's voxel grid, i.e. the
// 8 corners of [0..nx-1]x[0..ny-1]x[0..nz-1] under voxelToWorld.
Bounds WorldBounds(const Volume& volume);

}  // namespace tracto
