// CPU ODF glyph mesh builder — implementation.
//
// This is the macOS-safe (OpenGL 4.1, no compute / no SSBO) replacement for
// dmri-explorer's shfield_comp.glsl + shfield_vert.glsl GPU path. The math here
// mirrors that reference bit-for-bit so a fODF authored for dmri-explorer
// reconstructs identically:
//   * radius  : dmri-explorer/Engine/shaders/shfield_comp.glsl:36-56 (scaleSphere)
//   * normals : shfield_comp.glsl:64-95 (updateNormals)
//   * per-glyph max-amplitude normalize : shfield_vert.glsl:83-86
//   * direction-encoded color           : shfield_vert.glsl:59-66
//
// Pure C++17 + the odf module headers. No GL, no Qt, no shaders.

#include "glyph_builder.hpp"

#include <cmath>
#include <stdexcept>

namespace tracto {
namespace odf {

Vec3 DirectionColor(Vec3 dir) {
  // Conventional DTI orientation coloring: R=|x|, G=|y|, B=|z| of the unit
  // direction. abs() because a fiber direction and its antipode are the same
  // orientation, so the color must be sign-agnostic.
  const Vec3 u = Normalize(dir);
  return {std::fabs(u.x), std::fabs(u.y), std::fabs(u.z)};
}

GlyphMesh BuildGlyphs(const OdfVolume& volume,
                      const Icosphere& icosphere,
                      const ShBasisMatrix& basisMatrix,
                      const std::vector<VoxelIndex>& voxelIndices,
                      const GlyphParams& params) {
  const std::size_t nDir = icosphere.vertexCount();
  const std::size_t nCoeffs = static_cast<std::size_t>(volume.nCoeffs);

  // Preconditions (per the header): the basis must have been sampled at exactly
  // these icosphere directions, and its coefficient count must match the volume.
  // Without these the matvec below would index out of range.
  if (basisMatrix.nDir != nDir) {
    throw std::invalid_argument(
        "BuildGlyphs: basisMatrix.nDir != icosphere.vertexCount()");
  }
  if (basisMatrix.nCoeffs != nCoeffs) {
    throw std::invalid_argument(
        "BuildGlyphs: basisMatrix.nCoeffs != volume.nCoeffs");
  }

  GlyphMesh mesh;
  if (voxelIndices.empty() || nDir == 0 || icosphere.indices.empty()) {
    return mesh;  // nothing to draw
  }

  // Every glyph contributes the same vertex/index counts (the shared icosphere,
  // only deformed radially), so we can size the output exactly up front.
  const std::size_t glyphCount = voxelIndices.size();
  const std::size_t idxPerGlyph = icosphere.indices.size();
  const std::size_t totalVerts = glyphCount * nDir;
  const std::size_t totalIdx = glyphCount * idxPerGlyph;

  mesh.positions.resize(totalVerts);
  mesh.normals.assign(totalVerts, Vec3{0.0f, 0.0f, 0.0f});  // accumulated below
  mesh.colors.resize(totalVerts);
  mesh.indices.resize(totalIdx);

  // Scratch radius buffer reused per glyph (avoids reallocating per voxel).
  std::vector<float> radius(nDir, 0.0f);

  // FLOAT_EPS matches shfield_comp.glsl:25 — used to reject degenerate triangle
  // edges so a collapsed lobe (radius ~ 0) does not inject NaN normals.
  constexpr float kEdgeEps = 1e-4f;

  for (std::size_t g = 0; g < glyphCount; ++g) {
    const VoxelIndex& vox = voxelIndices[g];
    const float* coeff = volume.voxelCoeffs(vox);  // coeff-outermost stride
    const std::size_t vertexBase = g * nDir;
    const std::size_t indexBase = g * idxPerGlyph;

    // --- 1) Reconstruct radius per direction: radius[v] = sum_c coeff[c]*B[v][c]
    //         then clamp negatives, and track the per-glyph max amplitude.
    // NOTE on the coefficient stride: OdfVolume stores coefficients OUTERMOST
    // (coeff c of voxel is `coeff[c * voxelCount]`), so we step by voxelCount,
    // not by 1. The basis row is contiguous (one row per direction).
    const std::size_t coeffStride = volume.voxelCount();
    float maxAmp = 0.0f;
    for (std::size_t v = 0; v < nDir; ++v) {
      const float* b = basisMatrix.row(v);
      float r = 0.0f;
      for (std::size_t c = 0; c < nCoeffs; ++c) {
        r += coeff[c * coeffStride] * b[c];
      }
      // A glyph radius cannot be negative (a lobe where the SH sum dips below
      // zero is just absent). Clamp before tracking the max so normalization is
      // by the largest *visible* lobe. clampNegative is exposed for a future
      // signed mode that wants the raw value.
      if (params.clampNegative && r < 0.0f) {
        r = 0.0f;
      }
      radius[v] = r;
      if (r > maxAmp) {
        maxAmp = r;
      }
    }

    // Per-glyph normalization factor (shfield_vert.glsl:84-86). When enabled,
    // divide every radius by this glyph's max so all glyphs reach the same
    // nominal size (shape comparison). maxAmp <= 0 means the glyph is empty;
    // guard against divide-by-zero by leaving it unnormalized (matches the
    // reference's `maxAmplitude > 0 ? maxAmplitude : 1.0`).
    float normFactor = 1.0f;
    if (params.normalizePerGlyph && maxAmp > 0.0f) {
      normFactor = 1.0f / maxAmp;
    }
    const float radiusScale = normFactor * params.scale;

    // --- 2) Deform the shared unit sphere and place it at the voxel's world
    //         position. deformed = unitDir * radius * radiusScale; world pos =
    //         VoxelToWorld(affine, voxel) + deformed. We keep the deformed
    //         OFFSET (relative to the voxel center) around for the normal/color
    //         pass so geometry math is in glyph-local space (numerically nicer
    //         than differencing large world coordinates).
    const Vec3 voxelWorld = VoxelToWorld(volume.affine, vox);
    for (std::size_t v = 0; v < nDir; ++v) {
      const Vec3 dir = icosphere.vertices[v];
      const Vec3 offset = dir * (radius[v] * radiusScale);
      mesh.positions[vertexBase + v] = voxelWorld + offset;
      // Color is the direction-encoded fiber color of the (unit) sample
      // direction. Since radius >= 0 after clamp, the deformed offset has the
      // same sign as dir, so normalize(offset) == dir; using dir directly is
      // equivalent and well-defined even when radius == 0 (matches
      // shfield_vert.glsl's abs(normalize(scaledVertice))).
      mesh.colors[vertexBase + v] = DirectionColor(dir);
    }

    // --- 3) Recompute per-vertex normals from the DEFORMED surface
    //         (shfield_comp.glsl:64-95). Accumulate each triangle's face normal
    //         into its three vertices, then normalize once at the end. The
    //         per-glyph uniform scale (radiusScale) does not change normal
    //         DIRECTION, so we use the deformed offsets directly.
    for (std::size_t t = 0; t < idxPerGlyph; t += 3) {
      const unsigned i0 = icosphere.indices[t];
      const unsigned i1 = icosphere.indices[t + 1];
      const unsigned i2 = icosphere.indices[t + 2];

      const Vec3 a = mesh.positions[vertexBase + i0];
      const Vec3 b = mesh.positions[vertexBase + i1];
      const Vec3 c = mesh.positions[vertexBase + i2];

      Vec3 ab = b - a;
      Vec3 ac = c - a;
      // Skip degenerate triangles (collapsed lobe): a zero-length edge or two
      // (anti)parallel edges give a zero cross product whose Normalize would
      // hand back {0,0,0} — accumulating that is a no-op, but the explicit
      // guard also avoids NaN from normalizing a zero edge. Mirrors the
      // reference's length/dot checks (shfield_comp.glsl:82-87).
      if (Length(ab) > kEdgeEps && Length(ac) > kEdgeEps) {
        ab = Normalize(ab);
        ac = Normalize(ac);
        if (std::fabs(Dot(ab, ac)) < 1.0f) {  // not (anti)parallel
          const Vec3 n = Normalize(Cross(ab, ac));
          mesh.normals[vertexBase + i0] = mesh.normals[vertexBase + i0] + n;
          mesh.normals[vertexBase + i1] = mesh.normals[vertexBase + i1] + n;
          mesh.normals[vertexBase + i2] = mesh.normals[vertexBase + i2] + n;
        }
      }
    }

    // --- 4) Emit this glyph's triangle indices, offset into the packed arrays.
    for (std::size_t t = 0; t < idxPerGlyph; ++t) {
      mesh.indices[indexBase + t] =
          static_cast<unsigned>(vertexBase) + icosphere.indices[t];
    }
  }

  // --- 5) Normalize accumulated vertex normals once (smooth shading). A vertex
  //         touched by no valid triangle keeps {0,0,0}; Normalize leaves it
  //         {0,0,0} rather than producing NaN, which a shader treats as an
  //         unlit/black normal — acceptable for an empty/degenerate glyph.
  for (Vec3& n : mesh.normals) {
    n = Normalize(n);
  }

  return mesh;
}

}  // namespace odf
}  // namespace tracto
