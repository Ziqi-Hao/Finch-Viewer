#include "main_window.hpp"

#include "display_geometry.hpp"
#include "nifti_io.hpp"
#include "track_density.hpp"
#include "properties_panel.hpp"
#include "layers_panel.hpp"
#include "selection_backend.hpp"
#include "statistics.hpp"
#include "tract_viewport.hpp"
#include "perf_overlay.hpp"
#include "trk_io.hpp"
#include "utils.hpp"
#include "viewport_hud.hpp"

// ODF (SH-coefficient) load + CPU glyph build — the standalone tracto_odf lib.
#include "odf_volume.hpp"
#include "sh_basis.hpp"
#include "icosphere.hpp"
#include "glyph_builder.hpp"
#include "discrete_sphere.hpp"  // embedded symmetric362 for sphere-sampled (SF) ODFs

#include <QAction>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDir>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QImage>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QUrl>

#include <chrono>
#include <cmath>
#include <cstdio>

#include <algorithm>
#include <exception>
#include <utility>

namespace tracto {
namespace {

// NIfTI intent codes that mark a per-voxel statistic (fMRI activation / FC maps).
// Used by the Open router as a positive hint that a 3-D scalar is a stat overlay,
// not anatomy. Many tools leave intent_code = 0 even for stats, so this only adds
// to the signed-values heuristic; it never vetoes it.
bool IsStatIntent(int code) {
  switch (code) {
    case 2:   // NIFTI_INTENT_CORREL (Pearson r)
    case 3:   // NIFTI_INTENT_TTEST
    case 4:   // NIFTI_INTENT_FTEST
    case 5:   // NIFTI_INTENT_ZSCORE
    case 6:   // NIFTI_INTENT_CHISQ
    case 7:   // NIFTI_INTENT_BETA (signed regression coefficients)
    case 22:  // NIFTI_INTENT_PVAL
    case 23:  // NIFTI_INTENT_LOGPVAL
    case 24:  // NIFTI_INTENT_LOG10PVAL
      return true;
    default:
      return false;
  }
}

// Short statistic-unit label for the stat colorbar, from the NIfTI intent_code.
QString StatUnits(int intentCode) {
  switch (intentCode) {
    case 2:  return QStringLiteral("r");     // CORREL
    case 3:  return QStringLiteral("t");     // TTEST
    case 4:  return QStringLiteral("F");     // FTEST
    case 5:  return QStringLiteral("z");     // ZSCORE
    case 6:  return QStringLiteral("χ²");    // CHISQ
    case 7:  return QStringLiteral("β");     // BETA
    case 22: case 23: case 24: return QStringLiteral("p");  // PVAL / LOGPVAL / LOG10PVAL
    default: return QStringLiteral("stat");  // signed map with no/unknown intent
  }
}

// A drawable ODF glyph scene handed to the viewport: interleaved
// [pos.xyz, normal.xyz, rgb] vertices + a uint32 triangle index list + the world
// (RAS) bounds, plus a little metadata for the status line.
struct OdfGlyphScene {
  std::vector<float> vertices;          // 9 floats/vertex
  std::vector<std::uint32_t> indices;   // triangle list into `vertices`
  Bounds bounds{};                      // world AABB (padded for the glyph radii)
  std::size_t glyphCount = 0;
  int lMax = 0;
  bool sliced = false;                  // true when only one axial slice is drawn
                                        // (whole-volume too big) -> can follow scrubbing
};

// Pack a built GlyphMesh into a drawable scene: interleave [pos, normal, rgb] verts,
// copy the index list, and frame the bounds on the ACTUAL glyph vertices (a one-slice
// subset would otherwise be lost in the empty voxel grid). Shared by the SH and
// discrete-sphere builders. Leaves the scene empty (caller checks) if the mesh is empty.
void FillSceneFromMesh(OdfGlyphScene& scene, const tracto::odf::GlyphMesh& mesh,
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
  scene.bounds.v[0] = lo[0]; scene.bounds.v[1] = hi[0];
  scene.bounds.v[2] = lo[1]; scene.bounds.v[3] = hi[1];
  scene.bounds.v[4] = lo[2]; scene.bounds.v[5] = hi[2];
  scene.glyphCount = glyphCount;
}

// Memoized (icosphere, SH basis matrix) for a given (subdiv, nCoeffs). Both are pure
// functions of those two, but BuildShBasisMatrix evaluates the SH basis at every sphere
// vertex (nDir·nCoeffs transcendental terms) — rebuilding it on every slice scrub is pure
// waste, since (subdiv, nCoeffs) are stable across scrubs of one volume. A one-entry cache
// (not a map) suffices: subdiv only flips at the 512-glyph budget boundary, rarely mid-scrub.
const std::pair<tracto::odf::Icosphere, tracto::odf::ShBasisMatrix>& CachedGlyphBasis(int subdiv,
                                                                                      int nCoeffs) {
  using namespace tracto::odf;
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

// Reconstruct a bounded set of ODF glyphs on the CPU (cpp/odf). Bounded on purpose:
// a whole-brain fODF has far too many voxels to draw every glyph (hundreds of verts
// each), so above a budget we show ONE axial slice (`sliceK`, or the centre when
// sliceK<0), striding within it if even that is too big. The 3x3x3 demo shows all 27.
// `sliceK` lets the caller follow the scrub focus (slice-following).
OdfGlyphScene BuildOdfGlyphScene(const tracto::odf::OdfVolume& vol, int sliceK = -1) {
  using namespace tracto::odf;
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
OdfGlyphScene BuildDiscreteOdfScene(const tracto::odf::OdfVolume& vol, int sliceK = -1) {
  using namespace tracto::odf;
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

// A drawable peaks scene: interleaved [pos.xyz, rgb] line segments (2 verts each)
// + the world (RAS) bounds + metadata for the status line. Same vertex layout as the
// streamlines, so the viewport draws it with the existing line pipeline.
struct PeaksScene {
  std::vector<float> vertices;  // 6 floats/vertex; 2 verts (12 floats) per segment
  Bounds bounds{};
  std::size_t segmentCount = 0;
  int nPeaks = 0;
  bool sliced = false;          // true when only one axial slice is drawn -> follows scrub
};

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
PeaksScene BuildPeaksScene(const tracto::odf::OdfVolume& vol, int sliceK = -1) {
  using namespace tracto::odf;
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
  scene.bounds.v[0] = lo[0]; scene.bounds.v[1] = hi[0];
  scene.bounds.v[2] = lo[1]; scene.bounds.v[3] = hi[1];
  scene.bounds.v[4] = lo[2]; scene.bounds.v[5] = hi[2];
  return scene;
}

// True only for the canonical even-symmetric SH counts real fODF data uses
// (6,15,28,45,66,...). InferShOrder also admits degenerate odd-symmetric solutions
// (e.g. nCoeffs=3 -> "lMax=1"), which are actually small peaks fields, not ODFs —
// exclude those so the router never glyphs a peaks file.
bool IsEvenSymmetricShCount(int t) {
  if (t < 6) return false;  // lMax>=2; nCoeffs=1 (lMax 0) is a scalar, not a glyph ODF
  try {
    const tracto::odf::ShOrder o = tracto::odf::InferShOrder(static_cast<std::size_t>(t));
    return o.kind == tracto::odf::ShBasisKind::Symmetric && o.lMax >= 2 && (o.lMax % 2 == 0);
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
bool LooksLikeFodf(const tracto::odf::OdfVolume& vol) {
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
int AxialSliceIndex(const tracto::odf::OdfVolume& vol, float worldZ) {
  const float a = vol.affine.at(2, 2);  // world-z per voxel-k step
  const float b = vol.affine.at(2, 3);  // world-z at k=0
  if (std::abs(a) < 1e-6f) return vol.dims[2] / 2;
  const int k = static_cast<int>(std::lround((worldZ - b) / a));
  return std::clamp(k, 0, vol.dims[2] - 1);
}

}  // namespace

MainWindow::MainWindow(Args args, QWidget* parent)
    : QMainWindow(parent), args_(std::move(args)) {
  setWindowTitle("Finch-Viewer — diffusion-MRI viewer");

  selection_ = CreateGridSelectionBackend();  // O(box) localized queries; built per load

  viewport_ = new TractViewport(this);
  setCentralWidget(viewport_);
  // Drive the live white in-box highlight: the viewport queries the grid backend
  // (fast, localized) as the box moves and lights up the streamlines inside it.
  viewport_->SetSelectionQuery(
      [this](const Bounds& box) { return selection_->SelectInBox(store_, box); });

  // Translucent counts overlay, top-left, over the viewport. Transparent to the
  // mouse so it never eats rotate/pan drags; fixed corner needs no reposition.
  hud_ = new ViewportHud(viewport_);
  hud_->move(12, 12);
  // Self-contained FPS/CPU/GPU diagnostics, top-right (parents to the viewport).
  new PerfOverlay(viewport_);

  // Drag any supported file onto the window to open it (auto-detected).
  setAcceptDrops(true);

  // Empty-state invite: centred over the blank viewport until data is loaded. Hosts
  // clickable shortcuts to load the bundled sample and to the controls cheatsheet.
  emptyHint_ = new QLabel(viewport_);
  emptyHint_->setObjectName("emptyHint");
  emptyHint_->setTextFormat(Qt::RichText);
  emptyHint_->setAlignment(Qt::AlignCenter);
  {
    const QString sample = SamplePath().isEmpty()
        ? QString()
        : "<a href='sample' style='color:#5aa0ff;text-decoration:none;'>Load a sample ODF</a> "
          "&nbsp;·&nbsp; ";
    emptyHint_->setText(
        "<div style='color:#9aa0aa;font-size:14px;'>"
        "<span style='font-size:22px;color:#d7dae0;'>Finch-Viewer</span><br><br>"
        "Drag a <b>.trk</b> or <b>NIfTI</b> file here, or press <b>⌘/Ctrl&nbsp;O</b> to Open.<br><br>"
        + sample +
        "<a href='help' style='color:#5aa0ff;text-decoration:none;'>Controls (?)</a></div>");
  }
  connect(emptyHint_, &QLabel::linkActivated, this, [this](const QString& link) {
    if (link == "sample") LoadSample();
    else if (link == "help") ShowControlsHelp();
  });
  viewport_->installEventFilter(this);  // re-centre emptyHint_ when the viewport resizes

  // One QAction per command, shared by the menu bar, the toolbar, and its
  // shortcut — Qt's single-source-of-truth idiom. Slots are defined below.
  auto act = [this](const QString& text, const QString& tip,
                    void (MainWindow::*slot)(), const QKeySequence& sc = {}) {
    auto* a = new QAction(text, this);
    a->setToolTip(tip);
    a->setStatusTip(tip);
    if (!sc.isEmpty()) a->setShortcut(sc);
    connect(a, &QAction::triggered, this, slot);
    return a;
  };

  // One unified entry point: pick any supported file and the router (Open ->
  // DetectAndLoad) figures out what it is — tractogram, scalar volume, or ODF.
  // The explicit per-type actions stay available under File ▸ "Open as" for the
  // cases auto-detect can't resolve (a label is just a 3-D integer volume).
  QAction* openAct = act("Open…", "Open any file — tractogram, volume, or ODF (auto-detected)",
                         &MainWindow::Open, QKeySequence::Open);
  QAction* openTrkAct = act("Tractogram (.trk)…", "Open a .trk tractogram", &MainWindow::OpenTrk);
  QAction* openVolumeAct = act("Volume (NIfTI)…", "Load a NIfTI background volume", &MainWindow::OpenVolume);
  QAction* openLabelAct = act("Label / segmentation…", "Load a NIfTI label / segmentation", &MainWindow::OpenLabel);
  QAction* saveAct = act("Save As…", "Write the surviving streamlines to .trk",
                         &MainWindow::SaveAs, QKeySequence::Save);
  QAction* deleteAct = act("Delete", "Delete kept streamlines inside the box (D)",
                           &MainWindow::DeleteInBox, Qt::Key_D);
  QAction* keepAct = act("Keep", "Keep only streamlines inside the box (K)",
                         &MainWindow::KeepInBox, Qt::Key_K);
  QAction* undoAct = act("Undo", "Undo the last edit (U)", &MainWindow::Undo, Qt::Key_U);
  QAction* resetBoxAct = act("Reset Box", "Re-place the selection box in the data (B)",
                             &MainWindow::ResetBox, Qt::Key_B);
  QAction* toggleVolumeAct = act("Volume", "Toggle the volume slice planes (H)",
                             &MainWindow::ToggleVolume, Qt::Key_H);  // single source for H

  // Re-frame the views. ⇧R resets all four panes; plain R (handled in the viewport)
  // resets only the pane under the cursor — so a single zoomed-in pane is recoverable
  // without disturbing the others.
  QAction* resetViewsAct = new QAction("Reset Views", this);
  resetViewsAct->setToolTip("Re-frame all four views (⇧R). Press R over one view to reset just that view.");
  resetViewsAct->setStatusTip(resetViewsAct->toolTip());
  resetViewsAct->setShortcut(QKeySequence("Shift+R"));
  connect(resetViewsAct, &QAction::triggered, this, [this] { if (viewport_) viewport_->ResetCamera(); });

  // Edit is an opt-in mode (View is the default core experience). Toggling it on
  // reveals the selection box + edit tools/cards; off returns to a clean view.
  editAct_ = new QAction("Edit", this);
  editAct_->setCheckable(true);
  editAct_->setToolTip("Edit mode: show the selection box + edit tools (E)");
  editAct_->setShortcut(Qt::Key_E);
  connect(editAct_, &QAction::toggled, this, &MainWindow::SetEditMode);
  editTools_ = {deleteAct, keepAct, undoAct, resetBoxAct};

  QMenu* fileMenu = menuBar()->addMenu("&File");
  fileMenu->addAction(openAct);
  QMenu* openAsMenu = fileMenu->addMenu("Open &as");  // explicit per-type fallbacks
  openAsMenu->addAction(openTrkAct);
  openAsMenu->addAction(openVolumeAct);
  openAsMenu->addAction(openLabelAct);
  fileMenu->addSeparator();
  fileMenu->addAction(saveAct);
  fileMenu->addSeparator();
  fileMenu->addAction("&Quit", this, &QWidget::close);

  QMenu* editMenu = menuBar()->addMenu("&Edit");
  editMenu->addAction(deleteAct);
  editMenu->addAction(keepAct);
  editMenu->addAction(undoAct);
  editMenu->addSeparator();
  editMenu->addAction(resetBoxAct);

  QMenu* viewMenu = menuBar()->addMenu("&View");
  viewMenu->addAction(resetViewsAct);
  viewMenu->addAction(toggleVolumeAct);
  viewMenu->addSeparator();
  viewMenu->addAction(editAct_);

  // Help: the controls cheatsheet (? / F1) + a one-click bundled sample.
  QAction* helpAct = new QAction("Controls", this);
  helpAct->setToolTip("Show the controls cheatsheet (?)");
  helpAct->setShortcuts({QKeySequence(Qt::Key_Question), QKeySequence(Qt::Key_F1)});
  connect(helpAct, &QAction::triggered, this, &MainWindow::ShowControlsHelp);
  QMenu* helpMenu = menuBar()->addMenu("&Help");
  helpMenu->addAction(helpAct);
  if (!SamplePath().isEmpty()) {
    QAction* sampleAct = new QAction("Open Sample ODF", this);
    sampleAct->setToolTip("Load the bundled demo fODF");
    connect(sampleAct, &QAction::triggered, this, &MainWindow::LoadSample);
    helpMenu->addAction(sampleAct);
  }

  // Compact, always-visible command strip mirroring the actions.
  QToolBar* toolbar = addToolBar("Main");
  toolbar->setMovable(false);
  toolbar->setFloatable(false);
  toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
  toolbar->addAction(openAct);  // one button, auto-detects the file type
  toolbar->addAction(saveAct);
  toolbar->addSeparator();
  toolbar->addAction(resetViewsAct);  // re-frame all views (discoverable; R = per-pane)
  toolbar->addSeparator();
  // Volume visibility now lives per-layer in the Layers panel, so the toolbar
  // toggle is redundant — keep it only as the View-menu item + H shortcut.
  toolbar->addAction(editAct_);  // the View/Edit toggle
  toolbar->addSeparator();
  toolbar->addAction(deleteAct);
  toolbar->addAction(keepAct);
  toolbar->addAction(undoAct);
  toolbar->addAction(resetBoxAct);
  if (QWidget* deleteButton = toolbar->widgetForAction(deleteAct))
    deleteButton->setObjectName("dangerButton");  // red-on-hover (destructive)

  // Right-dock inspector. Its edit buttons reuse the same QActions as the
  // toolbar; density/step changes route back here to rebuild the display.
  const EditActions editActions{deleteAct, keepAct, undoAct, resetBoxAct};
  properties_ = new PropertiesPanel(editActions);
  properties_->SetDensity(args_.displayN, args_.displayN);
  properties_->SetStep(args_.dispStep);
  connect(properties_, &PropertiesPanel::densityChanged, this, [this](int n) {
    args_.displayN = std::max(1, n);
    if (hasTracts_) RebuildDisplay();  // active only; overlays stay at load density
  });
  connect(properties_, &PropertiesPanel::stepChanged, this, [this](int s) {
    args_.dispStep = std::max(1, s);
    if (hasTracts_) RebuildDisplay();  // active only; overlays stay at load density
  });
  connect(properties_, &PropertiesPanel::refreshStatsRequested, this, &MainWindow::RefreshStats);
  connect(properties_, &PropertiesPanel::boxChanged, this, [this](const Bounds& box) {
    if (viewport_) viewport_->SetSelectionBox(box);  // typed bounds -> viewport box
  });
  connect(properties_, &PropertiesPanel::contrastRangeChanged, this, [this](double lo, double hi) {
    for (VolumeLayer& vl : volumes_) {
      if (vl.id != selectedVolumeId_) continue;
      vl.winLo = static_cast<float>(lo);  // remember per-volume
      vl.winHi = static_cast<float>(hi);
      if (viewport_) viewport_->SetImageParams(vl.id, vl.winLo, vl.winHi, vl.opacity);
      if (vl.isStat)  // keep the diverging legend in step with the dragged threshold/cap
        properties_->SetStatColorbar(true, lo, hi, StatUnits(vl.statIntent));
      break;
    }
  });
  connect(properties_, &PropertiesPanel::colormapChanged, this, [this](bool viridis) {
    for (VolumeLayer& vl : volumes_) {
      if (vl.id != selectedVolumeId_) continue;
      vl.viridis = viridis;  // remember per-volume (grayscale <-> viridis)
      if (viewport_) viewport_->SetImageViridis(vl.id, viridis);
      break;
    }
  });

  auto* scroll = new QScrollArea;
  scroll->setWidget(properties_);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  auto* dock = new QDockWidget("Properties", this);
  dock->setWidget(scroll);
  dock->setFeatures(QDockWidget::NoDockWidgetFeatures);  // a fixed pro-tool panel
  dock->setMinimumWidth(248);
  addDockWidget(Qt::RightDockWidgetArea, dock);

  // Left-dock Layers list (Volume / Tracts / Label; load many, check to show).
  layers_ = new LayersPanel;
  connect(layers_, &LayersPanel::visibilityChanged, this, &MainWindow::OnLayerVisibility);
  connect(layers_, &LayersPanel::opacityChanged, this, &MainWindow::OnLayerOpacity);
  auto* layersDock = new QDockWidget("Layers", this);
  layersDock->setWidget(layers_);
  layersDock->setFeatures(QDockWidget::NoDockWidgetFeatures);
  layersDock->setMinimumWidth(200);
  addDockWidget(Qt::LeftDockWidgetArea, layersDock);

  // The viewport owns the box and exposes no change signal, so poll it a few
  // times a second and mirror it into the panel only when it actually moved.
  auto* boxTimer = new QTimer(this);
  boxTimer->setInterval(100);
  connect(boxTimer, &QTimer::timeout, this, &MainWindow::PollSelectionReadout);
  boxTimer->start();

  // Slice-following: debounce a burst of scrub events (arrow keys / slice-plane drag)
  // into one ODF/peaks rebuild ~40 ms after the user pauses — keeps scrubbing smooth
  // while the glyphs catch up, instead of rebuilding on every step.
  sliceRebuildTimer_ = new QTimer(this);
  sliceRebuildTimer_->setSingleShot(true);
  connect(sliceRebuildTimer_, &QTimer::timeout, this, &MainWindow::RebuildSliceGlyphs);
  connect(viewport_, &TractViewport::sliceFocusChanged, this, [this] {
    if (odfVol_ || peaksVol_) sliceRebuildTimer_->start(40);
  });

  SetEditMode(false);  // start in View: edit tools disabled, Selection/Edit cards hidden
  statusBar()->showMessage("Open a file, drag one in, or press ? for controls.");
  UpdateEmptyHint();   // show the centred invite over the empty viewport
}

// Defined here (not =default in the header) so unique_ptr<odf::OdfVolume> destroys a
// COMPLETE type — odf_volume.hpp is included in this TU.
MainWindow::~MainWindow() = default;

void MainWindow::SetEditMode(bool on) {
  editMode_ = on;
  if (viewport_) viewport_->SetEditMode(on);
  if (properties_) properties_->SetEditMode(on);
  if (hud_) hud_->setVisible(on);  // the kept/total overlay is shown only while editing
  for (QAction* a : editTools_) {  // delete/keep/undo/reset: hidden AND disabled in view
    a->setVisible(on);             // hide the toolbar/menu buttons in view mode
    a->setEnabled(on);             // disabled => their shortcuts (D/K/U/B) don't fire either
  }
  if (editAct_ && editAct_->isChecked() != on) {     // keep the toggle in sync if set in code
    QSignalBlocker block(editAct_);
    editAct_->setChecked(on);
  }
  UpdateStatus();  // swap the status line between the view hint and the edit count
}

void MainWindow::LoadTractogram(const QString& path) {
  try {
    // Build a fresh bundle (load many TRKs; each is its own layer row).
    TractBundle b;
    b.store.streamlines = LoadTrk(path.toStdString(), b.store.header);
    BuildSoA(b.store);
    b.alive.assign(b.store.StreamlineCount(), 1);
    b.rasBounds = RasBounds(b.store);
    // Track-density map computed ONCE here from the full original streamlines; it
    // is cached and reused (independent of edits and the display-density slider).
    b.densityMap = BuildTrackDensity(b.store, b.alive, 1.0f);  // ~1 mm isotropic grid
    b.name = QFileInfo(path).fileName();
    b.path = path;
    const Bounds& rb = b.rasBounds;
    std::printf("loaded %s streamlines · RAS x[%.1f,%.1f] y[%.1f,%.1f] z[%.1f,%.1f]\n",
                FormatCount(b.store.StreamlineCount()).c_str(),
                rb.v[0], rb.v[1], rb.v[2], rb.v[3], rb.v[4], rb.v[5]);
    std::fflush(stdout);

    if (layers_) {
      // Tractograms blend (FSLeyes-style): a new one is visible + becomes the
      // active editable bundle, but does NOT hide the already-loaded ones.
      b.id = layers_->AddLayer(LayersPanel::Kind::Tracts, b.name, true);
    }
    tracts_.push_back(std::move(b));
    ActivateTracts(static_cast<int>(tracts_.size()) - 1);  // archives the previous active
    viewport_->SetTractsVisible(true);
    viewport_->ResetCamera();
  } catch (const std::exception& e) {
    ReportError("Load failed", e.what());
  }
}

void MainWindow::ActivateTracts(int index) {
  if (index < 0 || index >= static_cast<int>(tracts_.size()) || index == activeTracts_) return;
  // Archive the current active working state back into its bundle (move, no copy).
  if (activeTracts_ >= 0 && activeTracts_ < static_cast<int>(tracts_.size())) {
    TractBundle& cur = tracts_[activeTracts_];
    cur.store = std::move(store_);
    cur.alive = std::move(aliveFull_);
    cur.history = std::move(history_);
    cur.rasBounds = tractRasBounds_;
    cur.dirty = tractsDirty_;
    // It's now a non-active overlay: cache its display geometry (while it still
    // has its full SoA), then slim that SoA to free ~half its memory. Hidden
    // bundles need no overlay, so just slim them.
    if (cur.visible) {
      LineGeometry geo = BuildDisplayLineGeometry(cur.store, cur.alive, args_.displayN,
                                                  args_.dispStep, args_.seed);
      cur.overlayVerts = std::move(geo.vertices);
    } else {
      cur.overlayVerts.clear();
    }
    SlimStore(cur.store);  // free x/y/z/sid; RehydrateSoA rebuilds them on re-activate
  }
  // Swap the requested bundle into the active working members, rehydrating its
  // point cloud first if it was slimmed while inactive.
  TractBundle& nb = tracts_[index];
  RehydrateSoA(nb.store);  // no-op if its SoA is already present
  nb.overlayVerts.clear();  // active is drawn via lineVbo_, not as an overlay
  store_ = std::move(nb.store);
  aliveFull_ = std::move(nb.alive);
  history_ = std::move(nb.history);
  tractRasBounds_ = nb.rasBounds;
  tractsDirty_ = nb.dirty;
  args_.trkPath = nb.path.toStdString();
  hasTracts_ = !store_.x.empty();
  activeTracts_ = index;
  selection_->Build(store_);  // rebuild the grid index for the now-active tractogram
  RebuildDisplay();
  RebuildTractOverlays();  // the previously-active bundle (if visible) is now an overlay
  RebuildDensityMap();     // density map follows the active bundle
}

bool MainWindow::AnyTractsDirty() const {
  if (tractsDirty_) return true;  // the active one
  for (int i = 0; i < static_cast<int>(tracts_.size()); ++i)
    if (i != activeTracts_ && tracts_[i].dirty) return true;
  return false;
}

void MainWindow::OpenTrk() {
  const QString path =
      QFileDialog::getOpenFileName(this, "Open tractogram", StartDir(), "TrackVis (*.trk)");
  if (!path.isEmpty()) {
    RememberDir(path);
    LoadTractogram(path);
  }
}

void MainWindow::LoadVolume(const QString& path) { LoadVolumeLayer(path, false); }
void MainWindow::LoadLabel(const QString& path) { LoadVolumeLayer(path, true); }

void MainWindow::LoadVolumeLayer(const QString& path, bool isLabel) {
  // A label is also a scalar NIfTI; for now it renders through the same volume
  // path (grayscale). Per-label colouring is Stage 4. Both share the single
  // volume render slot, so checking one makes it the active rendered image.
  try {
    Volume volume = LoadNifti(path.toStdString());
    const Bounds wb = WorldBounds(volume);
    const int dx = volume.dims[0], dy = volume.dims[1], dz = volume.dims[2];
    std::printf("loaded %s %dx%dx%d · range[%.3f,%.3f] · RAS x[%.1f,%.1f] y[%.1f,%.1f] z[%.1f,%.1f]\n",
                isLabel ? "label" : "volume", dx, dy, dz, volume.valueMin, volume.valueMax,
                wb.v[0], wb.v[1], wb.v[2], wb.v[3], wb.v[4], wb.v[5]);
    std::fflush(stdout);

    args_.volumePath = path.toStdString();
    const QString name = QFileInfo(path).fileName();
    const float vmin = volume.valueMin, vmax = volume.valueMax;  // window default before move

    // fMRI stat-overlay auto-detect: a *signed* scalar (meaningful negatives) or a
    // NIfTI intent_code marking a statistic is a z/t/r activation map, not anatomy
    // (T1/EPI/FA/MD are non-negative). Render it diverging + thresholded rather than
    // grayscale. Labels never qualify — they arrive through the explicit label path.
    const NiftiInfo peek = PeekNifti(path.toStdString());  // header-only: intent_code
    const float dynRange = std::max(1.0e-6f, vmax - vmin);
    const bool signedVals = (vmin / dynRange) < -0.01f;    // negative lobe > 1% of the range
    const bool isStat = !isLabel && (signedVals || IsStatIntent(peek.intentCode));

    // Add a layer row in the right group, visible by default. Layers blend
    // (FSLeyes-style), so loading one does NOT hide the others; it just becomes
    // the layer the Contrast panel edits.
    const auto kind = isStat ? LayersPanel::Kind::Stat
                             : (isLabel ? LayersPanel::Kind::Label : LayersPanel::Kind::Volume);
    const int id = layers_->AddLayer(kind, name, true);
    VolumeLayer layer{std::move(volume), name, id, isLabel, vmin, vmax};
    layer.isStat = isStat;
    layer.statIntent = isStat ? peek.intentCode : 0;
    if (isStat)
      ComputeStatHistogram(layer, peek.intentCode);  // |stat| bins + symmetric threshold/cap
    else
      ComputeHistogram(layer);  // grayscale window: histBins + adaptive [dispMin,dispMax]
    if (isLabel) ComputeLabelLut(layer);  // per-label colour table for label-mode rendering
    volumes_.push_back(std::move(layer));  // keep our copy (data + histogram + window)
    selectedVolumeId_ = id;
    const VolumeLayer& added = volumes_.back();
    viewport_->SetImage(id, added.vol, added.isLabel, added.lut, added.lutWidth);
    viewport_->SetImageParams(id, added.winLo, added.winHi, added.opacity);
    if (isStat) {
      viewport_->SetImageStatmap(id, true);    // diverging hot/cool ramp + |stat| threshold
      viewport_->SetImageOrthoOnly(id, true);  // 2-D panes only (avoid 3-D co-planar z-fighting)
    }
    viewport_->SetImageVisible(id, true);
    UpdateInfo();
    UpdateHistogram();

    // Space sanity: an image in a different space from the tractogram lands
    // off-screen (the outputWarped vs SUBG08 mixup). Warn instead of confusing.
    const bool mismatch =
        hasTracts_ &&
        (wb.v[1] < tractRasBounds_.v[0] || wb.v[0] > tractRasBounds_.v[1] ||
         wb.v[3] < tractRasBounds_.v[2] || wb.v[2] > tractRasBounds_.v[3] ||
         wb.v[5] < tractRasBounds_.v[4] || wb.v[4] > tractRasBounds_.v[5]);
    if (mismatch) {
      statusBar()->showMessage(
          "⚠ Image does not overlap the streamlines — likely a different space.", 12000);
    } else {
      const char* what = isStat ? "Stat overlay" : (isLabel ? "Label" : "Volume");
      statusBar()->showMessage(
          QString("%1 loaded: %2×%3×%4").arg(what).arg(dx).arg(dy).arg(dz), 8000);
    }
  } catch (const std::exception& e) {
    ReportError(isLabel ? "Label load failed" : "Volume load failed", e.what());
  }
}

void MainWindow::OpenVolume() {
  const QString path = QFileDialog::getOpenFileName(this, "Open volume", StartDir(),
                                                    "NIfTI (*.nii *.nii.gz)");
  if (!path.isEmpty()) { RememberDir(path); LoadVolume(path); }
}

void MainWindow::OpenLabel() {
  const QString path = QFileDialog::getOpenFileName(this, "Open label / segmentation", StartDir(),
                                                    "NIfTI (*.nii *.nii.gz)");
  if (!path.isEmpty()) { RememberDir(path); LoadLabel(path); }
}

void MainWindow::Open() {
  // Multi-select: pick several files at once and load each (auto-detected). Starts
  // in the last-used folder and remembers wherever you end up.
  const QStringList paths = QFileDialog::getOpenFileNames(
      this, "Open", StartDir(),
      "All supported (*.trk *.nii *.nii.gz);;TrackVis (*.trk);;NIfTI (*.nii *.nii.gz)");
  if (paths.isEmpty()) return;
  RememberDir(paths.first());
  for (const QString& path : paths) DetectAndLoad(path);
}

// ── File-dialog folder memory (persisted across runs via QSettings) ──────────
QString MainWindow::StartDir() const {
  return QSettings().value("lastDir").toString();  // empty -> Qt's default (cwd/home)
}
void MainWindow::RememberDir(const QString& path) {
  if (!path.isEmpty()) QSettings().setValue("lastDir", QFileInfo(path).absolutePath());
}

void MainWindow::DetectAndLoad(const QString& path) {
  // Route by extension, then (for NIfTI) by a header-only peek at the shape: a 3-D
  // image is the scalar background; a 4-D image is an SH-coefficient ODF (drawn as
  // glyphs) or a peaks field (Nx3 directions). The user never pre-classifies a file.
  const QString lower = path.toLower();
  if (lower.endsWith(".trk")) { LoadTractogram(path); return; }
  if (!(lower.endsWith(".nii") || lower.endsWith(".nii.gz"))) {
    ReportError("Open failed", "Unsupported file type (expected .trk, .nii, or .nii.gz):\n" + path);
    return;
  }

  const NiftiInfo info = PeekNifti(path.toStdString());
  if (!info.ok) {
    ReportError("Open failed", "Not a readable NIfTI-1 file:\n" + path);
    return;
  }
  const int t = info.ndim >= 4 ? info.dim[4] : 1;  // length of the 4th axis
  if (t <= 1) { LoadVolume(path); return; }        // 3-D scalar background

  if (IsEvenSymmetricShCount(t)) { LoadOdf(path); return; }  // SH ODF (LoadOdf re-checks vs peaks)
  if (t % 3 == 0) { LoadPeaks(path); return; }               // Nx3 peaks field
  // A 4-D image that is neither SH-ODF nor peaks. A large 4th axis (362, 642, 724, …)
  // is a DISCRETE-SPHERE ODF: per-voxel amplitudes sampled on a fixed sphere whose
  // directions are NOT in the NIfTI. We HAVE the SF glyph renderer + embed dipy's
  // symmetric362, BUT we deliberately do NOT auto-render here: verified against the
  // subject's peaks, the 362-direction RUMBA file does NOT match symmetric362 under any
  // axis convention (best 48°), so blindly assuming that sphere would orient every glyph
  // WRONG. Render only via the explicit `--discrete-odf` opt-in once the sphere is
  // confirmed. See LoadDiscreteOdf / discrete_sphere.hpp.
  if (t >= 100) {
    std::printf("discrete-sphere ODF: %d directions — sphere not auto-assumable "
                "(symmetric362 unverified for this file); showing the first volume\n", t);
    std::fflush(stdout);
    statusBar()->showMessage(
        QString("Discrete-sphere ODF detected (%1 directions/voxel) — its sphere isn't "
                "known/verified, so glyphs aren't auto-rendered; showing the first volume.")
            .arg(t), 14000);
  } else {
    statusBar()->showMessage(
        QString("4-D NIfTI (4th axis = %1) is neither ODF nor peaks — loading as a volume.").arg(t),
        10000);
  }
  LoadVolume(path);
}

void MainWindow::LoadOdf(const QString& path) {
  try {
    tracto::odf::OdfVolume vol = tracto::odf::LoadOdfNifti(path.toStdString());
    // Confirm it's really an fODF and not a peaks/vector field sharing the same
    // coefficient count (the header can't tell them apart — content can). If it's a
    // peaks field, draw it as peaks from THIS already-loaded volume (no second read).
    if (!LooksLikeFodf(vol)) {
      if (vol.nCoeffs % 3 == 0) {
        std::printf("4-D image (%d coeffs) is a peaks/vector field, not an fODF — "
                    "drawing as peaks\n", vol.nCoeffs);
        std::fflush(stdout);
        const bool sliced = DisplayPeaks(vol, path);  // keep resident only if slice-following
        peaksVol_ = sliced ? std::make_unique<tracto::odf::OdfVolume>(std::move(vol)) : nullptr;
      } else {
        statusBar()->showMessage(
            QString("4-D image (dim4=%1) does not look like an fODF — not rendered.")
                .arg(vol.nCoeffs), 12000);
      }
      return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    OdfGlyphScene scene = BuildOdfGlyphScene(vol);
    const double buildMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (scene.glyphCount == 0 || scene.indices.empty()) {
      ReportError("ODF load failed",
                  "No glyphs to draw (the volume looks empty after thresholding):\n" + path);
      return;
    }
    std::printf("loaded ODF %dx%dx%d nCoeffs=%d · lMax=%d · %zu glyphs · build %.1f ms\n",
                vol.dims[0], vol.dims[1], vol.dims[2], vol.nCoeffs, scene.lMax, scene.glyphCount, buildMs);
    std::fflush(stdout);

    args_.volumePath = path.toStdString();
    const std::size_t glyphs = scene.glyphCount;
    const int lmax = scene.lMax;
    viewport_->SetGlyphGeometry(std::move(scene.vertices), std::move(scene.indices), scene.bounds);
    viewport_->SetGlyphsVisible(true);
    // One ODF layer at a time: a new load replaces the old Layers row (+ checkbox).
    if (layers_) {
      if (odfLayerId_ >= 0) layers_->RemoveLayer(odfLayerId_);
      odfLayerId_ = layers_->AddLayer(LayersPanel::Kind::Odf, QFileInfo(path).fileName(), true);
    }
    // Frame on the glyphs when nothing else anchors the view.
    if (!hasTracts_ && volumes_.empty()) viewport_->ResetCamera();
    statusBar()->showMessage(QString("ODF loaded: %1×%2×%3, lmax=%4, %5 glyphs")
                                 .arg(vol.dims[0]).arg(vol.dims[1]).arg(vol.dims[2])
                                 .arg(lmax).arg(glyphs),
                             8000);
    odfInfo_ = QStringLiteral("%1\n         %2 glyphs (lmax %3)")
                   .arg(QFileInfo(path).fileName()).arg(glyphs).arg(lmax);
    UpdateInfo();
    // Keep the volume resident ONLY when sliced (so scrubbing can rebuild the glyph
    // slice); a whole-shown small ODF needs no rebuild, so free it. (vol read above.)
    odfVol_ = scene.sliced ? std::make_unique<tracto::odf::OdfVolume>(std::move(vol)) : nullptr;
    odfIsDiscrete_ = false;  // SH ODF -> rebuild via the SH path
  } catch (const std::exception& e) {
    ReportError("ODF load failed", e.what());
  }
}

void MainWindow::LoadDiscreteOdf(const QString& path) {
  // Sphere-sampled (SF) ODF: per-voxel amplitudes on an embedded sphere (symmetric362).
  // Renders through the SAME glyph layer as the SH ODF (Layers row, slice-following) —
  // only the mesh builder differs (BuildGlyphsSF, no SH basis).
  try {
    tracto::odf::OdfVolume vol = tracto::odf::LoadOdfNifti(path.toStdString());
    OdfGlyphScene scene = BuildDiscreteOdfScene(vol);
    if (scene.glyphCount == 0 || scene.indices.empty()) {
      ReportError("ODF load failed",
                  "No glyphs to draw (the volume looks empty after thresholding):\n" + path);
      return;
    }
    std::printf("loaded discrete-sphere ODF %dx%dx%d · %d directions · %zu glyphs\n",
                vol.dims[0], vol.dims[1], vol.dims[2], vol.nCoeffs, scene.glyphCount);
    std::fflush(stdout);

    args_.volumePath = path.toStdString();
    const std::size_t glyphs = scene.glyphCount;
    const int ndir = vol.nCoeffs;
    viewport_->SetGlyphGeometry(std::move(scene.vertices), std::move(scene.indices), scene.bounds);
    viewport_->SetGlyphsVisible(true);
    if (layers_) {
      if (odfLayerId_ >= 0) layers_->RemoveLayer(odfLayerId_);
      odfLayerId_ = layers_->AddLayer(LayersPanel::Kind::Odf, QFileInfo(path).fileName(), true);
    }
    if (!hasTracts_ && volumes_.empty()) viewport_->ResetCamera();
    statusBar()->showMessage(
        QString("ODF loaded (sphere-sampled): %1×%2×%3, %4 directions, %5 glyphs")
            .arg(vol.dims[0]).arg(vol.dims[1]).arg(vol.dims[2]).arg(ndir).arg(glyphs),
        8000);
    odfInfo_ = QStringLiteral("%1\n         %2 glyphs (%3-dir sphere)")
                   .arg(QFileInfo(path).fileName()).arg(glyphs).arg(ndir);
    UpdateInfo();
    // Whole-brain -> always sliced; keep resident for slice-following, flagged discrete.
    odfVol_ = std::make_unique<tracto::odf::OdfVolume>(std::move(vol));
    odfIsDiscrete_ = true;
  } catch (const std::exception& e) {
    ReportError("ODF load failed", e.what());
  }
}

bool MainWindow::DisplayPeaks(const odf::OdfVolume& vol, const QString& path) {
  // Build + show peaks from an ALREADY-LOADED volume. Shared by LoadPeaks (clean peaks
  // files) and LoadOdf's not-fODF fallback (ambiguous SH-count files that are really
  // peaks) — so an ambiguous peaks file is read from disk exactly once. Returns whether
  // the scene is a single slice (the caller then keeps the volume resident to scrub).
  PeaksScene scene = BuildPeaksScene(vol);  // central slice on first show; scrub follows
  if (scene.segmentCount == 0 || scene.vertices.empty()) {
    ReportError("Peaks load failed",
                "No peak directions to draw (the field looks empty):\n" + path);
    return false;
  }
  std::printf("loaded peaks %dx%dx%d · %d dirs/voxel · %zu segments\n",
              vol.dims[0], vol.dims[1], vol.dims[2], scene.nPeaks, scene.segmentCount);
  std::fflush(stdout);

  args_.volumePath = path.toStdString();
  const std::size_t segs = scene.segmentCount;
  const int np = scene.nPeaks;
  viewport_->SetPeaksGeometry(std::move(scene.vertices), scene.bounds);
  viewport_->SetPeaksVisible(true);
  // One peaks layer at a time: a new load replaces the old Layers row (+ checkbox).
  if (layers_) {
    if (peaksLayerId_ >= 0) layers_->RemoveLayer(peaksLayerId_);
    peaksLayerId_ = layers_->AddLayer(LayersPanel::Kind::Peaks, QFileInfo(path).fileName(), true);
  }
  if (!hasTracts_ && volumes_.empty()) viewport_->ResetCamera();  // frame on the peaks
  statusBar()->showMessage(QString("Peaks loaded: %1×%2×%3, %4 dirs/voxel, %5 segments")
                               .arg(vol.dims[0]).arg(vol.dims[1]).arg(vol.dims[2]).arg(np).arg(segs),
                           8000);
  peaksInfo_ = QStringLiteral("%1\n         %2 segments (%3 dirs/voxel)")
                   .arg(QFileInfo(path).fileName()).arg(segs).arg(np);
  UpdateInfo();
  return scene.sliced;
}

void MainWindow::LoadPeaks(const QString& path) {
  try {
    tracto::odf::OdfVolume vol = tracto::odf::LoadOdfNifti(path.toStdString());
    const bool sliced = DisplayPeaks(vol, path);
    // Keep resident only when slice-following (a whole-shown small field needs no rebuild).
    peaksVol_ = sliced ? std::make_unique<tracto::odf::OdfVolume>(std::move(vol)) : nullptr;
  } catch (const std::exception& e) {
    ReportError("Peaks load failed", e.what());
  }
}

void MainWindow::RebuildSliceGlyphs() {
  // Rebuild whichever resident ODF / peaks layer at the slice nearest the scrub focus.
  // Geometry-only updates (no camera re-frame); an empty slice clears that layer. The
  // debounce timer already coalesced the scrub burst, so this runs at most ~25 Hz.
  if (!viewport_ || (!odfVol_ && !peaksVol_)) return;
  const float z = viewport_->SliceFocus().z;
  if (odfVol_) {
    const int k = AxialSliceIndex(*odfVol_, z);
    OdfGlyphScene s = odfIsDiscrete_ ? BuildDiscreteOdfScene(*odfVol_, k)
                                     : BuildOdfGlyphScene(*odfVol_, k);
    viewport_->SetGlyphGeometry(std::move(s.vertices), std::move(s.indices), s.bounds);
  }
  if (peaksVol_) {
    PeaksScene s = BuildPeaksScene(*peaksVol_, AxialSliceIndex(*peaksVol_, z));
    viewport_->SetPeaksGeometry(std::move(s.vertices), s.bounds);
  }
}

void MainWindow::ToggleVolume() {
  if (!viewport_) return;
  // Toolbar shortcut: a global master switch for all slice layers (the per-layer
  // checkboxes in the Layers panel control individual images).
  viewport_->SetVolumeVisible(!viewport_->VolumeVisible());
}

void MainWindow::OnLayerVisibility(int id, bool on) {
  // ODF glyph / peaks layers: a single layer each, toggled straight on the viewport.
  if (id == odfLayerId_) { viewport_->SetGlyphsVisible(on); return; }
  if (id == peaksLayerId_) { viewport_->SetPeaksVisible(on); return; }
  // Image layers (volumes + labels) blend independently: toggling one only
  // changes its own visibility. Checking one also makes it the selected layer
  // for the Contrast panel; unchecking the selected one clears that selection.
  for (const VolumeLayer& vl : volumes_) {
    if (vl.id != id) continue;
    viewport_->SetImageVisible(id, on);
    if (on) selectedVolumeId_ = id;
    else if (selectedVolumeId_ == id) selectedVolumeId_ = -1;
    UpdateInfo();
    UpdateHistogram();
    return;
  }
  // A tractogram layer: bundles blend, so toggling one only changes its own
  // visibility. Checking one also makes it the active (editable) bundle; the
  // others stay on as read-only overlays. Unchecking the active hands editing to
  // another visible bundle (or, if none, just hides it).
  for (int i = 0; i < static_cast<int>(tracts_.size()); ++i) {
    if (tracts_[i].id != id) continue;
    tracts_[i].visible = on;
    bool activated = false;
    if (on) {
      if (i != activeTracts_) { ActivateTracts(i); activated = true; }  // rebuilds display + overlays
    } else if (i == activeTracts_) {
      int next = -1;
      for (int j = 0; j < static_cast<int>(tracts_.size()); ++j)
        if (j != i && tracts_[j].visible) { next = j; break; }
      if (next >= 0) { ActivateTracts(next); activated = true; }
    }
    // Gate the active editable lines on the active bundle's own checkbox.
    const bool activeVisible = activeTracts_ >= 0 &&
                               activeTracts_ < static_cast<int>(tracts_.size()) &&
                               tracts_[activeTracts_].visible;
    viewport_->SetTractsVisible(activeVisible);
    viewport_->SetImageVisible(kDensityLayerId, activeVisible);  // density follows the active bundle
    if (!activated) RebuildTractOverlays();  // ActivateTracts already rebuilds them
    return;
  }
}

void MainWindow::OnLayerOpacity(int id, double opacity) {
  for (VolumeLayer& vl : volumes_) {
    if (vl.id != id) continue;
    vl.opacity = static_cast<float>(opacity);
    if (viewport_) viewport_->SetImageParams(id, vl.winLo, vl.winHi, vl.opacity);
    return;
  }
}

bool MainWindow::SaveScreenshot(const QString& path) {
  // Viewport (RHI) contents only (lines, cage, FA slices, box). The HUD/panel are
  // separate QWidgets and are NOT in this image — use window()->grab() for a full
  // capture. QRhiWidget::grabFramebuffer() renders a frame offscreen and returns it.
  const QImage image = viewport_->grabFramebuffer();
  return image.save(path);
}

void MainWindow::DebugScrubToZ(float z) {
  if (!viewport_) return;
  viewport_->SetSliceFocusZ(z);  // jump the focus (also emits, but we rebuild now)
  RebuildSliceGlyphs();          // synchronous rebuild, bypassing the debounce timer
}

bool MainWindow::SaveWindowShot(const QString& path) {
  // Whole-window grab (toolbar + docks + panels). The RHI viewport area may come out
  // blank here (its content is composited separately), but this captures the Qt
  // widget chrome that grabFramebuffer() omits.
  return grab().save(path);
}

void MainWindow::SaveAs() {
  if (store_.Empty()) {
    QMessageBox::information(this, "Nothing to save", "Load a tractogram first.");
    return;
  }
  const QString path =
      QFileDialog::getSaveFileName(this, "Save surviving streamlines", StartDir(), "TrackVis (*.trk)");
  if (path.isEmpty()) {
    return;
  }
  RememberDir(path);
  // Save is exact: it writes the full-set survivors, not the sampled display.
  if (!WriteTrkSubset(path.toStdString(), store_.header, store_.streamlines, aliveFull_)) {
    QMessageBox::critical(this, "Save failed", "Could not write the output .trk.");
    return;
  }
  const std::size_t kept =
      static_cast<std::size_t>(std::count(aliveFull_.begin(), aliveFull_.end(), uint8_t{1}));
  tractsDirty_ = false;  // edits are now persisted
  statusBar()->showMessage(
      QString("Saved %1 streamlines → %2").arg(QString::fromStdString(FormatCount(kept)), path));
}

void MainWindow::closeEvent(QCloseEvent* event) {
  // Prompt before discarding unsaved edits. (Only the tractogram is editable for
  // now; MRI/label join this check once they become editable.)
  if (!AnyTractsDirty() || !args_.screenshotPath.empty()) {  // no modal in headless runs
    event->accept();
    return;
  }
  const auto choice = QMessageBox::warning(
      this, "Unsaved changes",
      "There are unsaved tractogram edits. Save before closing?",
      QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
  if (choice == QMessageBox::Discard) {
    event->accept();
  } else if (choice == QMessageBox::Save) {
    SaveAs();
    tractsDirty_ ? event->ignore() : event->accept();  // still dirty => save was cancelled
  } else {
    event->ignore();
  }
}

// ── Drag & drop: drop any supported file(s) onto the window to open them ─────
namespace {
bool IsSupportedDrop(const QString& path) {
  const QString p = path.toLower();
  return p.endsWith(".trk") || p.endsWith(".nii") || p.endsWith(".nii.gz");
}
}  // namespace

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
  if (!event->mimeData()->hasUrls()) return;
  for (const QUrl& u : event->mimeData()->urls())
    if (u.isLocalFile() && IsSupportedDrop(u.toLocalFile())) {
      event->acceptProposedAction();  // show the "drop OK" cursor
      return;
    }
}

void MainWindow::dropEvent(QDropEvent* event) {
  bool any = false;
  for (const QUrl& u : event->mimeData()->urls()) {
    if (!u.isLocalFile() || !IsSupportedDrop(u.toLocalFile())) continue;
    const QString path = u.toLocalFile();
    RememberDir(path);
    DetectAndLoad(path);  // same auto-detect path as File ▸ Open
    any = true;
  }
  if (any) event->acceptProposedAction();
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event) {
  if (obj == viewport_ && event->type() == QEvent::Resize) UpdateEmptyHint();  // keep it centred
  return QMainWindow::eventFilter(obj, event);
}

// ── Onboarding: empty-state invite + one-click sample + controls cheatsheet ──
bool MainWindow::SceneHasData() const {
  return hasTracts_ || !volumes_.empty() ||
         (viewport_ && (viewport_->HasGlyphs() || viewport_->HasPeaks()));
}

void MainWindow::UpdateEmptyHint() {
  if (!emptyHint_ || !viewport_) return;
  const bool empty = !SceneHasData();
  if (empty) {
    emptyHint_->adjustSize();
    emptyHint_->move((viewport_->width() - emptyHint_->width()) / 2,
                     (viewport_->height() - emptyHint_->height()) / 2);
    emptyHint_->raise();
  }
  emptyHint_->setVisible(empty);
}

QString MainWindow::SamplePath() const {
  // Bundled demo fODF, resolved against likely roots (run-from-repo, or next to the
  // binary). Empty if absent -> the "Load sample" affordance simply hides.
  const QString rel = "dmri-explorer/data/odf.nii.gz";
  const QString appDir = QCoreApplication::applicationDirPath();
  for (const QString& root : {QDir::currentPath(), appDir, appDir + "/..", appDir + "/../.."}) {
    const QString p = QDir(root).filePath(rel);
    if (QFileInfo::exists(p)) return p;
  }
  return {};
}

void MainWindow::LoadSample() {
  const QString p = SamplePath();
  if (p.isEmpty())
    ReportError("Sample not found", "Couldn't find the bundled demo dmri-explorer/data/odf.nii.gz.");
  else
    DetectAndLoad(p);
}

void MainWindow::ShowControlsHelp() {
  // Compact, non-modal cheatsheet — the app has more controls than the toolbar shows
  // (slice scrub, per-pane reset, box editing, …).
  static const QString html = QStringLiteral(
      "<table cellpadding='4'>"
      "<tr><td colspan=2><b>Files</b></td></tr>"
      "<tr><td><b>⌘/Ctrl&nbsp;O</b></td><td>Open &mdash; auto-detects the type</td></tr>"
      "<tr><td><b>Drag &amp; drop</b></td><td>drop .trk / .nii(.gz) onto the window</td></tr>"
      "<tr><td><b>⌘/Ctrl&nbsp;S</b></td><td>save surviving streamlines</td></tr>"
      "<tr><td colspan=2>&nbsp;</td></tr><tr><td colspan=2><b>Navigate</b></td></tr>"
      "<tr><td><b>Left-drag</b></td><td>orbit (3-D pane) / pan (slice panes)</td></tr>"
      "<tr><td><b>Right-drag&nbsp;/&nbsp;Wheel</b></td><td>pan / zoom</td></tr>"
      "<tr><td><b>↑ ↓</b></td><td>scrub slice in the hovered pane (PgUp/PgDn = ×10)</td></tr>"
      "<tr><td><b>Double-click</b></td><td>maximize / restore a pane</td></tr>"
      "<tr><td><b>R&nbsp;/&nbsp;⇧R</b></td><td>reset the hovered view / all views</td></tr>"
      "<tr><td><b>H</b></td><td>toggle volume slices</td></tr>"
      "<tr><td colspan=2>&nbsp;</td></tr>"
      "<tr><td colspan=2><b>Edit</b> &nbsp;<span style='color:#888'>(press E for Edit mode)</span></td></tr>"
      "<tr><td><b>Drag handles</b></td><td>move / resize the 3-D selection box</td></tr>"
      "<tr><td><b>D&nbsp;/&nbsp;K</b></td><td>delete / keep streamlines in the box</td></tr>"
      "<tr><td><b>U&nbsp;/&nbsp;B</b></td><td>undo / re-place the box</td></tr></table>");
  auto* box = new QMessageBox(this);
  box->setAttribute(Qt::WA_DeleteOnClose);
  box->setWindowTitle("Controls");
  box->setTextFormat(Qt::RichText);
  box->setText(html);
  box->setStandardButtons(QMessageBox::Close);
  box->setModal(false);
  box->show();
}

void MainWindow::RebuildDisplay() {
  LineGeometry geo = BuildDisplayLineGeometry(
      store_, aliveFull_, args_.displayN, args_.dispStep, args_.seed);
  const Bounds bounds = geo.bounds;
  viewport_->SetLineGeometry(std::move(geo.vertices), std::move(geo.spans), bounds);
  UpdateStatus();
}

void MainWindow::RebuildTractOverlays() {
  // Collect the cached display geometry of every visible non-active bundle. The
  // geometry was built once (in ActivateTracts, while the bundle still had its
  // SoA), so this just gathers buffers — no re-decimation, and the slimmed
  // bundles need no point cloud. Overlays therefore stay at their load-time
  // density; only the active bundle responds to the density slider.
  if (!viewport_) return;
  std::vector<std::vector<float>> overlays;
  for (int i = 0; i < static_cast<int>(tracts_.size()); ++i) {
    if (i == activeTracts_ || !tracts_[i].visible || tracts_[i].overlayVerts.empty()) continue;
    overlays.push_back(tracts_[i].overlayVerts);  // copy: the bundle keeps its cache
  }
  viewport_->SetTractOverlays(std::move(overlays));
}

void MainWindow::RebuildDensityMap() {
  // Show the ACTIVE bundle's cached density map in the 2-D slice panes (computed
  // once at load; this only pushes the cached volume, it does NOT recompute). It
  // is an ortho-only heatmap layer blended over whatever volume(s) are loaded.
  if (!viewport_) return;
  if (activeTracts_ < 0 || activeTracts_ >= static_cast<int>(tracts_.size())) {
    viewport_->RemoveImage(kDensityLayerId);
    return;
  }
  const Volume& tdi = tracts_[activeTracts_].densityMap;
  if (tdi.Empty()) { viewport_->RemoveImage(kDensityLayerId); return; }
  viewport_->SetImage(kDensityLayerId, tdi, /*isLabel=*/false, {}, 0);  // copies the cached map
  viewport_->SetImageHeatmap(kDensityLayerId, true);    // "hot" density ramp
  viewport_->SetImageOrthoOnly(kDensityLayerId, true);  // 2-D slice panes only
  viewport_->SetImageVisible(kDensityLayerId, true);
}

void MainWindow::ReportError(const QString& title, const QString& message) {
  if (args_.screenshotPath.empty()) {
    QMessageBox::critical(this, title, message);
  } else {
    std::fprintf(stderr, "%s: %s\n", title.toStdString().c_str(), message.toStdString().c_str());
  }
}

void MainWindow::UpdateStatus() {
  const std::size_t kept =
      static_cast<std::size_t>(std::count(aliveFull_.begin(), aliveFull_.end(), uint8_t{1}));
  // The kept/total count is an editing concern: show it only in Edit mode. View
  // mode keeps a clean navigation hint with no running count.
  if (editMode_) {
    statusBar()->showMessage(
        QString("kept %1 / %2   ·   cap %3 step %4   ·   drag handles · d delete · k keep · u undo · b reset-box")
            .arg(QString::fromStdString(FormatCount(kept)),
                 QString::fromStdString(FormatCount(store_.StreamlineCount())),
                 QString::number(args_.displayN),
                 QString::number(args_.dispStep)));
  } else {
    statusBar()->showMessage(
        "drag rotate · right-drag pan · wheel zoom · double-click pane to maximize · R recenter · E edit");
  }

  const QString name = args_.trkPath.empty()
                           ? QStringLiteral("untitled")
                           : QFileInfo(QString::fromStdString(args_.trkPath)).fileName();
  if (hud_) hud_->SetInfo(name, kept, store_.StreamlineCount());  // HUD is shown only in Edit

  if (properties_) {
    const int total = static_cast<int>(std::max<std::size_t>(1, store_.StreamlineCount()));
    properties_->SetDensity(args_.displayN, total);
    properties_->SetStep(args_.dispStep);
    // Force the next poll to recompute the in-box count against the new alive set
    // (the box may not have moved, but the edit changed what it holds).
    lastBox_.v[0] = std::nan("");
  }
  UpdateInfo();
}

void MainWindow::UpdateInfo() {
  if (!properties_) return;
  QString s;
  if (activeTracts_ >= 0 && activeTracts_ < static_cast<int>(tracts_.size())) {
    s += QStringLiteral("Tracts:  %1\n         %2 streamlines\n\n")
             .arg(tracts_[activeTracts_].name,
                  QString::fromStdString(FormatCount(store_.StreamlineCount())));
  }
  for (const VolumeLayer& vl : volumes_) {
    if (vl.id != selectedVolumeId_) continue;
    s += QStringLiteral("%1:  %2\n         %3×%4×%5   [%6, %7]\n\n")
             .arg(vl.isLabel ? "Label" : "Volume", vl.name)
             .arg(vl.vol.dims[0]).arg(vl.vol.dims[1]).arg(vl.vol.dims[2])
             .arg(vl.vol.valueMin, 0, 'g', 3).arg(vl.vol.valueMax, 0, 'g', 3);
    break;
  }
  if (!odfInfo_.isEmpty()) s += QStringLiteral("ODF:  %1\n\n").arg(odfInfo_);
  if (!peaksInfo_.isEmpty()) s += QStringLiteral("Peaks:  %1\n\n").arg(peaksInfo_);
  properties_->SetInfo(s.isEmpty() ? QStringLiteral("No data loaded.") : s.trimmed());
}

void MainWindow::ComputeHistogram(VolumeLayer& vl) {
  // Adaptive intensity axis: a few bright outliers shouldn't squash the bulk of
  // the data to the left, so cap the displayed max at the 99.5th percentile. The
  // default window uses that same robust range (FSLeyes-style auto contrast).
  const Volume& v = vl.vol;
  const float dmin = v.valueMin;
  const float dmax = (v.valueMax > v.valueMin) ? v.valueMax : v.valueMin + 1.0f;
  constexpr int kFine = 1024;
  std::vector<double> fine(kFine, 0.0);
  const double finv = static_cast<double>(kFine - 1) / (static_cast<double>(dmax) - dmin);
  for (float s : v.data) fine[std::clamp(static_cast<int>((static_cast<double>(s) - dmin) * finv),
                                         0, kFine - 1)] += 1.0;
  const double total = std::max(1.0, static_cast<double>(v.data.size()));
  double cum = 0.0;
  int robBin = kFine - 1;
  for (int i = 0; i < kFine; ++i) { cum += fine[i]; if (cum >= 0.995 * total) { robBin = i; break; } }

  vl.dispMin = dmin;
  vl.dispMax = dmin + static_cast<float>(robBin + 1) / kFine * (dmax - dmin);
  if (vl.dispMax <= vl.dispMin) vl.dispMax = dmax;
  vl.winLo = vl.dispMin;
  vl.winHi = vl.dispMax;

  // 160 display bins over [dispMin, dispMax], aggregated from the fine bins.
  constexpr int kDisp = 160;
  vl.histBins.assign(kDisp, 0.0f);
  const float span = std::max(1e-9f, vl.dispMax - vl.dispMin);
  for (int j = 0; j < kFine; ++j) {
    const double cj = dmin + (j + 0.5) / kFine * (static_cast<double>(dmax) - dmin);
    const int db = std::clamp(static_cast<int>((cj - vl.dispMin) / span * kDisp), 0, kDisp - 1);
    vl.histBins[db] += static_cast<float>(fine[j]);
  }
}

void MainWindow::ComputeStatHistogram(VolumeLayer& vl, int intentCode) {
  // An fMRI stat map (z/t/r/…) is SIGNED and symmetric about 0, so the meaningful
  // axis is |stat|, not the raw min/max grayscale window (which a single outlier
  // would wreck). Build a |stat| histogram over the non-zero voxels (the map is
  // mostly exact-0 background) and seed a [threshold, cap] window that is data-aware
  // and editable: cap from a robust high percentile so a few hot peaks don't squash
  // the scale; threshold by statistic kind (z/t≈2.3, r≈0.3 — FSL/SPM conventions),
  // else a robust percentile so low-magnitude noise starts hidden.
  const Volume& v = vl.vol;  // data is finite — LoadNifti sanitizes NaN/Inf to 0 at load
  float absMax = 0.0f;
  for (float s : v.data) absMax = std::max(absMax, std::abs(s));
  if (absMax <= 0.0f) absMax = 1.0f;

  constexpr int kFine = 1024;
  std::vector<double> fine(kFine, 0.0);
  const double finv = static_cast<double>(kFine - 1) / absMax;
  double nz = 0.0;  // count of non-zero voxels (background excluded from percentiles)
  for (float s : v.data) {
    const float a = std::abs(s);
    if (a <= 0.0f) continue;
    fine[std::clamp(static_cast<int>(a * finv), 0, kFine - 1)] += 1.0;
    nz += 1.0;
  }
  if (nz <= 0.0) nz = 1.0;
  auto percentile = [&](double frac) {
    double cum = 0.0;
    for (int i = 0; i < kFine; ++i) {
      cum += fine[i];
      if (cum >= frac * nz) return static_cast<float>((i + 1) / static_cast<double>(kFine) * absMax);
    }
    return absMax;
  };

  const float cap = percentile(0.98);                          // default high handle
  const float axisMax = std::max(cap, percentile(0.999));      // histogram axis (headroom over cap)
  float thr;
  switch (intentCode) {
    case 5: case 3: thr = 2.3f; break;            // ZSCORE / TTEST (FSL cluster-forming default)
    case 2:         thr = 0.3f; break;            // CORREL (Pearson r)
    default:        thr = percentile(0.80); break;  // unknown signed map: hide the low-magnitude bulk
  }
  thr = std::clamp(thr, 0.0f, 0.9f * cap);        // keep threshold below the cap (winLo < winHi)

  vl.dispMin = 0.0f;
  vl.dispMax = axisMax;
  vl.winLo = thr;   // |stat| threshold (low handle)
  vl.winHi = cap;   // |stat| cap       (high handle)

  // 160 display bins over [0, axisMax], aggregated from the fine |stat| bins.
  constexpr int kDisp = 160;
  vl.histBins.assign(kDisp, 0.0f);
  for (int j = 0; j < kFine; ++j) {
    const double cj = (j + 0.5) / kFine * absMax;
    const int db = std::clamp(static_cast<int>(cj / axisMax * kDisp), 0, kDisp - 1);
    vl.histBins[db] += static_cast<float>(fine[j]);
  }
}

void MainWindow::ComputeLabelLut(VolumeLayer& vl) {
  // Build a dense RGBA table indexed by integer label value (the shader does
  // texelFetch(uLut, idx)). Index 0 is background → transparent; each non-zero
  // label gets a distinct hue from the golden-ratio walk (maximally spread,
  // stable per index). Width = maxLabel+1, capped so a stray huge value can't
  // allocate an absurd texture (the shader clamps out-of-range indices).
  constexpr int kMaxLut = 4096;  // covers FreeSurfer aseg (~2035) and atlases
  int maxLabel = 0;
  for (float s : vl.vol.data) {
    const int idx = static_cast<int>(s + 0.5f);
    if (idx > maxLabel) maxLabel = idx;
  }
  const int width = std::clamp(maxLabel + 1, 2, kMaxLut);
  vl.lut.assign(static_cast<std::size_t>(width) * 4, 0.0f);  // index 0 stays (0,0,0,0)

  auto hsvToRgb = [](float h, float s, float v, float& r, float& g, float& b) {
    const float i = std::floor(h * 6.0f);
    const float f = h * 6.0f - i;
    const float p = v * (1.0f - s), q = v * (1.0f - f * s), t = v * (1.0f - (1.0f - f) * s);
    switch (static_cast<int>(i) % 6) {
      case 0: r = v; g = t; b = p; break;
      case 1: r = q; g = v; b = p; break;
      case 2: r = p; g = v; b = t; break;
      case 3: r = p; g = q; b = v; break;
      case 4: r = t; g = p; b = v; break;
      default: r = v; g = p; b = q; break;
    }
  };
  constexpr float kGolden = 0.61803398875f;
  for (int idx = 1; idx < width; ++idx) {
    const float hue = std::fmod(static_cast<float>(idx) * kGolden, 1.0f);
    float r, g, b;
    hsvToRgb(hue, 0.65f, 0.95f, r, g, b);
    float* px = &vl.lut[static_cast<std::size_t>(idx) * 4];
    px[0] = r; px[1] = g; px[2] = b; px[3] = 1.0f;
  }
  vl.lutWidth = width;
}

void MainWindow::UpdateHistogram() {
  if (!properties_) return;
  // The Contrast panel edits the selected layer's window. The image's grayscale-
  // vs-label mode + LUT were set once at load (SetImage), so only the window /
  // opacity need re-pushing here.
  for (const VolumeLayer& vl : volumes_) {
    if (vl.id != selectedVolumeId_) continue;
    properties_->SetHistogram(true, vl.histBins, vl.dispMin, vl.dispMax, vl.winLo, vl.winHi);
    // For a stat layer the window handles are |stat| threshold/cap; show the diverging
    // legend (with units) beneath them. Hidden for grayscale volumes/labels.
    properties_->SetStatColorbar(vl.isStat, vl.winLo, vl.winHi, StatUnits(vl.statIntent));
    // Colormap choice (grayscale/viridis) only for a plain scalar volume.
    properties_->SetColormap(!vl.isLabel && !vl.isStat, vl.viridis);
    if (viewport_) viewport_->SetImageParams(vl.id, vl.winLo, vl.winHi, vl.opacity);
    return;
  }
  properties_->SetHistogram(false, {}, 0.0, 0.0, 0.0, 0.0);  // no selected layer
  properties_->SetStatColorbar(false, 0.0, 0.0, {});
  properties_->SetColormap(false, false);
}

void MainWindow::RefreshStats() {
  if (!properties_) return;
  if (!hasTracts_) {
    properties_->SetStats("Load a tractogram first.");
    return;
  }
  const BasicStats s = ComputeBasicStats(store_, aliveFull_);
  auto summary = [](const NumericSummary& n) {
    return n.valid ? QStringLiteral("%1 ± %2  [%3, %4]")
                         .arg(n.mean, 0, 'f', 1)
                         .arg(n.stddev, 0, 'f', 1)
                         .arg(n.min, 0, 'f', 1)
                         .arg(n.max, 0, 'f', 1)
                   : QStringLiteral("—");
  };
  properties_->SetStats(
      QStringLiteral("kept    %1 / %2  (%3% deleted)\nlength  %4 mm\npts/ln  %5")
          .arg(QString::fromStdString(FormatCount(s.aliveCount)),
               QString::fromStdString(FormatCount(s.fullCount)))
          .arg(s.deletedPercent, 0, 'f', 1)
          .arg(summary(s.lengthMm))
          .arg(summary(s.pointsPerLine)));
}

void MainWindow::PollSelectionReadout() {
  UpdateEmptyHint();  // cheap: reflect load/clear in the centred invite (no per-load wiring)
  if (!properties_) return;
  if (!hasTracts_ || viewport_ == nullptr || !viewport_->HasSelectionBox()) {
    properties_->SetBox(false, Bounds{}, 0, 0.0);
    return;
  }
  const Bounds box = viewport_->SelectionBox();
  bool changed = false;
  for (int i = 0; i < 6; ++i)
    if (box.v[i] != lastBox_.v[i]) { changed = true; break; }
  if (changed) {
    // Still moving (a drag mutates the box every tick) — record it but defer the
    // O(all-points) selection scan until the box settles, so dragging doesn't run
    // a full-set scan 10x/second on the GUI thread.
    lastBox_ = box;
    boxStableTicks_ = 0;
    return;
  }
  if (boxStableTicks_ >= 1) return;  // already mirrored this resting box
  boxStableTicks_ = 1;

  const std::size_t inBox = CountInBox(selection_->SelectInBox(store_, box));
  const std::size_t alive = std::max<std::size_t>(
      1, static_cast<std::size_t>(std::count(aliveFull_.begin(), aliveFull_.end(), uint8_t{1})));
  const double pct = 100.0 * static_cast<double>(inBox) / static_cast<double>(alive);
  properties_->SetBox(true, box, inBox, pct);
}

std::size_t MainWindow::CountInBox(const std::vector<uint8_t>& inBox) const {
  std::size_t n = 0;
  for (std::size_t i = 0; i < inBox.size(); ++i)
    if (inBox[i] && aliveFull_[i]) ++n;
  return n;
}

void MainWindow::PushHistory() {
  constexpr std::size_t kMaxUndo = 50;
  history_.push_back(aliveFull_);
  if (history_.size() > kMaxUndo) history_.erase(history_.begin());
  tractsDirty_ = true;  // an edit is about to happen -> unsaved changes
}

void MainWindow::DeleteInBox() {
  if (!hasTracts_ || !viewport_->HasSelectionBox()) return;
  const std::vector<uint8_t> inBox = selection_->SelectInBox(store_, viewport_->SelectionBox());
  const std::size_t nKill = CountInBox(inBox);
  if (nKill == 0) {
    statusBar()->showMessage("Box holds no kept streamlines.", 4000);
    return;
  }
  PushHistory();
  for (std::size_t i = 0; i < inBox.size(); ++i)
    if (inBox[i]) aliveFull_[i] = 0;
  RebuildDisplay();
  statusBar()->showMessage(
      QString("Deleted %1 streamlines.").arg(QString::fromStdString(FormatCount(nKill))), 4000);
}

void MainWindow::KeepInBox() {
  if (!hasTracts_ || !viewport_->HasSelectionBox()) return;
  const std::vector<uint8_t> inBox = selection_->SelectInBox(store_, viewport_->SelectionBox());
  const std::size_t nKeep = CountInBox(inBox);
  if (nKeep == 0) {
    statusBar()->showMessage("Box holds no kept streamlines.", 4000);
    return;
  }
  PushHistory();
  for (std::size_t i = 0; i < inBox.size(); ++i)
    aliveFull_[i] = static_cast<uint8_t>(aliveFull_[i] && inBox[i]);
  RebuildDisplay();
  statusBar()->showMessage(
      QString("Kept %1 streamlines.").arg(QString::fromStdString(FormatCount(nKeep))), 4000);
}

void MainWindow::Undo() {
  if (history_.empty()) {
    statusBar()->showMessage("Nothing to undo.", 3000);
    return;
  }
  aliveFull_ = std::move(history_.back());
  history_.pop_back();
  tractsDirty_ = true;  // state changed from the last save
  RebuildDisplay();
  statusBar()->showMessage("Undo.", 3000);
}

void MainWindow::ResetBox() {
  viewport_->ResetSelectionBox();
  statusBar()->showMessage("Selection box reset.", 3000);
}

bool MainWindow::RunEditSelfTest() {
  if (!hasTracts_) {
    std::printf("selftest-edit: no tractogram\n");
    return false;
  }
  const std::size_t total = store_.StreamlineCount();
  const Bounds& b = tractRasBounds_;
  auto countSel = [&](const Bounds& box) {
    const auto f = selection_->SelectInBox(store_, box);
    return static_cast<std::size_t>(std::count(f.begin(), f.end(), uint8_t{1}));
  };

  // Geometry-agnostic checks (no assumption about where streamlines sit):
  //   inclusion — the exact data AABB selects every streamline;
  //   exclusion — a box translated far outside the data selects none.
  const std::size_t nFull = countSel(b);
  Bounds outside = b;
  const double shift = (b.v[1] - b.v[0]) + 1000.0;
  outside.v[0] += shift;
  outside.v[1] += shift;
  const std::size_t nOut = countSel(outside);

  // Strict subset + count equivalence: delete the left half (x <= centre);
  // alive must drop by exactly the alive-in-box count.
  Bounds half = b;
  half.v[1] = 0.5 * (b.v[0] + b.v[1]);
  const std::vector<uint8_t> inHalf = selection_->SelectInBox(store_, half);
  const std::size_t nHalf = CountInBox(inHalf);
  const std::vector<uint8_t> saved = aliveFull_;
  for (std::size_t i = 0; i < inHalf.size(); ++i)
    if (inHalf[i]) aliveFull_[i] = 0;
  const std::size_t after =
      static_cast<std::size_t>(std::count(aliveFull_.begin(), aliveFull_.end(), uint8_t{1}));
  aliveFull_ = saved;  // restore (no display rebuild needed)

  const bool ok = (nFull == total) && (nOut == 0) && (after == total - nHalf);
  std::printf(
      "selftest-edit: total=%zu · full-box=%zu(==total? %s) · outside-box=%zu(==0? %s) · "
      "left-half in=%zu, delete->%zu (countOk %s) — %s\n",
      total, nFull, nFull == total ? "y" : "N", nOut, nOut == 0 ? "y" : "N", nHalf, after,
      (after == total - nHalf) ? "y" : "N", ok ? "OK" : "FAIL");
  std::fflush(stdout);
  return ok;
}

}  // namespace tracto
