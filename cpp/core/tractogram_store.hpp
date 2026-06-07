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
  std::vector<int64_t> offsets;
  std::vector<int64_t> pointCounts;
  std::vector<double> lengthsMm;

  std::size_t StreamlineCount() const { return streamlines.size(); }
  std::size_t TotalPointCount() const { return x.size(); }
  bool Empty() const { return streamlines.empty(); }
};

void BuildSoA(TractogramStore& store);

// RAS-mm axis-aligned bounds over all points (from the SoA x/y/z arrays).
// Returns a unit box when the store has no points.
Bounds RasBounds(const TractogramStore& store);

}  // namespace tracto
