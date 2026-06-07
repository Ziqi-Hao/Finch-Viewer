// Unit icosphere generation — see icosphere.hpp for the public contract.
//
// Mirrors dmri-explorer's Slicer::Primitive::Sphere (dmri-explorer/Engine/src/
// sphere.cpp): an icosahedron seed (12 verts / 20 faces) followed by `subdiv`
// rounds of midpoint subdivision, with every vertex projected onto the unit
// sphere. The reference round-trips each vertex through spherical coordinates
// (theta=acos z, phi=atan2 y,x; then back to Cartesian at r=1) to renormalize.
// That is just an indirect unit-length projection, so here we project directly
// with Normalize() — same result, no trig, and no special-casing of the poles
// the spherical round-trip needs. Output is identical: unit-length vertices.

#include "icosphere.hpp"

#include <cstdint>
#include <map>
#include <utility>

namespace tracto {
namespace odf {

namespace {

// Regular icosahedron seed: 12 vertices on three mutually-orthogonal golden
// rectangles. X/Z are the canonical icosahedron constants (sphere.cpp:13-14);
// each seed vertex already has length sqrt(X^2 + Z^2) == 1, i.e. it lies on the
// unit sphere, so Normalize() below is a no-op for level 0.
constexpr float kX = 0.525731112119133606f;
constexpr float kZ = 0.850650808352039932f;

constexpr Vec3 kSeedVertices[12] = {
    {-kX, 0.0f, kZ}, {kX, 0.0f, kZ}, {-kX, 0.0f, -kZ}, {kX, 0.0f, -kZ},
    {0.0f, kZ, kX}, {0.0f, kZ, -kX}, {0.0f, -kZ, kX}, {0.0f, -kZ, -kX},
    {kZ, kX, 0.0f}, {-kZ, kX, 0.0f}, {kZ, -kX, 0.0f}, {-kZ, -kX, 0.0f}};

// 20 triangular faces, CCW from outside (sphere.cpp:25-30).
constexpr unsigned kSeedIndices[60] = {
    0, 1, 4,  0, 4, 9,  9, 4, 5,  4, 8, 5,  4, 1, 8,
    8, 1, 10, 8, 10, 3, 5, 8, 3,  5, 3, 2,  2, 3, 7,
    7, 3, 10, 7, 10, 6, 7, 6, 11, 11, 6, 0, 0, 6, 1,
    6, 10, 1, 9, 11, 0, 9, 2, 11, 9, 5, 2,  7, 11, 2};

// Order-independent key for an undirected edge {a, b}: the same midpoint must be
// shared by the two triangles adjacent to that edge, so {a,b} and {b,a} must map
// to one stored vertex. Storing (min, max) collapses both orderings to one key.
using EdgeKey = std::pair<unsigned, unsigned>;
EdgeKey MakeEdgeKey(unsigned a, unsigned b) {
  return (a < b) ? EdgeKey{a, b} : EdgeKey{b, a};
}

}  // namespace

Icosphere MakeIcosphere(int subdiv) {
  Icosphere sphere;

  // Seed: copy the icosahedron, renormalizing each vertex to the unit sphere.
  // (The seed is already unit-length; Normalize keeps this robust if the
  // constants are ever edited and documents the invariant "vertices are unit".)
  sphere.vertices.reserve(12);
  for (const Vec3& v : kSeedVertices) {
    sphere.vertices.push_back(Normalize(v));
  }
  sphere.indices.assign(std::begin(kSeedIndices), std::end(kSeedIndices));

  if (subdiv < 0) subdiv = 0;

  // Each level: split every triangle into 4 by its three edge midpoints. New
  // midpoint vertices are de-duplicated per edge so shared edges create one
  // vertex (without this, V would balloon and the mesh would crack at seams).
  for (int level = 0; level < subdiv; ++level) {
    std::vector<unsigned> oldIndices;
    oldIndices.swap(sphere.indices);  // sphere.indices is now empty to refill
    sphere.indices.reserve(oldIndices.size() * 4);

    // Edge -> index of its (already created) midpoint vertex, for this level.
    std::map<EdgeKey, unsigned> midpointOf;

    for (std::size_t t = 0; t + 2 < oldIndices.size(); t += 3) {
      const unsigned a = oldIndices[t];
      const unsigned b = oldIndices[t + 1];
      const unsigned c = oldIndices[t + 2];

      // Get (or create) the midpoint vertex of an edge. Midpoint = average of
      // the two endpoints, then projected back to the unit sphere — the linear
      // average sinks below the surface, so normalization is what makes this an
      // *ico-sphere* and not just a refined icosahedron.
      auto midpoint = [&](unsigned i0, unsigned i1) -> unsigned {
        const EdgeKey key = MakeEdgeKey(i0, i1);
        const auto found = midpointOf.find(key);
        if (found != midpointOf.end()) return found->second;

        const Vec3 mid = Normalize((sphere.vertices[i0] + sphere.vertices[i1]) * 0.5f);
        const auto index = static_cast<unsigned>(sphere.vertices.size());
        sphere.vertices.push_back(mid);
        midpointOf.emplace(key, index);
        return index;
      };

      const unsigned ab = midpoint(a, b);
      const unsigned bc = midpoint(b, c);
      const unsigned ca = midpoint(c, a);

      // Four child triangles (corner, corner, corner, center), each CCW so the
      // outward winding of the parent is preserved.
      const unsigned children[12] = {a, ab, ca,
                                     ab, b, bc,
                                     ca, bc, c,
                                     ab, bc, ca};
      sphere.indices.insert(sphere.indices.end(), std::begin(children), std::end(children));
    }
  }

  return sphere;
}

}  // namespace odf
}  // namespace tracto
