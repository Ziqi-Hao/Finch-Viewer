#include "glyph_scene.hpp"

#include "discrete_sphere.hpp"  // embedded symmetric362 for sphere-sampled (SF) ODFs
#include "glyph_builder.hpp"
#include "icosphere.hpp"
#include "sh_basis.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace tracto {
namespace odf {
namespace {

// Pack a built GlyphMesh into a drawable scene: interleave [pos, normal, rgb] verts,
// copy the index list, and frame the bounds on the ACTUAL glyph vertices (a one-slice
// subset would otherwise be lost in the empty voxel grid). Shared by the SH and
// discrete-sphere builders. Leaves the scene empty (caller checks) if the mesh is empty.
void FillSceneFromMesh(OdfGlyphScene& scene, const GlyphMesh& mesh,
                       std::size_t glyphCount) {
  if (mesh.positions.empty()) return;
  scene.vertices.reserve(mesh.positions.size() * 9);
  for (std::size_t v = 0; v < mesh.positions.size(); ++v) {
    const auto& p = mesh.positions[v];
    const auto& n = mesh.normals[v];
    const auto& c = mesh.colors[v];
    scene.vertices.insert(scene.vertices.end(), {p.x, p.y, p.z, n.x, n.y, n.z, c.x, c.y, c.z});
  }
  scene.indices.assign(mesh.indices.begin(), mesh.indices.end());
  const auto& p0 = mesh.positions[0];
  float lo[3] = {p0.x, p0.y, p0.z}, hi[3] = {p0.x, p0.y, p0.z};
  for (const auto& p : mesh.positions) {
    lo[0] = std::min(lo[0], p.x); hi[0] = std::max(hi[0], p.x);
    lo[1] = std::min(lo[1], p.y); hi[1] = std::max(hi[1], p.y);
    lo[2] = std::min(lo[2], p.z); hi[2] = std::max(hi[2], p.z);
  }
  scene.bounds.min = {lo[0], lo[1], lo[2]};
  scene.bounds.max = {hi[0], hi[1], hi[2]};
  scene.glyphCount = glyphCount;
}

// Memoized (icosphere, SH basis matrix) for a given (subdiv, nCoeffs). Both are pure
// functions of those two, but BuildShBasisMatrix evaluates the SH basis at every sphere
// vertex (nDir·nCoeffs transcendental terms) — rebuilding it on every slice scrub is pure
// waste, since (subdiv, nCoeffs) are stable across scrubs of one volume. A one-entry cache
// (not a map) suffices: subdiv only flips at the 512-glyph budget boundary, rarely mid-scrub.
const std::pair<Icosphere, ShBasisMatrix>& CachedGlyphBasis(int subdiv, int nCoeffs) {
  static int cachedSubdiv = -1, cachedNCoeffs = -1;
  static std::pair<Icosphere, ShBasisMatrix> cache;
  if (subdiv != cachedSubdiv || nCoeffs != cachedNCoeffs) {
    Icosphere ico = MakeIcosphere(subdiv);
    ShBasisMatrix B = BuildShBasisMatrix(ico.vertices, InferShOrder(static_cast<std::size_t>(nCoeffs)));
    cache = {std::move(ico), std::move(B)};
    cachedSubdiv = subdiv;
    cachedNCoeffs = nCoeffs;
  }
  return cache;
}

}  // namespace

// Reconstruct a bounded set of ODF glyphs on the CPU (cpp/odf). Bounded on purpose:
// a whole-brain fODF has far too many voxels to draw every glyph (hundreds of verts
// each), so above a budget we show ONE axial slice (`sliceK`, or the centre when
// sliceK<0), striding within it if even that is too big. The 3x3x3 demo shows all 27.
// `sliceK` lets the caller follow the scrub focus (slice-following).
OdfGlyphScene BuildOdfGlyphScene(const OdfVolume& vol, int sliceK) {
  OdfGlyphScene scene;
  if (vol.empty() || vol.nCoeffs <= 0) return scene;

  const int nx = vol.dims[0], ny = vol.dims[1], nz = vol.dims[2];

  // Keep only voxels with real signal: skip those whose DC (l=0) coefficient is a
  // small fraction of the volume's peak DC — those are empty background. Coeff 0 of
  // a voxel is *voxelCoeffs(i,j,k) (the coeff-outermost layout puts c=0 first).
  float maxDc = 0.0f;
  for (int k = 0; k < nz; ++k)
    for (int j = 0; j < ny; ++j)
      for (int i = 0; i < nx; ++i) maxDc = std::max(maxDc, *vol.voxelCoeffs(i, j, k));
  const float dcThresh = 0.1f * maxDc;  // 10% of peak DC ~ a coarse brain mask

  std::vector<VoxelIndex> cand;
  for (int k = 0; k < nz; ++k)
    for (int j = 0; j < ny; ++j)
      for (int i = 0; i < nx; ++i)
        if (*vol.voxelCoeffs(i, j, k) > dcThresh) cand.push_back({i, j, k});

  // Budget: ~3000 glyphs keeps the mesh well under ~20 MB at subdiv 2/3.
  constexpr std::size_t kGlyphBudget = 3000;
  std::vector<VoxelIndex> chosen;
  if (cand.size() <= kGlyphBudget) {
    chosen = std::move(cand);  // small volume (e.g. the demo): every glyph
  } else {
    // Too many to draw all: one axial slice (the scrub focus, or the centre), strided
    // within if even that is over budget. An empty slice -> empty scene (the caller
    // just clears the glyphs while scrubbing past empty slices).
    scene.sliced = true;
    const int k0 = (sliceK >= 0 && sliceK < nz) ? sliceK : nz / 2;
    for (const VoxelIndex& v : cand)
      if (v.k == k0) chosen.push_back(v);
    if (chosen.size() > kGlyphBudget) {
      const std::size_t stride = (chosen.size() + kGlyphBudget - 1) / kGlyphBudget;
      std::vector<VoxelIndex> strided;
      for (std::size_t i = 0; i < chosen.size(); i += stride) strided.push_back(chosen[i]);
      chosen = std::move(strided);
    }
  }
  if (chosen.empty()) return scene;

  // Smoother spheres only when there are few glyphs (cost scales with verts*coeffs).
  const int subdiv = (chosen.size() <= 512) ? 3 : 2;
  const auto& [ico, B] = CachedGlyphBasis(subdiv, vol.nCoeffs);  // rebuilt only on (subdiv,nCoeffs) change

  // Glyph radius ~ half a voxel so neighbours don't overlap (voxel size = the
  // shortest affine column length, matching the ODF render prototype).
  auto colLen = [&](int c) {
    return std::sqrt(vol.affine.at(0, c) * vol.affine.at(0, c) +
                     vol.affine.at(1, c) * vol.affine.at(1, c) +
                     vol.affine.at(2, c) * vol.affine.at(2, c));
  };
  const float voxSize = std::min({colLen(0), colLen(1), colLen(2)});

  GlyphParams gp;
  gp.scale = 0.45f * voxSize;
  gp.normalizePerGlyph = true;
  gp.clampNegative = true;
  const GlyphMesh mesh = BuildGlyphs(vol, ico, B, chosen, gp);
  FillSceneFromMesh(scene, mesh, chosen.size());
  scene.lMax = B.order.lMax;
  return scene;
}

// Discrete-sphere (SF) glyph scene: per-voxel AMPLITUDES on a fixed embedded sphere
// (e.g. symmetric362), not SH. Always one axial slice (these are whole-brain). The
// signal mask scans the target slice CACHE-FRIENDLY: a slice's voxels are a CONTIGUOUS
// nx*ny run within each amplitude plane, so we accumulate the per-voxel max over the
// sphere's planes as sequential slabs (prefetcher-fast) — not a strided per-voxel gather.
OdfGlyphScene BuildDiscreteOdfScene(const OdfVolume& vol, int sliceK) {
  OdfGlyphScene scene;
  const Icosphere* sphere = DiscreteSphereForCount(vol.nCoeffs);
  if (vol.empty() || sphere == nullptr) return scene;
  scene.sliced = true;

  const int nx = vol.dims[0], ny = vol.dims[1], nz = vol.dims[2];
  const std::size_t spatial = vol.voxelCount();
  const std::size_t plane = static_cast<std::size_t>(nx) * ny;
  const int k0 = (sliceK >= 0 && sliceK < nz) ? sliceK : nz / 2;

  // Per-voxel max amplitude on slice k0, plane-by-plane over contiguous slabs.
  std::vector<float> sliceMax(plane, 0.0f);
  const float* base = vol.coeffs.data();
  const std::size_t sliceBase = static_cast<std::size_t>(k0) * plane;
  for (int v = 0; v < vol.nCoeffs; ++v) {
    const float* ampPlane = base + static_cast<std::size_t>(v) * spatial + sliceBase;  // nx*ny contiguous
    for (std::size_t p = 0; p < plane; ++p) sliceMax[p] = std::max(sliceMax[p], ampPlane[p]);
  }
  const float thresh = 0.1f * vol.valueMax;  // coarse mask vs the volume's global max amplitude

  std::vector<VoxelIndex> cand;
  for (std::size_t p = 0; p < plane; ++p)
    if (sliceMax[p] > thresh)
      cand.push_back({static_cast<int>(p % nx), static_cast<int>(p / nx), k0});

  // Budget (each glyph = 362 verts / 720 tris): stride within the slice if over.
  constexpr std::size_t kGlyphBudget = 3000;
  std::vector<VoxelIndex> chosen;
  if (cand.size() <= kGlyphBudget) {
    chosen = std::move(cand);
  } else {
    const std::size_t stride = (cand.size() + kGlyphBudget - 1) / kGlyphBudget;
    for (std::size_t i = 0; i < cand.size(); i += stride) chosen.push_back(cand[i]);
  }
  if (chosen.empty()) return scene;

  auto colLen = [&](int c) {
    return std::sqrt(vol.affine.at(0, c) * vol.affine.at(0, c) +
                     vol.affine.at(1, c) * vol.affine.at(1, c) +
                     vol.affine.at(2, c) * vol.affine.at(2, c));
  };
  GlyphParams gp;
  gp.scale = 0.45f * std::min({colLen(0), colLen(1), colLen(2)});
  gp.normalizePerGlyph = true;
  gp.clampNegative = true;
  FillSceneFromMesh(scene, BuildGlyphsSF(vol, *sphere, chosen, gp), chosen.size());
  scene.lMax = 0;  // not applicable to a discrete-sphere ODF
  return scene;
}

// Build DEC-coloured peak line segments from a 4-D peaks NIfTI (loaded via the ODF
// loader: coeff-outermost, nCoeffs = 3·nPeaks, so peak p of a voxel is the triplet
// at coeff indices 3p,3p+1,3p+2). Each present peak becomes one bidirectional segment
// through the voxel centre, length ∝ its magnitude, coloured by |unit direction|.
//
// EFFICIENCY (the user's hard requirement):
//  * The per-voxel signal mask scans only |peak0|, indexed by FLAT voxel v over the
//    three CONTIGUOUS component planes (coeffs[v], coeffs[spatial+v], coeffs[2·spatial+v])
//    — three sequential streams the prefetcher loves, NOT a strided (i,j,k) gather.
//  * Bounded like the glyphs: above a segment budget, central axial slice, then stride,
//    so a whole-brain field (millions of voxels) never explodes the vertex buffer.
//  * The only strided reads are the final gather over the BOUNDED chosen subset.
//  * `sliceK` (>=0) follows the scrub focus; <0 uses the central axial slice.
PeaksScene BuildPeaksScene(const OdfVolume& vol, int sliceK) {
  PeaksScene scene;
  const int nPeaks = vol.nCoeffs / 3;
  if (vol.empty() || nPeaks <= 0) return scene;
  scene.nPeaks = nPeaks;

  const int nx = vol.dims[0], ny = vol.dims[1], nz = vol.dims[2];
  const std::size_t spatial = vol.voxelCount();
  const float* d = vol.coeffs.data();  // component (3p+c) plane starts at (3p+c)*spatial

  // |peak0| per voxel = the signal gauge (the first/strongest direction). Two
  // sequential passes over the three component planes: max, then threshold.
  auto mag0 = [&](std::size_t v) {
    const float x = d[v], y = d[spatial + v], z = d[2 * spatial + v];
    return std::sqrt(x * x + y * y + z * z);
  };
  float maxMag = 0.0f;
  for (std::size_t v = 0; v < spatial; ++v) maxMag = std::max(maxMag, mag0(v));
  if (maxMag <= 0.0f) return scene;
  const float thresh = 0.1f * maxMag;  // coarse mask: 10% of the strongest peak

  std::vector<std::size_t> cand;
  for (std::size_t v = 0; v < spatial; ++v)
    if (mag0(v) > thresh) cand.push_back(v);

  // Budget the segments (each voxel emits up to nPeaks). Above it: central axial
  // slice, then stride — the same bounding strategy as the ODF glyphs.
  constexpr std::size_t kSegBudget = 60000;
  const std::size_t budgetVox = std::max<std::size_t>(1, kSegBudget / static_cast<std::size_t>(nPeaks));
  std::vector<std::size_t> chosen;
  if (cand.size() <= budgetVox) {
    chosen = std::move(cand);
  } else {
    scene.sliced = true;
    const std::size_t plane = static_cast<std::size_t>(nx) * ny;
    const int k0 = (sliceK >= 0 && sliceK < nz) ? sliceK : nz / 2;
    for (std::size_t v : cand)
      if (static_cast<int>(v / plane) == k0) chosen.push_back(v);
    if (chosen.size() > budgetVox) {
      const std::size_t stride = (chosen.size() + budgetVox - 1) / budgetVox;
      std::vector<std::size_t> strided;
      for (std::size_t i = 0; i < chosen.size(); i += stride) strided.push_back(chosen[i]);
      chosen = std::move(strided);
    }
  }
  if (chosen.empty()) return scene;

  // Half-length: half a voxel for the strongest peak, scaled by each peak's magnitude
  // so weaker peaks read shorter. voxSize = shortest affine column (matches the glyphs).
  auto colLen = [&](int c) {
    return std::sqrt(vol.affine.at(0, c) * vol.affine.at(0, c) +
                     vol.affine.at(1, c) * vol.affine.at(1, c) +
                     vol.affine.at(2, c) * vol.affine.at(2, c));
  };
  const float halfMax = 0.5f * std::min({colLen(0), colLen(1), colLen(2)});

  scene.vertices.reserve(chosen.size() * static_cast<std::size_t>(nPeaks) * 12);
  for (std::size_t v : chosen) {
    const int i = static_cast<int>(v % nx);
    const int j = static_cast<int>((v / nx) % ny);
    const int k = static_cast<int>(v / (static_cast<std::size_t>(nx) * ny));
    const auto ctr = VoxelToWorld(vol.affine, VoxelIndex{i, j, k});  // segment centre (world mm)
    for (int p = 0; p < nPeaks; ++p) {
      const float x = d[(3 * p + 0) * spatial + v];
      const float y = d[(3 * p + 1) * spatial + v];
      const float z = d[(3 * p + 2) * spatial + v];
      const float mag = std::sqrt(x * x + y * y + z * z);
      if (mag <= thresh) continue;  // absent / negligible peak
      const float inv = 1.0f / mag;
      const float ux = x * inv, uy = y * inv, uz = z * inv;
      const float h = halfMax * std::min(1.0f, mag / maxMag);  // length ∝ strength
      const float r = std::abs(ux), g = std::abs(uy), b = std::abs(uz);  // DEC colour
      scene.vertices.insert(scene.vertices.end(),  // bidirectional (peaks are antipodal)
                            {ctr.x - ux * h, ctr.y - uy * h, ctr.z - uz * h, r, g, b,
                             ctr.x + ux * h, ctr.y + uy * h, ctr.z + uz * h, r, g, b});
      ++scene.segmentCount;
    }
  }
  if (scene.vertices.empty()) return scene;

  // Tight bounds from the actual segment endpoints (like the glyph scene).
  float lo[3] = {scene.vertices[0], scene.vertices[1], scene.vertices[2]};
  float hi[3] = {lo[0], lo[1], lo[2]};
  for (std::size_t o = 0; o < scene.vertices.size(); o += 6) {
    lo[0] = std::min(lo[0], scene.vertices[o + 0]); hi[0] = std::max(hi[0], scene.vertices[o + 0]);
    lo[1] = std::min(lo[1], scene.vertices[o + 1]); hi[1] = std::max(hi[1], scene.vertices[o + 1]);
    lo[2] = std::min(lo[2], scene.vertices[o + 2]); hi[2] = std::max(hi[2], scene.vertices[o + 2]);
  }
  scene.bounds.min = {lo[0], lo[1], lo[2]};
  scene.bounds.max = {hi[0], hi[1], hi[2]};
  return scene;
}

// True only for the canonical even-symmetric SH counts real fODF data uses
// (6,15,28,45,66,...). InferShOrder also admits degenerate odd-symmetric solutions
// (e.g. nCoeffs=3 -> "lMax=1"), which are actually small peaks fields, not ODFs —
// exclude those so the router never glyphs a peaks file.
bool IsEvenSymmetricShCount(int nCoeffs) {
  if (nCoeffs < 6) return false;  // lMax>=2; nCoeffs=1 (lMax 0) is a scalar, not a glyph ODF
  try {
    const ShOrder o = InferShOrder(static_cast<std::size_t>(nCoeffs));
    return o.kind == ShBasisKind::Symmetric && o.lMax >= 2 && (o.lMax % 2 == 0);
  } catch (...) {
    return false;
  }
}

// Content sanity check separating a real symmetric-SH fODF from a peaks / vector
// field that happens to share a coefficient count (e.g. 15 = lMax-4 SH = 5 peaks,
// 6 = lMax-2 SH = 2 peaks). The headers carry no intent metadata to tell them
// apart, but a real fODF has a POSITIVE l=0 (DC) coefficient — the isotropic mean —
// in every voxel with signal, whereas a stacked vector field's "coeff 0" is a
// signed component (positive ~half the time). So: overwhelmingly-positive DC ⇒ fODF.
bool LooksLikeFodf(const OdfVolume& vol) {
  const int nx = vol.dims[0], ny = vol.dims[1], nz = vol.dims[2];
  const float thr = 0.05f * std::max(std::abs(vol.valueMin), std::abs(vol.valueMax));
  std::size_t signal = 0, positive = 0;
  for (int k = 0; k < nz; ++k)
    for (int j = 0; j < ny; ++j)
      for (int i = 0; i < nx; ++i) {
        const float dc = *vol.voxelCoeffs(i, j, k);  // coeff 0 of this voxel
        if (std::abs(dc) > thr) {
          ++signal;
          if (dc > 0.0f) ++positive;
        }
      }
  return signal != 0 && static_cast<double>(positive) / static_cast<double>(signal) >= 0.9;
}

// Voxel k (axial slice index) whose plane is nearest a world-Z. Assumes an
// axis-aligned affine (world-z depends only on k), true for typical RAS data; for a
// rotated affine this is an approximation. Clamped into [0, nz-1].
int AxialSliceIndex(const OdfVolume& vol, float worldZ) {
  const float a = vol.affine.at(2, 2);  // world-z per voxel-k step
  const float b = vol.affine.at(2, 3);  // world-z at k=0
  if (std::abs(a) < 1e-6f) return vol.dims[2] / 2;
  const int k = static_cast<int>(std::lround((worldZ - b) / a));
  return std::clamp(k, 0, vol.dims[2] - 1);
}

}  // namespace odf
}  // namespace tracto
