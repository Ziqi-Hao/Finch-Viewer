// Standalone self-test for the pure-CPU ODF module (cpp/odf/).
//
// WHY a hand-rolled main() instead of GTest: this module must build with nothing
// beyond the C++17 stdlib + zlib (see CLAUDE.md / the module headers). Pulling in
// a test framework would contradict that. So this is one `int main()` that
// exercises every public signature, asserts the load-bearing math, prints a clear
// PASS/FAIL per block, and returns non-zero on the first failure.
//
// Coverage:
//   (a) sh_basis    — order inference for known coeff counts, the l=0 constant,
//                     basis-matrix dimensions.
//   (b) icosphere   — V/F counts per subdivision, unit-length vertices, Euler
//                     characteristic V - E + F == 2.
//   (c) glyph_builder — a synthetic isotropic fODF (only the l=0 coeff set) must
//                     reconstruct an (almost) perfect sphere: all radii equal and
//                     positive.
//   (d) odf_volume  — if the demo fODF .nii.gz exists, load it and sanity-check
//                     dims / nCoeffs / finiteness; otherwise print a skip note.
//
// Build is via the module's own implementation TUs (sh_basis.cpp, icosphere.cpp,
// odf_volume.cpp, glyph_builder.cpp); this file adds no new dependency.

#include "glyph_builder.hpp"
#include "glyph_scene.hpp"
#include "icosphere.hpp"
#include "odf_types.hpp"
#include "odf_volume.hpp"
#include "sh_basis.hpp"

#include <algorithm>  // std::min, std::max
#include <cmath>
#include <cstddef>
#include <cstdio>     // std::FILE, std::fopen, std::fclose (demo-file probe)
#include <iostream>
#include <set>
#include <stdexcept>  // std::exception (catch from the throwing loaders)
#include <string>
#include <utility>    // std::make_pair, std::pair
#include <vector>

