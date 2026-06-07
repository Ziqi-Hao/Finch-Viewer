#pragma once

// 4D SH-coefficient ODF volume loader (.nii.gz).
//
// SEPARATE CONCERN FROM cpp/core/nifti_io.cpp ON PURPOSE.
//   cpp/core/nifti_io.{hpp,cpp} loads a 3D *scalar* background (the scalar volume)
//   into tracto::Volume for slice rendering. This loader handles a 4D volume
//   whose 4th axis (NIfTI dim[4], the "t"/time dimension) is the SH coefficient
//   axis, and returns tracto::odf::OdfVolume. The two are kept standalone for
//   now: the ODF foundation is meant to be a self-contained CPU module, and
//   collapsing them would force the scalar path to grow a coefficient axis it does
//   not want. They MAY be merged later once both are stable; until then this
//   file does its own NIfTI-1 header parse + gzip read (zlib only, no GL/Qt).
//
// MEMORY LAYOUT (the source 4D file and our `coeffs` buffer):
//   The coefficient axis is OUTERMOST. For a volume of dims (nx, ny, nz) with
//   nCoeffs coefficients (nii_volume.h:95-118, 128-136):
//       flatIndex(i, j, k, c) = c*(nx*ny*nz) + k*(nx*ny) + j*nx + i
//   so x varies fastest, then y, then z, then the coefficient c (slowest).
//   Reading every coefficient of one voxel therefore strides by nx*ny*nz.
//
// AFFINE PRECEDENCE (nifti_io.cpp:254-269): sform (sform_code>0) > qform
//   (qform_code>0, quaternion) > diagonal pixdim. Stored row-major in `affine`
//   as voxel(i,j,k) -> world RAS mm, the same space as the scalar volume and
//   streamlines.
//
// DATATYPES handled: DT_FLOAT32 (16) and DT_FLOAT64 (64) at minimum — the demo
//   fODF (dmri-explorer/data/odf.nii.gz) is 3x3x3x45 Float64. Coefficients are
//   converted to float on load.

#include "odf_types.hpp"  // Vec3, Affine, AABB, VoxelIndex

#include <cstddef>
#include <string>
#include <vector>

namespace tracto {
namespace odf {

struct OdfVolume {
  int dims[3] = {0, 0, 0};       // nx, ny, nz (spatial)
  int nCoeffs = 0;               // SH coefficient count = NIfTI dim[4]
  std::vector<float> coeffs;     // size nx*ny*nz*nCoeffs, coeff-outermost (see above)

  Affine affine;                 // voxel(i,j,k) -> world RAS mm (row-major)
  AABB worldBounds;              // RAS-mm AABB of the 8 voxel-grid corners

  // Per-VOLUME coefficient value range across the whole buffer (min..max over
  // all coeffs of all voxels). A coarse normalizer / sanity gauge; per-glyph
  // radius normalization is a separate flag in glyph_builder.
  float valueMin = 0.0f;
  float valueMax = 0.0f;

  bool empty() const { return coeffs.empty(); }
  std::size_t voxelCount() const {
    return static_cast<std::size_t>(dims[0]) * dims[1] * dims[2];
  }

  // Flat index of coefficient c at voxel (i, j, k), coefficient-outermost.
  // Matches nii_volume.h:128-136. No bounds check (hot path).
  std::size_t coeffIndex(int i, int j, int k, int c) const {
    const std::size_t spatial = static_cast<std::size_t>(dims[0]) * dims[1] * dims[2];
    return static_cast<std::size_t>(c) * spatial +
           (static_cast<std::size_t>(k) * dims[1] + j) * dims[0] + i;
  }

  // Pointer to coefficient 0 of voxel (i, j, k). Successive coefficients are
  // spaced by voxelCount() floats (the coeff-outermost stride); a consumer that
  // wants them contiguous must gather with that stride.
  const float* voxelCoeffs(int i, int j, int k) const {
    return coeffs.data() + coeffIndex(i, j, k, 0);
  }
  const float* voxelCoeffs(const VoxelIndex& v) const {
    return voxelCoeffs(v.i, v.j, v.k);
  }
};

// Load a 4D SH-coefficient ODF NIfTI-1 (".nii" / ".nii.gz"). Parses the header,
// reads via zlib, applies the affine precedence above, fills worldBounds and the
// per-volume value range, and converts coefficients to float.
// Throws std::runtime_error on a malformed file, a non-4D image, or an
// unsupported datatype/variant (NIfTI-2, .hdr/.img pairs).
OdfVolume LoadOdfNifti(const std::string& path);

}  // namespace odf
}  // namespace tracto
