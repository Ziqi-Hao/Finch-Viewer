#include "track_density.hpp"

#include <algorithm>
#include <cmath>

namespace tracto {

Volume BuildTrackDensity(const TractogramStore& store,
                         const std::vector<uint8_t>& alive,
                         float voxelSizeMm) {
  Volume vol;
  const std::size_t nStream = store.StreamlineCount();
  if (store.x.empty() || store.offsets.size() != nStream + 1) return vol;

  const Bounds b = RasBounds(store);
  const float ex = static_cast<float>(b.v[1] - b.v[0]);
  const float ey = static_cast<float>(b.v[3] - b.v[2]);
  const float ez = static_cast<float>(b.v[5] - b.v[4]);

  // Pick a voxel size: the requested one, enlarged if needed so the largest grid
  // dimension stays within a safe cap (keeps the texture small and the build fast).
  constexpr int kMaxDim = 320;
  float vs = std::max(0.1f, voxelSizeMm);
  const float maxExtent = std::max({ex, ey, ez});
  if (maxExtent / vs > static_cast<float>(kMaxDim)) vs = maxExtent / static_cast<float>(kMaxDim);
  const float margin = 2.0f * vs;  // a little padding so endpoints aren't clipped

  const float ox = static_cast<float>(b.v[0]) - margin;
  const float oy = static_cast<float>(b.v[2]) - margin;
  const float oz = static_cast<float>(b.v[4]) - margin;
  auto dimFor = [&](float extent) {
    return std::max(1, static_cast<int>(std::ceil((extent + 2.0f * margin) / vs)));
  };
  const int nx = dimFor(ex), ny = dimFor(ey), nz = dimFor(ez);

  vol.dims[0] = nx; vol.dims[1] = ny; vol.dims[2] = nz;
  vol.data.assign(static_cast<std::size_t>(nx) * ny * nz, 0.0f);
  // voxel (i,j,k) -> RAS mm: diagonal scale `vs` + origin (row-major affine).
  vol.voxelToWorld = Identity();
  vol.voxelToWorld.m[0] = vs; vol.voxelToWorld.m[5] = vs; vol.voxelToWorld.m[10] = vs;
  vol.voxelToWorld.m[3] = ox; vol.voxelToWorld.m[7] = oy; vol.voxelToWorld.m[11] = oz;

  // Accumulate track density. For each streamline we increment a voxel only when
  // the point crosses into a *different* voxel than the previous point, so the
  // count approximates "streamlines through the voxel" rather than "points in the
  // voxel" (the latter is biased by point spacing).
  const float inv = 1.0f / vs;
  const float* X = store.x.data();
  const float* Y = store.y.data();
  const float* Z = store.z.data();
  for (std::size_t s = 0; s < nStream; ++s) {
    if (s < alive.size() && !alive[s]) continue;
    const std::int64_t beg = store.offsets[s], end = store.offsets[s + 1];
    std::int64_t lastIdx = -1;
    for (std::int64_t p = beg; p < end; ++p) {
      const int i = static_cast<int>((X[p] - ox) * inv);
      const int j = static_cast<int>((Y[p] - oy) * inv);
      const int k = static_cast<int>((Z[p] - oz) * inv);
      if (i < 0 || i >= nx || j < 0 || j >= ny || k < 0 || k >= nz) continue;
      const std::int64_t idx = i + static_cast<std::int64_t>(nx) * (j + static_cast<std::int64_t>(ny) * k);
      if (idx == lastIdx) continue;  // same voxel as the previous point -> don't double-count
      vol.data[static_cast<std::size_t>(idx)] += 1.0f;
      lastIdx = idx;
    }
  }

  // Robust window: cap the displayed max at the 99th percentile of non-zero
  // counts so a handful of crossing-heavy voxels don't flatten the rest.
  std::vector<float> counts;
  counts.reserve(vol.data.size() / 4);
  for (float v : vol.data) { if (v > 0.0f) counts.push_back(v); }
  vol.valueMin = 0.0f;
  if (counts.empty()) {
    vol.valueMax = 1.0f;
  } else {
    const std::size_t q = static_cast<std::size_t>(0.99 * static_cast<double>(counts.size() - 1));
    std::nth_element(counts.begin(), counts.begin() + q, counts.end());
    vol.valueMax = std::max(1.0f, counts[q]);
  }
  return vol;
}

}  // namespace tracto
