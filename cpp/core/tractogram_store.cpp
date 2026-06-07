#include "tractogram_store.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace tracto {

void BuildSoA(TractogramStore& store) {
  const std::size_t n = store.streamlines.size();
  store.offsets.assign(n + 1, 0);
  store.pointCounts.assign(n, 0);
  store.lengthsMm.assign(n, 0.0);

  std::size_t totalPoints = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const Streamline& sl = store.streamlines[i];
    if (sl.pointCount < 0) {
      throw std::runtime_error("negative streamline point count");
    }
    store.pointCounts[i] = sl.pointCount;
    totalPoints += static_cast<std::size_t>(sl.pointCount);
    store.offsets[i + 1] = static_cast<int64_t>(totalPoints);
  }

  store.x.resize(totalPoints);
  store.y.resize(totalPoints);
  store.z.resize(totalPoints);
  store.sid.resize(totalPoints);

  std::size_t cursor = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const Streamline& sl = store.streamlines[i];
    const std::size_t count = static_cast<std::size_t>(sl.pointCount);
    if (sl.rasPoints.size() != count * 3) {
      throw std::runtime_error("invalid RAS point buffer size");
    }

    double length = 0.0;
    for (std::size_t p = 0; p < count; ++p) {
      const std::size_t src = p * 3;
      const float px = sl.rasPoints[src];
      const float py = sl.rasPoints[src + 1];
      const float pz = sl.rasPoints[src + 2];
      store.x[cursor] = px;
      store.y[cursor] = py;
      store.z[cursor] = pz;
      store.sid[cursor] = static_cast<int32_t>(i);

      if (p > 0) {
        const std::size_t prev = cursor - 1;
        const double dx = static_cast<double>(px) - store.x[prev];
        const double dy = static_cast<double>(py) - store.y[prev];
        const double dz = static_cast<double>(pz) - store.z[prev];
        length += std::sqrt(dx * dx + dy * dy + dz * dz);
      }
      ++cursor;
    }
    store.lengthsMm[i] = length;
  }
}

Bounds RasBounds(const TractogramStore& store) {
  Bounds b;
  if (store.x.empty()) {
    b.v[0] = b.v[2] = b.v[4] = -1.0;
    b.v[1] = b.v[3] = b.v[5] = 1.0;
    return b;
  }
  const auto xr = std::minmax_element(store.x.begin(), store.x.end());
  const auto yr = std::minmax_element(store.y.begin(), store.y.end());
  const auto zr = std::minmax_element(store.z.begin(), store.z.end());
  b.v[0] = *xr.first;  b.v[1] = *xr.second;
  b.v[2] = *yr.first;  b.v[3] = *yr.second;
  b.v[4] = *zr.first;  b.v[5] = *zr.second;
  return b;
}

}  // namespace tracto