namespace {

using namespace tracto::odf;

// --- Tiny assertion harness -------------------------------------------------
// We do NOT use <cassert>: NDEBUG would silently disable it, and we want the
// test to fail loudly in any build. Each check records a failure and keeps going
// within a block so one run reports as much as possible; main() returns non-zero
// if g_failures != 0.
int g_failures = 0;

void Check(bool ok, const std::string& what) {
  if (ok) {
    std::cout << "  [ ok ] " << what << "\n";
  } else {
    std::cout << "  [FAIL] " << what << "\n";
    ++g_failures;
  }
}

// Approximate equality with an absolute tolerance. Used for floating-point
// comparisons throughout (basis constant, radii, unit length).
bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

constexpr double kPi = 3.14159265358979323846;

// --- (a) SH basis -----------------------------------------------------------
void TestShBasis() {
  std::cout << "[a] sh_basis\n";

  // Order-from-count for the symmetric (even-l only) coefficient counts seen in
  // real fODF data. nCoeffs = (L+1)(L+2)/2, so 6->L2, 15->L4, 28->L6, 45->L8.
  struct Case { std::size_t n; int lMax; };
  const Case symCases[] = {{6, 2}, {15, 4}, {28, 6}, {45, 8}};
  for (const Case& c : symCases) {
    const ShOrder ord = InferShOrder(c.n);
    Check(ord.kind == ShBasisKind::Symmetric && ord.lMax == c.lMax &&
              ord.nCoeffs == c.n,
          "InferShOrder(" + std::to_string(c.n) + ") -> symmetric lMax=" +
              std::to_string(c.lMax));
    // Round-trip: ShNumCoeffs must invert the inference.
    Check(ShNumCoeffs(c.lMax, ShBasisKind::Symmetric) == c.n,
          "ShNumCoeffs(" + std::to_string(c.lMax) + ", Symmetric) == " +
              std::to_string(c.n));
  }

  // Full basis counts are perfect squares: (L+1)^2. 1->L0, 9->L2, 16->L3.
  // 1 is ambiguous (also symmetric L0); the inference is documented to try
  // symmetric first, so we only assert the unambiguous full counts here.
  Check(InferShOrder(9).kind == ShBasisKind::Full && InferShOrder(9).lMax == 2,
        "InferShOrder(9) -> full lMax=2");
  Check(ShNumCoeffs(3, ShBasisKind::Full) == 16,
        "ShNumCoeffs(3, Full) == 16");

  // A count that is neither (L+1)(L+2)/2 nor a perfect square must throw.
  bool threw = false;
  try {
    InferShOrder(7);  // not symmetric (7 != 6,10,...), not a perfect square.
  } catch (const std::exception&) {
    threw = true;
  }
  Check(threw, "InferShOrder(7) throws on invalid count");

  // The l=0, m=0 SH is the constant 1/(2*sqrt(pi)) everywhere on the sphere.
  // WHY: Y_0^0 = sqrt(1/(4*pi)) = 1/(2*sqrt(pi)) ~= 0.2820947918. It must be
  // direction-independent, so we sample two unrelated directions.
  const double kY00 = 1.0 / (2.0 * std::sqrt(kPi));
  float t0, p0, t1, p1;
  DirectionToSpherical(Vec3{0.0f, 0.0f, 1.0f}, t0, p0);  // +Z pole: theta=0.
  DirectionToSpherical(Vec3{1.0f, 1.0f, 1.0f}, t1, p1);  // arbitrary octant.
  const double v0 = EvalRealSh(0, 0, t0, p0, ShBasisKind::Symmetric);
  const double v1 = EvalRealSh(0, 0, t1, p1, ShBasisKind::Symmetric);
  Check(Near(v0, kY00, 1e-5), "EvalRealSh(0,0) at +Z == 1/(2*sqrt(pi))");
  Check(Near(v1, kY00, 1e-5), "EvalRealSh(0,0) off-axis == 1/(2*sqrt(pi))");

  // DirectionToSpherical convention: theta=acos(z). +Z -> theta=0.
  Check(Near(t0, 0.0, 1e-5), "DirectionToSpherical(+Z) -> theta == 0");

  // Basis-matrix dimensions: nDir rows (one per direction), nCoeffs columns.
  const Icosphere ico = MakeIcosphere(2);  // 162 directions
  const ShBasisMatrix mat = BuildShBasisMatrix(ico.vertices, std::size_t{45});
  Check(mat.nDir == ico.vertexCount(), "BuildShBasisMatrix nDir == 162");
  Check(mat.nCoeffs == 45, "BuildShBasisMatrix nCoeffs == 45");
  Check(mat.order.lMax == 8 && mat.order.kind == ShBasisKind::Symmetric,
        "BuildShBasisMatrix inferred order L8 symmetric");
  Check(mat.values.size() == mat.nDir * mat.nCoeffs,
        "BuildShBasisMatrix values size == nDir*nCoeffs");
  // Column 0 is the l=0 basis: every row's first entry is the same constant.
  if (mat.nDir >= 2 && mat.nCoeffs >= 1) {
    Check(Near(mat.row(0)[0], kY00, 1e-5) && Near(mat.row(1)[0], kY00, 1e-5),
          "basis matrix column 0 == 1/(2*sqrt(pi)) for all directions");
  }
}

// --- (b) Icosphere ----------------------------------------------------------
// Count the unique undirected edges of a triangle mesh. Each triangle cont 3
// edges; shared edges appear twice, so we de-dup with a set keyed by the sorted
// vertex pair (the same (v0<v1) normalization the generator uses for midpoint
// de-dup). Needed to evaluate the Euler characteristic V - E + F.
std::size_t CountEdges(const std::vector<unsigned>& indices) {
  std::set<std::pair<unsigned, unsigned>> edges;
  for (std::size_t f = 0; f + 2 < indices.size(); f += 3) {
    const unsigned a = indices[f], b = indices[f + 1], c = indices[f + 2];
    auto addEdge = [&edges](unsigned u, unsigned v) {
      edges.insert(u < v ? std::make_pair(u, v) : std::make_pair(v, u));
    };
    addEdge(a, b);
    addEdge(b, c);
    addEdge(c, a);
  }
  return edges.size();
}

void TestIcosphere() {
  std::cout << "[b] icosphere\n";

  // Expected V/F/E per subdivision level (Euler must hold).
  // E is FORCED by F: a closed (watertight, genus-0) triangle mesh has every
  // edge shared by exactly two faces, so 3F = 2E, i.e. E = 3F/2. With F x4 per
  // level (each triangle -> 4 children), E also x4: 30 -> 120 -> 480 -> 1920.
  // (An earlier table here listed 90/360/1440 from a stale "E x3" note in
  // icosphere.hpp; that violates 3F=2E and made V-E+F != 2. The generator is
  // correct — the Euler check below passes with these counted edges.)
  struct Case { int subdiv; std::size_t v; std::size_t f; std::size_t e; };
  const Case cases[] = {
      {0, 12, 20, 30},
      {1, 42, 80, 120},
      {2, 162, 320, 480},
      {3, 642, 1280, 1920},
  };

  for (const Case& c : cases) {
    const Icosphere ico = MakeIcosphere(c.subdiv);
    const std::string tag = "subdiv " + std::to_string(c.subdiv);

    Check(ico.vertexCount() == c.v,
          tag + ": V == " + std::to_string(c.v));
    Check(ico.triangleCount() == c.f,
          tag + ": F == " + std::to_string(c.f));
    Check(ico.indices.size() == c.f * 3,
          tag + ": indices == 3*F");

    // Every vertex must lie on the unit sphere (radius 1) within 1e-5.
    bool allUnit = !ico.vertices.empty();
    double maxErr = 0.0;
    for (const Vec3& v : ico.vertices) {
      const double len = Length(v);
      maxErr = std::max(maxErr, std::fabs(len - 1.0));
      if (!Near(len, 1.0, 1e-5)) allUnit = false;
    }
    Check(allUnit, tag + ": all vertices unit-length (max err " +
                       std::to_string(maxErr) + ")");

    // Index bounds: every index must reference an existing vertex.
    bool inBounds = true;
    for (unsigned idx : ico.indices) {
      if (idx >= ico.vertexCount()) { inBounds = false; break; }
    }
    Check(inBounds, tag + ": all indices in [0, V)");

    // Euler characteristic of a closed genus-0 triangulation: V - E + F == 2.
    const std::size_t E = CountEdges(ico.indices);
    Check(E == c.e, tag + ": E == " + std::to_string(c.e) + " (got " +
                        std::to_string(E) + ")");
    const long euler = static_cast<long>(c.v) - static_cast<long>(E) +
                       static_cast<long>(c.f);
    Check(euler == 2, tag + ": V - E + F == 2 (got " +
                          std::to_string(euler) + ")");
  }
}

// --- (c) Glyph builder ------------------------------------------------------
void TestGlyphBuilder() {
  std::cout << "[c] glyph_builder\n";

  // Build a 1x1x1 volume carrying a single isotropic fODF: only the l=0
  // coefficient is non-zero. WHY isotropic-> sphere: with only c[0] set, the SH
  // reconstruction at every direction is radius = c[0] * Y_0^0 = constant, so the
  // deformed icosphere is a uniform-radius sphere — the cleanest invariant to
  // assert that the matvec + deformation is correct.
  const int nCoeffs = 45;  // L8 symmetric, like the demo data
  OdfVolume vol;
  vol.dims[0] = vol.dims[1] = vol.dims[2] = 1;
  vol.nCoeffs = nCoeffs;
  vol.coeffs.assign(static_cast<std::size_t>(nCoeffs), 0.0f);
  vol.coeffs[0] = 1.0f;          // isotropic amplitude on the l=0 band only
  vol.affine = Affine{};         // identity: voxel (0,0,0) -> world origin
  vol.valueMin = 0.0f;
  vol.valueMax = 1.0f;

  const Icosphere ico = MakeIcosphere(2);  // 162 verts -> smooth sphere
  const ShBasisMatrix mat =
      BuildShBasisMatrix(ico.vertices, static_cast<std::size_t>(nCoeffs));

  // Test BOTH normalization modes. With per-glyph normalization on, all radii
  // equal `scale` exactly; with it off, all radii equal c[0]*Y_0^0*scale.
  // Either way the key invariant is "all radii equal and positive".
  for (bool perGlyph : {true, false}) {
    GlyphParams params;
    params.scale = 1.0f;
    params.normalizePerGlyph = perGlyph;
    params.clampNegative = true;

    const std::vector<VoxelIndex> voxels = {VoxelIndex{0, 0, 0}};
    const GlyphMesh mesh = BuildGlyphs(vol, ico, mat, voxels, params);

    const std::string tag =
        std::string("isotropic glyph (perGlyph=") + (perGlyph ? "1" : "0") + ")";

    Check(mesh.vertexCount() == ico.vertexCount(),
          tag + ": one glyph -> V == icosphere V");
    Check(mesh.triangleCount() == ico.triangleCount(),
          tag + ": one glyph -> F == icosphere F");
    Check(mesh.positions.size() == mesh.normals.size() &&
              mesh.positions.size() == mesh.colors.size(),
          tag + ": positions/normals/colors parallel");

    // Radius of each vertex from the glyph center (the voxel's world position,
    // which is the origin under the identity affine). For an isotropic ODF every
    // radius must match the first, and all must be strictly positive.
    if (!mesh.positions.empty()) {
      const Vec3 center = VoxelToWorld(vol.affine, VoxelIndex{0, 0, 0});
      const double r0 = Length(mesh.positions[0] - center);
      double minR = r0, maxR = r0;
      for (const Vec3& p : mesh.positions) {
        const double r = Length(p - center);
        minR = std::min(minR, r);
        maxR = std::max(maxR, r);
      }
      Check(minR > 1e-6, tag + ": all radii positive (min " +
                             std::to_string(minR) + ")");
      // "~a sphere": spread between min and max radius is tiny relative to the
      // radius itself. 1e-4 absolute tolerance comfortably covers float matvec
      // round-off over 45 coeffs * 162 directions.
      Check(Near(maxR, minR, 1e-4),
            tag + ": all radii ~equal (min " + std::to_string(minR) +
                ", max " + std::to_string(maxR) + ")");
    }

    // Normals must be unit length (they are the deformed-surface normals; for a
    // sphere they point radially). Allow a slightly looser tol for re-normalize
    // round-off.
    bool normalsUnit = !mesh.normals.empty();
    for (const Vec3& n : mesh.normals) {
      if (!Near(Length(n), 1.0, 1e-3)) { normalsUnit = false; break; }
    }
    Check(normalsUnit, tag + ": normals unit-length");

    // Colors are direction-encoded RGB in [0,1].
    bool colorsValid = !mesh.colors.empty();
    for (const Vec3& col : mesh.colors) {
      if (col.x < -1e-5 || col.x > 1.0f + 1e-5 || col.y < -1e-5 ||
          col.y > 1.0f + 1e-5 || col.z < -1e-5 || col.z > 1.0f + 1e-5) {
        colorsValid = false;
        break;
      }
    }
    Check(colorsValid, tag + ": colors in [0,1]");

    // Index buffer references only existing vertices.
    bool idxOk = true;
    for (unsigned idx : mesh.indices) {
      if (idx >= mesh.vertexCount()) { idxOk = false; break; }
    }
    Check(idxOk, tag + ": indices in [0, V)");
  }

  // DirectionColor contract: R=|x|, G=|y|, B=|z| of the NORMALIZED direction.
  const Vec3 cx = DirectionColor(Vec3{3.0f, 0.0f, 0.0f});  // normalizes to +X
  Check(Near(cx.x, 1.0, 1e-5) && Near(cx.y, 0.0, 1e-5) && Near(cx.z, 0.0, 1e-5),
        "DirectionColor(+X) == (1,0,0)");
  const Vec3 cn = DirectionColor(Vec3{-1.0f, 0.0f, 0.0f});  // |x| -> 1
  Check(Near(cn.x, 1.0, 1e-5), "DirectionColor(-X) maps to |x|=1");
}

// --- (d) ODF volume loader (conditional) ------------------------------------
void TestOdfVolume() {
  std::cout << "[d] odf_volume\n";

  // Optional fixture: drop an fODF at data/odf.nii.gz (gitignored) to exercise the
  // loader. Absent -> this case skips (run from the repo root for the relative path).
  const std::string demoPath = "data/odf.nii.gz";

  // Probe for the demo file without throwing: try to load, treat "file not
  // found" style failures as a skip rather than a test failure. We can't stat
  // without extra deps, so we rely on the loader's exception and inspect via a
  // cheap fopen first.
  if (std::FILE* f = std::fopen(demoPath.c_str(), "rb")) {
    std::fclose(f);
  } else {
    std::cout << "  [skip] demo fODF not present at " << demoPath << "\n";
    return;
  }

  OdfVolume vol;
  try {
    vol = LoadOdfNifti(demoPath);
  } catch (const std::exception& e) {
    Check(false, std::string("LoadOdfNifti threw: ") + e.what());
    return;
  }

  Check(!vol.empty(), "loaded volume is non-empty");
  Check(vol.dims[0] > 0 && vol.dims[1] > 0 && vol.dims[2] > 0,
        "spatial dims all > 0 (" + std::to_string(vol.dims[0]) + "x" +
            std::to_string(vol.dims[1]) + "x" + std::to_string(vol.dims[2]) +
            ")");
  Check(vol.nCoeffs > 0,
        "nCoeffs > 0 (" + std::to_string(vol.nCoeffs) + ")");

  // The buffer size must equal nx*ny*nz*nCoeffs (coeff-outermost layout).
  const std::size_t expect = vol.voxelCount() *
                             static_cast<std::size_t>(vol.nCoeffs);
  Check(vol.coeffs.size() == expect,
        "coeffs.size() == voxels*nCoeffs (" + std::to_string(vol.coeffs.size()) +
            " == " + std::to_string(expect) + ")");

  // nCoeffs must be a valid SH count, and for the demo it is 45 (L8 symmetric).
  bool inferOk = false;
  try {
    const ShOrder ord = InferShOrder(static_cast<std::size_t>(vol.nCoeffs));
    inferOk = true;
    std::cout << "  [info] inferred SH order lMax=" << ord.lMax << " kind="
              << (ord.kind == ShBasisKind::Symmetric ? "symmetric" : "full")
              << "\n";
  } catch (const std::exception&) {
    inferOk = false;
  }
  Check(inferOk, "nCoeffs is a valid SH coefficient count");

  // Every coefficient must be finite (no NaN/Inf leaked from the gzip/datatype
  // conversion). NaN poisons the SH matvec silently, so catch it here.
  bool allFinite = true;
  for (float c : vol.coeffs) {
    if (!std::isfinite(c)) { allFinite = false; break; }
  }
  Check(allFinite, "all coefficients finite (no NaN/Inf)");

  // valueMin/valueMax must bracket the data and be finite/ordered.
  Check(std::isfinite(vol.valueMin) && std::isfinite(vol.valueMax) &&
            vol.valueMin <= vol.valueMax,
        "valueMin <= valueMax, both finite (" + std::to_string(vol.valueMin) +
            " .. " + std::to_string(vol.valueMax) + ")");

  // worldBounds must be a non-degenerate, ordered AABB (min <= max per axis).
  const Vec3 mn = vol.worldBounds.min, mx = vol.worldBounds.max;
  Check(mn.x <= mx.x && mn.y <= mx.y && mn.z <= mx.z,
        "worldBounds min <= max on every axis");

  // End-to-end: reconstruct glyphs for all voxels and confirm a non-empty,
  // finite mesh. This is the real integration check the foundation exists for.
  const Icosphere ico = MakeIcosphere(2);
  const ShBasisMatrix mat = BuildShBasisMatrix(
      ico.vertices, static_cast<std::size_t>(vol.nCoeffs));
  std::vector<VoxelIndex> voxels;
  voxels.reserve(vol.voxelCount());
  for (int k = 0; k < vol.dims[2]; ++k)
    for (int j = 0; j < vol.dims[1]; ++j)
      for (int i = 0; i < vol.dims[0]; ++i)
        voxels.push_back(VoxelIndex{i, j, k});

  try {
    const GlyphMesh mesh = BuildGlyphs(vol, ico, mat, voxels, GlyphParams{});
    Check(!mesh.empty(), "BuildGlyphs over demo volume -> non-empty mesh");
    Check(mesh.vertexCount() == voxels.size() * ico.vertexCount(),
          "mesh V == nVoxels * icosphere V");
    bool meshFinite = true;
    for (const Vec3& p : mesh.positions) {
      if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
        meshFinite = false;
        break;
      }
    }
    Check(meshFinite, "all glyph positions finite");
  } catch (const std::exception& e) {
    Check(false, std::string("BuildGlyphs over demo volume threw: ") + e.what());
  }
}

