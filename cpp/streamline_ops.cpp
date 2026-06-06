#include "streamline_ops.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>

namespace tracto {
namespace {

bool PointInBounds(const float* p, const Bounds& b) {
  return p[0] >= b.v[0] && p[0] <= b.v[1] &&
         p[1] >= b.v[2] && p[1] <= b.v[3] &&
         p[2] >= b.v[4] && p[2] <= b.v[5];
}

}  // namespace

bool StreamlineInBounds(const Streamline& sl, const Bounds& b) {
  const float* pts = sl.rasPoints.data();
  for (int32_t i = 0; i < sl.pointCount; ++i) {
    if (PointInBounds(pts + static_cast<std::size_t>(i) * 3, b)) {
      return true;
    }
  }
  return false;
}

std::array<unsigned char, 3> DirectionRgbFromPoints(const float* points,
                                                    std::size_t pointCount,
                                                    std::size_t pointIndex) {
  if (pointCount < 2) {
    return {0, 0, 0};
  }

  const auto point = [&](std::size_t p, int c) -> float {
    return points[p * 3 + static_cast<std::size_t>(c)];
  };

  float d[3] = {0.0f, 0.0f, 0.0f};
  if (pointIndex == 0) {
    for (int c = 0; c < 3; ++c) {
      d[c] = point(1, c) - point(0, c);
    }
  } else if (pointIndex == pointCount - 1) {
    for (int c = 0; c < 3; ++c) {
      d[c] = point(pointIndex, c) - point(pointIndex - 1, c);
    }
  } else {
    for (int c = 0; c < 3; ++c) {
      d[c] = 0.5f * (point(pointIndex + 1, c) - point(pointIndex - 1, c));
    }
  }

  const float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
  if (n <= 0.0f) {
    return {0, 0, 0};
  }

  return {
      static_cast<unsigned char>(std::clamp(std::abs(d[0] / n) * 255.0f, 0.0f, 255.0f)),
      static_cast<unsigned char>(std::clamp(std::abs(d[1] / n) * 255.0f, 0.0f, 255.0f)),
      static_cast<unsigned char>(std::clamp(std::abs(d[2] / n) * 255.0f, 0.0f, 255.0f)),
  };
}

std::array<unsigned char, 3> DirectionRgb(const Streamline& sl, int32_t i) {
  return DirectionRgbFromPoints(sl.rasPoints.data(),
                                static_cast<std::size_t>(sl.pointCount),
                                static_cast<std::size_t>(i));
}

std::vector<int> MakeDisplayIndices(int fullCount, int displayN, uint64_t seed) {
  std::vector<int> indices(static_cast<std::size_t>(fullCount));
  std::iota(indices.begin(), indices.end(), 0);
  if (fullCount > displayN) {
    std::mt19937_64 rng(seed);
    std::shuffle(indices.begin(), indices.end(), rng);
    indices.resize(static_cast<std::size_t>(displayN));
    std::sort(indices.begin(), indices.end());
  }
  return indices;
}

std::vector<int> MakeDisplayIndicesFromAlive(const std::vector<uint8_t>& alive,
                                             int displayN,
                                             uint64_t seed) {
  std::vector<int> indices;
  indices.reserve(alive.size());
  for (std::size_t i = 0; i < alive.size(); ++i) {
    if (alive[i]) {
      indices.push_back(static_cast<int>(i));
    }
  }

  if (static_cast<int>(indices.size()) > displayN) {
    std::mt19937_64 rng(seed);
    std::shuffle(indices.begin(), indices.end(), rng);
    indices.resize(static_cast<std::size_t>(displayN));
    std::sort(indices.begin(), indices.end());
  }

  return indices;
}

}  // namespace tracto
