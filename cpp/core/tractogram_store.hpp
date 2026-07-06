#pragma once

#include "bounds.hpp"
#include "trk_io.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tracto {

struct TractogramStore {
  TrkHeader header;
  std::vector<Streamline> streamlines;

  // Structure-of-arrays RASMM point cloud. This is the CPU SIMD / OpenMP /
  // CUDA-friendly representation used by selection and statistics kernels.
  std::vector<float> x;
  std::vector<float> y;
  std::vector<float> z;
  std::vector<int32_t> sid;
  std::vector<int64_t> offsets;  // CSR: size n+1; points-per-line i = offsets[i+1]-offsets[i]
  std::vector<double> lengthsMm;

  std::size_t StreamlineCount() const { return streamlines.size(); }
  std::size_t TotalPointCount() const { return x.size(); }
  bool Empty() const { return streamlines.empty(); }
};

void BuildSoA(TractogramStore& store);

// Free the per-point SoA arrays (x/y/z/sid) of a tractogram that is loaded but
// not currently active — they're only needed for selection/editing. rawPointData
// (save) and the small per-streamline metadata are kept, so RehydrateSoA can
// rebuild the cloud in memory when the tractogram is made active again.
void SlimStore(TractogramStore& store);

// Rebuild the SoA point cloud (x/y/z/sid/…) of a slimmed store from its retained
// rawPointData (no file re-read). No-op if the cloud is already present.
void RehydrateSoA(TractogramStore& store);

// RAS-mm axis-aligned bounds over all points (from the SoA x/y/z arrays).
// Returns a unit box when the store has no points.
Bounds RasBounds(const TractogramStore& store);

}  // namespace tracto