// --- (e) glyph/peaks SCENE builders + Open-router classifiers ---------------
// These moved out of the Qt shell into tracto_odf precisely so they could be
// exercised here without Qt. Small synthetic volumes make the budget/mask/pack
// and classification logic assertable.
void TestGlyphScene() {
  std::cout << "[e] glyph_scene\n";

  const int nCoeffs = 45;  // L8 symmetric
  const int nx = 2, ny = 2, nz = 2;
  const std::size_t spatial = static_cast<std::size_t>(nx) * ny * nz;

  // Isotropic fODF: positive DC (coeff 0) in every voxel, zero elsewhere. Below
  // the glyph budget, so every voxel is drawn (no slicing) with lMax = 8.
  OdfVolume fodf;
  fodf.dims[0] = nx; fodf.dims[1] = ny; fodf.dims[2] = nz;
  fodf.nCoeffs = nCoeffs;
  fodf.coeffs.assign(spatial * static_cast<std::size_t>(nCoeffs), 0.0f);
  for (std::size_t v = 0; v < spatial; ++v) fodf.coeffs[v] = 1.0f;  // coeff-0 plane
  fodf.affine = Affine{};
  fodf.valueMin = 0.0f;
  fodf.valueMax = 1.0f;

  const OdfGlyphScene scene = BuildOdfGlyphScene(fodf);
  Check(scene.glyphCount == spatial,
        "BuildOdfGlyphScene draws all voxels below budget (" +
            std::to_string(scene.glyphCount) + " == " + std::to_string(spatial) + ")");
  Check(!scene.sliced, "small fODF -> whole-volume scene (not sliced)");
  Check(scene.lMax == 8, "scene lMax == 8 for nCoeffs 45");
  Check(!scene.vertices.empty() && !scene.indices.empty(),
        "scene has vertices + indices");
  Check(scene.vertices.size() % 9 == 0, "glyph vertices are 9 floats each");
  Check(scene.bounds.min.x <= scene.bounds.max.x &&
            scene.bounds.min.y <= scene.bounds.max.y &&
            scene.bounds.min.z <= scene.bounds.max.z,
        "scene bounds min <= max on every axis");

  // LooksLikeFodf: overwhelmingly-positive DC -> true; a stacked vector field
  // (coeff-0 signed, positive ~half the time) -> false.
  Check(LooksLikeFodf(fodf), "positive-DC volume LooksLikeFodf");
  OdfVolume signed_ = fodf;
  for (std::size_t v = 0; v < spatial; ++v) signed_.coeffs[v] = (v % 2 == 0) ? 1.0f : -1.0f;
  signed_.valueMin = -1.0f;
  Check(!LooksLikeFodf(signed_), "signed-DC volume does NOT LooksLikeFodf");

  // IsEvenSymmetricShCount: the canonical fODF counts pass; a 3-vector peaks
  // count (nCoeffs = 3) is rejected so the router never glyphs a peaks file.
  Check(IsEvenSymmetricShCount(6) && IsEvenSymmetricShCount(45),
        "IsEvenSymmetricShCount accepts 6 and 45");
  Check(!IsEvenSymmetricShCount(3) && !IsEvenSymmetricShCount(1),
        "IsEvenSymmetricShCount rejects 3 (peaks) and 1 (scalar)");

  // Peaks scene: one peak per voxel (nCoeffs = 3), unit +X direction -> one
  // bidirectional segment per voxel, all present (below the segment budget).
  OdfVolume peaks;
  peaks.dims[0] = nx; peaks.dims[1] = ny; peaks.dims[2] = nz;
  peaks.nCoeffs = 3;
  peaks.coeffs.assign(spatial * 3, 0.0f);
  for (std::size_t v = 0; v < spatial; ++v) peaks.coeffs[v] = 1.0f;  // x-component plane
  peaks.affine = Affine{};
  peaks.valueMin = 0.0f;
  peaks.valueMax = 1.0f;

  const PeaksScene ps = BuildPeaksScene(peaks);
  Check(ps.nPeaks == 1, "peaks scene reports 1 dir/voxel");
  Check(ps.segmentCount == spatial,
        "one segment per voxel below budget (" + std::to_string(ps.segmentCount) +
            " == " + std::to_string(spatial) + ")");
  Check(ps.vertices.size() == ps.segmentCount * 12, "peaks verts = 12 floats/segment");

  // AxialSliceIndex: identity affine -> world-z equals k; clamps out-of-range.
  OdfVolume col;
  col.dims[0] = 1; col.dims[1] = 1; col.dims[2] = 4;
  col.nCoeffs = 1;
  col.coeffs.assign(4, 0.0f);
  col.affine = Affine{};
  Check(AxialSliceIndex(col, 2.0f) == 2, "AxialSliceIndex(z=2) == 2");
  Check(AxialSliceIndex(col, 100.0f) == 3, "AxialSliceIndex clamps high to nz-1");
  Check(AxialSliceIndex(col, -5.0f) == 0, "AxialSliceIndex clamps low to 0");
}

}  // namespace

int main() {
  std::cout << "=== ODF module self-test ===\n";

  TestShBasis();
  TestIcosphere();
  TestGlyphBuilder();
  TestGlyphScene();
  TestOdfVolume();

  std::cout << "============================\n";
  if (g_failures == 0) {
    std::cout << "ALL ODF SELFTESTS PASSED\n";
    return 0;
  }
  std::cout << g_failures << " ODF SELFTEST CHECK(S) FAILED\n";
  return 1;
}
