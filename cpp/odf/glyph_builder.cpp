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

namespace {

// FLOAT_EPS matches shfield_comp.glsl:25 — rejects degenerate triangle edges so a
// collapsed lobe (radius ~ 0) does not inject NaN normals.
constexpr float kEdgeEps = 1e-4f;

// Deform one glyph from a per-direction `radius` buffer and write its vertices,
// accumulated normals, colors, and triangle indices into the packed mesh. Shared by
// the SH builder (radius = SH reconstruction) and the SF builder (radius = the file's
// amplitudes) — only the radius SOURCE differs, so the deform/normal/index math lives
// here once. Steps mirror shfield_comp.glsl:64-95 + shfield_vert.glsl:59-86.
void EmitGlyphFromRadii(GlyphMesh& mesh, const Icosphere& sphere, const Vec3& voxelWorld,
                        const std::vector<float>& radius, float radiusScale,
                        std::size_t vertexBase, std::size_t indexBase) {
  const std::size_t nDir = sphere.vertexCount();
  const std::size_t idxPerGlyph = sphere.indices.size();

  // 1) Deform the unit sphere radially and place it at the voxel centre. Keep the
  //    deformed offset (glyph-local) for the normal pass — nicer numerically than
  //    differencing large world coords. Color = direction-encoded fiber color; since
  //    radius >= 0, normalize(offset) == the unit sample direction.
  for (std::size_t v = 0; v < nDir; ++v) {
    const Vec3 dir = sphere.vertices[v];
    mesh.positions[vertexBase + v] = voxelWorld + dir * (radius[v] * radiusScale);
    mesh.colors[vertexBase + v] = DirectionColor(dir);
  }

  // 2) Recompute per-vertex normals from the DEFORMED surface: accumulate each
  //    triangle's face normal into its three vertices (normalized once by the caller).
  for (std::size_t t = 0; t < idxPerGlyph; t += 3) {
    const unsigned i0 = sphere.indices[t], i1 = sphere.indices[t + 1], i2 = sphere.indices[t + 2];
    const Vec3 a = mesh.positions[vertexBase + i0];
    const Vec3 b = mesh.positions[vertexBase + i1];
    const Vec3 c = mesh.positions[vertexBase + i2];
    Vec3 ab = b - a, ac = c - a;
    if (Length(ab) > kEdgeEps && Length(ac) > kEdgeEps) {  // skip collapsed lobes
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

  // 3) Emit this glyph's triangle indices, offset into the packed arrays.
  for (std::size_t t = 0; t < idxPerGlyph; ++t) {
    mesh.indices[indexBase + t] = static_cast<unsigned>(vertexBase) + sphere.indices[t];
  }
}

// Allocate the packed mesh for `glyphCount` glyphs of a shared sphere; returns false
// (empty mesh) when there is nothing to draw.
bool SizeGlyphMesh(GlyphMesh& mesh, const Icosphere& sphere, std::size_t glyphCount) {
  if (glyphCount == 0 || sphere.vertexCount() == 0 || sphere.indices.empty()) return false;
  const std::size_t totalVerts = glyphCount * sphere.vertexCount();
  mesh.positions.resize(totalVerts);
  mesh.normals.assign(totalVerts, Vec3{0.0f, 0.0f, 0.0f});  // accumulated in EmitGlyphFromRadii
  mesh.colors.resize(totalVerts);
  mesh.indices.resize(glyphCount * sphere.indices.size());
  return true;
}

}  // namespace

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
  if (!SizeGlyphMesh(mesh, icosphere, voxelIndices.size())) return mesh;  // nothing to draw

  std::vector<float> radius(nDir, 0.0f);     // scratch, reused per glyph
  const std::size_t coeffStride = volume.voxelCount();  // coeffs are OUTERMOST

  for (std::size_t g = 0; g < voxelIndices.size(); ++g) {
    const VoxelIndex& vox = voxelIndices[g];
    const float* coeff = volume.voxelCoeffs(vox);
    const std::size_t vertexBase = g * nDir;

    // Radius per direction = SH reconstruction sum_c coeff[c]*B[v][c]; clamp negative
    // lobes (a lobe where the SH sum dips below zero is just absent), track the max.
    float maxAmp = 0.0f;
    for (std::size_t v = 0; v < nDir; ++v) {
      const float* b = basisMatrix.row(v);
      float r = 0.0f;
      for (std::size_t c = 0; c < nCoeffs; ++c) r += coeff[c * coeffStride] * b[c];
      if (params.clampNegative && r < 0.0f) r = 0.0f;
      radius[v] = r;
      if (r > maxAmp) maxAmp = r;
    }
    // Per-glyph normalize (shfield_vert.glsl:84-86): divide by the max so glyphs reach
    // the same nominal size; maxAmp<=0 (empty glyph) leaves it unnormalized.
    const float normFactor = (params.normalizePerGlyph && maxAmp > 0.0f) ? 1.0f / maxAmp : 1.0f;
    EmitGlyphFromRadii(mesh, icosphere, VoxelToWorld(volume.affine, vox), radius,
                       normFactor * params.scale, vertexBase, g * icosphere.indices.size());
  }

  for (Vec3& n : mesh.normals) n = Normalize(n);  // smooth shading, normalize once
  return mesh;
}

GlyphMesh BuildGlyphsSF(const OdfVolume& volume,
                        const Icosphere& sphere,
                        const std::vector<VoxelIndex>& voxelIndices,
                        const GlyphParams& params) {
  // Discrete-sphere (SF) glyphs: the volume stores per-direction AMPLITUDES (one per
  // `sphere` vertex), not SH coefficients — so the radius IS the amplitude, no basis
  // matvec. `sphere` must be the exact sphere the amplitudes were sampled on (its order
  // matters: amplitude[v] belongs to sphere.vertices[v]). Everything downstream (deform,
  // normals, color, indices) is identical to the SH path via EmitGlyphFromRadii.
  const std::size_t nDir = sphere.vertexCount();
  if (static_cast<std::size_t>(volume.nCoeffs) != nDir) {
    throw std::invalid_argument("BuildGlyphsSF: volume.nCoeffs != sphere.vertexCount()");
  }
  GlyphMesh mesh;
  if (!SizeGlyphMesh(mesh, sphere, voxelIndices.size())) return mesh;

  std::vector<float> radius(nDir, 0.0f);
  const std::size_t ampStride = volume.voxelCount();  // amplitudes are OUTERMOST too

  for (std::size_t g = 0; g < voxelIndices.size(); ++g) {
    const VoxelIndex& vox = voxelIndices[g];
    const float* amp = volume.voxelCoeffs(vox);  // amplitude v at amp[v * ampStride]
    float maxAmp = 0.0f;
    for (std::size_t v = 0; v < nDir; ++v) {
      float r = amp[v * ampStride];
      if (params.clampNegative && r < 0.0f) r = 0.0f;  // ODF amplitudes are >= 0 anyway
      radius[v] = r;
      if (r > maxAmp) maxAmp = r;
    }
    const float normFactor = (params.normalizePerGlyph && maxAmp > 0.0f) ? 1.0f / maxAmp : 1.0f;
    EmitGlyphFromRadii(mesh, sphere, VoxelToWorld(volume.affine, vox), radius,
                       normFactor * params.scale, g * nDir, g * sphere.indices.size());
  }

  for (Vec3& n : mesh.normals) n = Normalize(n);
  return mesh;
}

}  // namespace odf
}  // namespace tracto
