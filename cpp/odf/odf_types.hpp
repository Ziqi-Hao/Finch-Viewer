#pragma once

// Small self-contained POD types for the pure-CPU ODF module.
//
// WHY a private Vec3/Affine instead of cpp/core/render_math.hpp's Vec3/Mat4:
// the ODF foundation is intentionally standalone (CPU-only, zlib + C++17 stdlib
// only — no GL/Qt) so it can be unit-tested and reused without dragging in the
// renderer's math/camera. To stay link-safe if a future translation unit pulls
// in BOTH this header and render_math.hpp, every ODF type lives in the nested
// namespace `tracto::odf`, so `tracto::odf::Vec3` never collides with
// `tracto::Vec3`. Header-only, no dependencies beyond <array>/<cstddef>.

#include <array>
#include <cmath>
#include <cstddef>

namespace tracto {
namespace odf {

// 3-component float vector. Used for positions (world RAS mm), unit directions,
// normals, and RGB colors alike — context decides the meaning.
struct Vec3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 v, float s) { return {v.x * s, v.y * s, v.z * s}; }

inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

inline Vec3 Cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline float Length(Vec3 v) {
  // No sqrt-of-zero guard at the type level; callers normalizing degenerate
  // glyph normals add their own epsilon. Kept trivial on purpose.
  return std::sqrt(Dot(v, v));
}

inline Vec3 Normalize(Vec3 v) {
  const float n = Length(v);
  return n > 0.0f ? Vec3{v.x / n, v.y / n, v.z / n} : Vec3{0.0f, 0.0f, 0.0f};
}

// Discrete voxel coordinate (i, j, k) — an index into the ODF grid, NOT a world
// position. build_glyphs() takes a list of these to say which voxels to render.
struct VoxelIndex {
  int i = 0;
  int j = 0;
  int k = 0;
};

// 4x4 row-major homogeneous affine: voxel index (i, j, k) -> world RAS mm.
// Row-major matches cpp/core/render_math.hpp's Mat4 so the world coordinates
// this produces line up exactly with the streamline/scalar-volume space, even
// though we deliberately do not share that type. Row r, column c is m[r*4 + c].
// The bottom row is conventionally (0, 0, 0, 1).
struct Affine {
  std::array<float, 16> m{{1, 0, 0, 0,
                           0, 1, 0, 0,
                           0, 0, 1, 0,
                           0, 0, 0, 1}};

  float& at(int r, int c) { return m[static_cast<std::size_t>(r) * 4 + c]; }
  float at(int r, int c) const { return m[static_cast<std::size_t>(r) * 4 + c]; }
};

// Map an integer voxel index through the affine into world RAS mm.
// Ignores the (implicit) homogeneous w divide — the affine is assumed pure
// affine (w == 1), which is true for every NIfTI sform/qform/pixdim matrix.
inline Vec3 VoxelToWorld(const Affine& a, const VoxelIndex& v) {
  const float i = static_cast<float>(v.i);
  const float j = static_cast<float>(v.j);
  const float k = static_cast<float>(v.k);
  return {a.at(0, 0) * i + a.at(0, 1) * j + a.at(0, 2) * k + a.at(0, 3),
          a.at(1, 0) * i + a.at(1, 1) * j + a.at(1, 2) * k + a.at(1, 3),
          a.at(2, 0) * i + a.at(2, 1) * j + a.at(2, 2) * k + a.at(2, 3)};
}

// Same map for a continuous (fractional) voxel coordinate, e.g. for placing a
// glyph at a voxel center (i + 0.5, j + 0.5, k + 0.5) if a future caller wants
// cell-centered glyphs rather than node-centered.
inline Vec3 VoxelToWorld(const Affine& a, float i, float j, float k) {
  return {a.at(0, 0) * i + a.at(0, 1) * j + a.at(0, 2) * k + a.at(0, 3),
          a.at(1, 0) * i + a.at(1, 1) * j + a.at(1, 2) * k + a.at(1, 3),
          a.at(2, 0) * i + a.at(2, 1) * j + a.at(2, 2) * k + a.at(2, 3)};
}

// World-space axis-aligned bounding box (RAS mm). Self-contained so this module
// does not depend on cpp/core/bounds.hpp. min/max are inclusive corners.
struct AABB {
  Vec3 min{0.0f, 0.0f, 0.0f};
  Vec3 max{0.0f, 0.0f, 0.0f};

  Vec3 center() const { return (min + max) * 0.5f; }
  Vec3 extent() const { return max - min; }
};

}  // namespace odf
}  // namespace tracto
