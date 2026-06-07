#pragma once

// CPU ODF glyph mesh builder.
//
// macOS-FRIENDLY REPLACEMENT FOR dmri-explorer's COMPUTE SHADER.
//   dmri-explorer reconstructs each glyph on the GPU (SSBO + compute shader),
//   which needs GL 4.3+. macOS caps OpenGL at 4.1 (no compute, no SSBO), so the
//   whole reconstruction is done here on the CPU instead: for each requested
//   voxel we deform the SHARED unit icosphere — radius(vertex) = sum_c
//   coeff[c] * basis[vertex][c] — place it at the voxel's world position, and
//   emit a ready-to-draw triangle mesh. A later GPU path (someone else's file)
//   can replace this, but the CPU path is the foundation and the source of
//   truth. Pure C++17 + the odf module headers; no GL, no Qt, no shaders.

#include "icosphere.hpp"   // Icosphere
#include "odf_types.hpp"   // Vec3, VoxelIndex
#include "odf_volume.hpp"  // OdfVolume
#include "sh_basis.hpp"    // ShBasisMatrix

#include <cstddef>
#include <vector>

namespace tracto {
namespace odf {

// A drawable, interleaved-by-array glyph mesh. All four arrays are parallel and
// indexed by `indices` (triangle list, 3 per face). Several glyphs are packed
// into one mesh: each glyph contributes icosphere.vertexCount() vertices and
// icosphere.indices.size() indices (the latter offset by the glyph's vertex
// base), so glyph g occupies vertices [g*V, (g+1)*V).
struct GlyphMesh {
  std::vector<Vec3> positions;     // world RAS mm
  std::vector<Vec3> normals;       // unit, per-vertex
  std::vector<Vec3> colors;        // RGB in [0,1], direction-encoded
  std::vector<unsigned> indices;   // triangle list into the arrays above

  std::size_t vertexCount() const { return positions.size(); }
  std::size_t triangleCount() const { return indices.size() / 3; }
  bool empty() const { return positions.empty(); }
};

// Parameters controlling how glyphs are sized/shaped. Grouped in a struct so the
// signature stays readable and the defaults document intent.
struct GlyphParams {
  // Multiplies the reconstructed radius. Combined with the voxel spacing implied
  // by the affine, this controls how large a glyph is relative to its cell.
  float scale = 1.0f;

  // If true, each glyph's radii are divided by that glyph's own max radius so
  // every glyph fills the same nominal size regardless of fODF magnitude
  // (good for shape comparison). If false, radii are comparable across voxels
  // (good for amplitude comparison) and only the per-volume range / `scale`
  // govern absolute size.
  bool normalizePerGlyph = true;

  // Negative reconstructed radii (lobes where the SH sum goes below zero) are
  // clamped to 0 by default — a glyph radius cannot be negative. Exposed so a
  // future signed/asymmetric mode can opt out.
  bool clampNegative = true;
};

// Build a single packed mesh for the given `voxelIndices` of `volume`.
//   icosphere     : shared unit sphere (sample directions + topology).
//   basisMatrix   : SH basis sampled at exactly icosphere.vertices, with a
//                   coefficient count matching volume.nCoeffs (built once via
//                   BuildShBasisMatrix and reused for every call).
//   params        : scale / per-glyph normalization / negative clamp.
//
// For each voxel: radius[v] = sum_c volume.voxelCoeffs(voxel)[c]*basis.row(v)[c]
// (the CPU matvec), deformed vertex = icosphere.vertices[v] * radius[v] * scale,
// world position = VoxelToWorld(volume.affine, voxel) + deformed vertex. Normals
// are recomputed from the deformed surface; colors are direction-encoded
// (see DirectionColor below). Preconditions:
//   basisMatrix.nDir == icosphere.vertexCount()
//   basisMatrix.nCoeffs == volume.nCoeffs
// Throws std::invalid_argument if these do not hold.
GlyphMesh BuildGlyphs(const OdfVolume& volume,
                      const Icosphere& icosphere,
                      const ShBasisMatrix& basisMatrix,
                      const std::vector<VoxelIndex>& voxelIndices,
                      const GlyphParams& params);

// Standard DTI direction-encoded color: |dir| componentwise mapped to RGB,
// i.e. R=|x|, G=|y|, B=|z| for a unit direction (the conventional fiber
// orientation coloring). `dir` is normalized internally. Exposed so the GPU
// path and tests share one definition.
Vec3 DirectionColor(Vec3 dir);

}  // namespace odf
}  // namespace tracto
