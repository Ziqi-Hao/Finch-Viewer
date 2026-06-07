#pragma once

// Build GPU-ready line geometry from the surviving streamlines. This is the
// "data -> GPU buffer view" boundary from TOOLBOX_PLAN: VTK-free and Qt-free,
// so any renderer (Qt viewport now, others later) shares one decimation and
// direction-colouring policy instead of re-deriving it per backend.

#include "bounds.hpp"
#include "tractogram_store.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tracto {

// Maps one displayed streamline to its contiguous run of vertices in the buffer
// (so a renderer can highlight a subset without re-deriving geometry).
struct DisplaySpan {
  int fullId;             // full-set streamline index
  uint32_t firstVertex;   // first vertex of this streamline in `vertices`
  uint32_t vertexCount;   // number of vertices (GL_LINES, so even)
};

struct LineGeometry {
  // Interleaved [x, y, z, r, g, b] per vertex (rgb in 0..1). Drawn as GL_LINES,
  // i.e. two consecutive vertices per segment.
  std::vector<float> vertices;
  std::vector<DisplaySpan> spans;  // one per displayed streamline, in buffer order
  Bounds bounds;

  std::size_t VertexCount() const { return vertices.size() / 6; }
};

// Sample up to `displayN` alive streamlines (deterministically, by `seed`),
// keep every `dispStep`-th point (always including the last), and emit
// direction-coloured GL_LINES segments. `bounds` covers the emitted points.
LineGeometry BuildDisplayLineGeometry(const TractogramStore& store,
                                      const std::vector<uint8_t>& alive,
                                      int displayN,
                                      int dispStep,
                                      uint64_t seed);

}  // namespace tracto
