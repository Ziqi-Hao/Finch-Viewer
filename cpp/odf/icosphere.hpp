#pragma once

// Unit icosphere: the shared sample sphere whose vertices are the directions an
// ODF glyph is evaluated at, and whose triangle topology becomes the glyph mesh.
//
// Mirrors dmri-explorer's Slicer::Sphere (dmri-explorer/Engine/src/sphere.cpp).
// Pure-CPU geometry generation; no GL, no Qt. ONE icosphere is generated and
// shared by every glyph — glyph_builder.hpp only scales each vertex radially per
// voxel, so vertex count / index buffer are identical across all glyphs.
//
// ALGORITHM (sphere.cpp):
//  * Seed: a regular icosahedron — 12 vertices from three mutually-orthogonal
//    golden rectangles using X = 0.525731112119133606, Z = 0.850650808352039932
//    (sphere.cpp:13-24) and 20 triangular faces (sphere.cpp:25-30).
//  * Each subdivision level splits every triangle into 4 via its three edge
//    midpoints (linear interpolation at 0.5), with shared edges de-duplicated by
//    a normalized (v0<v1) key map so each midpoint vertex is created once
//    (sphere.cpp:33-41, 176-244).
//  * Every vertex is projected back onto the unit sphere (radius 1) by going
//    through spherical coords (theta=acos(z), phi=atan2(y,x)) and back to
//    Cartesian at r=1 (sphere.cpp:94-152).
//
// VERTEX / EDGE / FACE COUNTS by subdivision level (Euler V-E+F=2 holds):
//   level 0:  V=12,  E=30,   F=20
//   level 1:  V=42,  E=90,   F=80
//   level 2:  V=162, E=360,  F=320
//   level 3:  V=642, E=1440, F=1280
// Per level F x4, E x3, and V grows by (new midpoints) = E_prev.

#include "odf_types.hpp"  // tracto::odf::Vec3

#include <cstddef>
#include <vector>

namespace tracto {
namespace odf {

// A triangulated unit sphere. `vertices` are unit directions (also the SH sample
// directions). `indices` are triangle vertex indices, 3 per face, CCW when
// viewed from outside. positions[i] doubles as the outward normal of vertex i
// because every vertex lies on the unit sphere.
struct Icosphere {
  std::vector<Vec3> vertices;       // unit directions, size V
  std::vector<unsigned> indices;    // 3 * F, triangle list

  std::size_t vertexCount() const { return vertices.size(); }
  std::size_t triangleCount() const { return indices.size() / 3; }
};

// Build a unit icosphere with `subdiv` midpoint-subdivision levels (subdiv >= 0;
// 0 = bare icosahedron). For ODF glyphs, subdiv 2 (162 verts) or 3 (642 verts)
// gives smooth glyphs at a CPU cost that scales with vertexCount * nCoeffs per
// voxel. Counts follow the table above.
Icosphere MakeIcosphere(int subdiv);

}  // namespace odf
}  // namespace tracto
