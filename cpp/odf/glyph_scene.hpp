#pragma once

// Glyph/peaks SCENE building: turn a loaded ODF/peaks volume into drawable mesh
// data (interleaved vertices + indices + a world AABB), plus the small content
// classifiers the "Open" router uses to tell fODF / discrete-ODF / peaks apart.
//
// This is deliberately UI-free CPU meshing policy (brain masking, glyph budget,
// slice/stride fallback, DEC colouring) and lives in tracto_odf — NOT the Qt
// shell — so it stays unit-testable (odf_selftest) and reusable without Qt. The
// scene bounds use the module's own AABB (odf_types.hpp), keeping tracto_odf
// standalone; the Qt viewport converts AABB -> cpp/core Bounds at its boundary.

#include "odf_types.hpp"   // AABB, Vec3, VoxelIndex
#include "odf_volume.hpp"  // OdfVolume

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tracto {
namespace odf {

// A drawable ODF glyph scene: interleaved [pos.xyz, normal.xyz, rgb] vertices +
// a uint32 triangle index list + the world (RAS) AABB, plus metadata for the
// status line.
struct OdfGlyphScene {
  std::vector<float> vertices;         // 9 floats/vertex
  std::vector<std::uint32_t> indices;  // triangle list into `vertices`
  AABB bounds{};                       // world AABB (framed on the actual glyph verts)
  std::size_t glyphCount = 0;
  int lMax = 0;
  bool sliced = false;                 // true when only one axial slice is drawn
                                       // (whole-volume too big) -> can follow scrubbing
};

// A drawable peaks scene: interleaved [pos.xyz, rgb] line segments (2 verts each)
// + the world (RAS) AABB + metadata. Same vertex layout as the streamlines, so
// the viewport draws it with the existing line pipeline.
struct PeaksScene {
  std::vector<float> vertices;  // 6 floats/vertex; 2 verts (12 floats) per segment
  AABB bounds{};
  std::size_t segmentCount = 0;
  int nPeaks = 0;
  bool sliced = false;          // true when only one axial slice is drawn -> follows scrub
};

// Reconstruct a bounded set of SH-fODF glyphs on the CPU. Bounded on purpose: a
// whole-brain fODF has far too many voxels to draw every glyph, so above a budget
// only one axial slice (`sliceK`, or the centre when sliceK<0) is drawn, strided
// within it if even that is too big. `sliceK` lets the caller follow the scrub.
OdfGlyphScene BuildOdfGlyphScene(const OdfVolume& vol, int sliceK = -1);

// Discrete-sphere (SF) glyph scene: per-voxel amplitudes on a fixed embedded
// sphere (e.g. symmetric362), not SH. Always one axial slice (whole-brain data).
OdfGlyphScene BuildDiscreteOdfScene(const OdfVolume& vol, int sliceK = -1);

// DEC-coloured peak line segments from a 4-D peaks NIfTI (nCoeffs = 3·nPeaks).
// Each present peak becomes one bidirectional segment through the voxel centre,
// length proportional to its magnitude, coloured by |unit direction|.
PeaksScene BuildPeaksScene(const OdfVolume& vol, int sliceK = -1);

// True only for the canonical even-symmetric SH counts real fODF data uses
// (6,15,28,45,66,...) — excludes degenerate odd-symmetric solutions that are
// actually small peaks fields, so the router never glyphs a peaks file.
bool IsEvenSymmetricShCount(int nCoeffs);

// Content sanity check separating a real symmetric-SH fODF from a peaks/vector
// field that shares a coefficient count: a real fODF has an overwhelmingly
// positive l=0 (DC) coefficient in every voxel with signal.
bool LooksLikeFodf(const OdfVolume& vol);

// Voxel k (axial slice index) whose plane is nearest a world-Z. Assumes an
// axis-aligned affine (world-z depends only on k); clamped into [0, nz-1].
int AxialSliceIndex(const OdfVolume& vol, float worldZ);

}  // namespace odf
}  // namespace tracto
