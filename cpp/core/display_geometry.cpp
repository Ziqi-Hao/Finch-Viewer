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
  const std::vector<int> display = MakeDisplayIndicesFromAlive(alive, displayN, seed);
  const int step = std::max(1, dispStep);
  bool boundsInitialized = false;

  for (int fullIndex : display) {
    const Streamline& sl = store.streamlines[static_cast<std::size_t>(fullIndex)];
    if (sl.pointCount < 2) {
      continue;
    }

    // Decimate, always keeping the final point so endpoints stay anchored.
    std::vector<float> points;
    points.reserve((static_cast<std::size_t>(sl.pointCount) / static_cast<std::size_t>(step) + 2) * 3);
    int32_t lastPushed = -1;
    for (int32_t p = 0; p < sl.pointCount; p += step) {
      const std::size_t src = static_cast<std::size_t>(p) * 3;
      points.push_back(sl.rasPoints[src]);
      points.push_back(sl.rasPoints[src + 1]);
      points.push_back(sl.rasPoints[src + 2]);
      lastPushed = p;
    }
    if (lastPushed != sl.pointCount - 1) {
      const std::size_t src = static_cast<std::size_t>(sl.pointCount - 1) * 3;
      points.push_back(sl.rasPoints[src]);
      points.push_back(sl.rasPoints[src + 1]);
      points.push_back(sl.rasPoints[src + 2]);
    }

    const std::size_t count = points.size() / 3;
    if (count < 2) {
      continue;
    }

    for (std::size_t p = 0; p + 1 < count; ++p) {
      const float* a = points.data() + p * 3;
      const float* b = points.data() + (p + 1) * 3;
      AppendVertex(geo.vertices, a, DirectionRgbFromPoints(points.data(), count, p));
      AppendVertex(geo.vertices, b, DirectionRgbFromPoints(points.data(), count, p + 1));
      ExpandBounds(geo.bounds, a, boundsInitialized);
      ExpandBounds(geo.bounds, b, boundsInitialized);
    }
  }

  if (!boundsInitialized) {
    geo.bounds.v[0] = geo.bounds.v[2] = geo.bounds.v[4] = -1.0;
    geo.bounds.v[1] = geo.bounds.v[3] = geo.bounds.v[5] = 1.0;
  }
  return geo;
}

}  // namespace tracto
