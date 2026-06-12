#include "tractogram_store.hpp"

#include <algorithm>
#include <array>
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

  // Transform raw points -> RAS mm straight into the SoA. We read rawPointData
  // (kept for save) and apply the header's voxel->RAS affine inline, so the load
  // path never materializes a separate per-streamline rasPoints cloud — that
  // intermediate used to ~double the transient memory during load. Matches the
  // (row-major) transform in trk_io's TransformPoint.
  const std::array<double, 16>& M = store.header.voxToRas;
  const std::size_t comps = static_cast<std::size_t>(3 + std::max<int>(0, store.header.nScalars));
  std::size_t cursor = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const Streamline& sl = store.streamlines[i];
    const std::size_t count = static_cast<std::size_t>(sl.pointCount);
    if (sl.rawPointData.size() != count * comps) {
      throw std::runtime_error("invalid raw point buffer size");
    }

    double length = 0.0;
    for (std::size_t p = 0; p < count; ++p) {
      const std::size_t src = p * comps;
      const double rx = sl.rawPointData[src], ry = sl.rawPointData[src + 1],
                   rz = sl.rawPointData[src + 2];
      const float px = static_cast<float>(M[0] * rx + M[1] * ry + M[2] * rz + M[3]);
      const float py = static_cast<float>(M[4] * rx + M[5] * ry + M[6] * rz + M[7]);
      const float pz = static_cast<float>(M[8] * rx + M[9] * ry + M[10] * rz + M[11]);
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

void SlimStore(TractogramStore& store) {
  // Free the big per-point arrays; keep rawPointData + small per-streamline
  // metadata so RehydrateSoA can rebuild the cloud. swap-with-empty frees capacity.
  std::vector<float>().swap(store.x);
  std::vector<float>().swap(store.y);
  std::vector<float>().swap(store.z);
  std::vector<int32_t>().swap(store.sid);
}

void RehydrateSoA(TractogramStore& store) {
  if (!store.x.empty() || store.streamlines.empty()) return;  // already present / nothing to do
  BuildSoA(store);  // rebuilds the SoA from the retained rawPointData (no file re-read)
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
