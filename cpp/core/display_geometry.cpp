#include "display_geometry.hpp"

#include "streamline_ops.hpp"

#include <algorithm>
#include <array>

namespace tracto {
namespace {

void AppendVertex(std::vector<float>& out,
                  const float* xyz,
                  const std::array<unsigned char, 3>& rgb) {
  out.push_back(xyz[0]);
  out.push_back(xyz[1]);
  out.push_back(xyz[2]);
  out.push_back(static_cast<float>(rgb[0]) / 255.0f);
  out.push_back(static_cast<float>(rgb[1]) / 255.0f);
  out.push_back(static_cast<float>(rgb[2]) / 255.0f);
}

void ExpandBounds(Bounds& b, const float* xyz, bool& initialized) {
  if (!initialized) {
    b.v[0] = b.v[1] = xyz[0];
    b.v[2] = b.v[3] = xyz[1];
    b.v[4] = b.v[5] = xyz[2];
    initialized = true;
    return;
  }
  b.v[0] = std::min<double>(b.v[0], xyz[0]);
  b.v[1] = std::max<double>(b.v[1], xyz[0]);
  b.v[2] = std::min<double>(b.v[2], xyz[1]);
  b.v[3] = std::max<double>(b.v[3], xyz[1]);
  b.v[4] = std::min<double>(b.v[4], xyz[2]);
  b.v[5] = std::max<double>(b.v[5], xyz[2]);
}

}  // namespace

LineGeometry BuildDisplayLineGeometry(const TractogramStore& store,
                                      const std::vector<uint8_t>& alive,
                                      int displayN,
                                      int dispStep,
                                      uint64_t seed) {
  LineGeometry geo;
  // Sample a STABLE subset of the full set (independent of `alive`) and emit only
  // its alive members. Because the sample doesn't change when `alive` changes, an
  // edit leaves the surviving lines exactly in place and the deleted ones simply
  // vanish — no reshuffle (the Python "NaN-hide" stable-view behaviour). The
  // sample is only re-picked when displayN / the streamline count change.
  const std::vector<int> display =
      MakeDisplayIndices(static_cast<int>(store.StreamlineCount()), displayN, seed);
  const int step = std::max(1, dispStep);
  bool boundsInitialized = false;
  geo.spans.reserve(display.size());  // exact upper bound: ≤1 span per displayed line

  for (int fullIndex : display) {
    if (static_cast<std::size_t>(fullIndex) >= alive.size() || !alive[static_cast<std::size_t>(fullIndex)]) {
      continue;  // dead -> skip (vanishes in place; survivors keep their slots)
    }
    // Read points from the SoA cloud (offsets[i]..offsets[i+1]); the per-streamline
    // rasPoints AoS is freed after BuildSoA to halve the loaded point memory.
    const std::size_t base = static_cast<std::size_t>(store.offsets[static_cast<std::size_t>(fullIndex)]);
    const int32_t pointCount =
        static_cast<int32_t>(store.offsets[static_cast<std::size_t>(fullIndex) + 1]) -
        static_cast<int32_t>(base);
    if (pointCount < 2) {
      continue;
    }

    // Decimate, always keeping the final point so endpoints stay anchored.
    std::vector<float> points;
    points.reserve((static_cast<std::size_t>(pointCount) / static_cast<std::size_t>(step) + 2) * 3);
    int32_t lastPushed = -1;
    for (int32_t p = 0; p < pointCount; p += step) {
      const std::size_t src = base + static_cast<std::size_t>(p);
      points.push_back(store.x[src]);
      points.push_back(store.y[src]);
      points.push_back(store.z[src]);
      lastPushed = p;
    }
    if (lastPushed != pointCount - 1) {
      const std::size_t src = base + static_cast<std::size_t>(pointCount - 1);
      points.push_back(store.x[src]);
      points.push_back(store.y[src]);
      points.push_back(store.z[src]);
    }

    const std::size_t count = points.size() / 3;
    if (count < 2) {
      continue;
    }

    const auto firstVertex = static_cast<uint32_t>(geo.vertices.size() / 6);
    for (std::size_t p = 0; p + 1 < count; ++p) {
      const float* a = points.data() + p * 3;
      const float* b = points.data() + (p + 1) * 3;
      AppendVertex(geo.vertices, a, DirectionRgbFromPoints(points.data(), count, p));
      AppendVertex(geo.vertices, b, DirectionRgbFromPoints(points.data(), count, p + 1));
      ExpandBounds(geo.bounds, a, boundsInitialized);
      ExpandBounds(geo.bounds, b, boundsInitialized);
    }
    const auto vcount = static_cast<uint32_t>(geo.vertices.size() / 6) - firstVertex;
    geo.spans.push_back({fullIndex, firstVertex, vcount});
  }

  if (!boundsInitialized) {
    geo.bounds.v[0] = geo.bounds.v[2] = geo.bounds.v[4] = -1.0;
    geo.bounds.v[1] = geo.bounds.v[3] = geo.bounds.v[5] = 1.0;
  }
  return geo;
}

}  // namespace tracto
