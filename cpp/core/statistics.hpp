#pragma once

#include "tractogram_store.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tracto {

struct Volume;  // cpp/core/nifti_io.hpp — full type needed only in statistics.cpp

struct NumericSummary {
  bool valid = false;
  double mean = 0.0;
  double median = 0.0;
  double stddev = 0.0;
  double min = 0.0;
  double max = 0.0;
};

struct BasicStats {
  std::size_t fullCount = 0;
  std::size_t aliveCount = 0;
  std::size_t deadCount = 0;
  double deletedPercent = 0.0;
  NumericSummary lengthMm;
  NumericSummary pointsPerLine;
  NumericSummary volumeOnTract;  // valid only when a scalar volume was sampled
};

BasicStats ComputeBasicStats(const TractogramStore& store,
                             const std::vector<uint8_t>& aliveFull);

// Sample a scalar volume at every ALIVE streamline point (nearest voxel; world->
// voxel via the volume's inverse affine, coordinates np.round-style rounded) and
// summarise the in-bounds values. Mirrors the Python reference's "Volume on tract"
// statistic (volume.py `sample` + editor `_volume_pool`). Out-of-bounds points are
// dropped. DIVERGENCE: C++ LoadNifti sanitizes in-bounds NaN/Inf to 0, so a NaN
// voxel inside the field contributes 0 here where Python drops it — negligible for
// anatomical scalars (FA/MD), which carry no in-mask NaN. Returns an invalid
// summary if the volume or the point cloud is empty.
NumericSummary SampleVolumeAlongTracts(const TractogramStore& store, const Volume& volume,
                                       const std::vector<uint8_t>& aliveFull);

// Display histogram + default contrast window for a scalar background volume,
// computed off a core Volume so the UI layer that owns per-layer state stays a
// thin adapter. (The numeric policy moved out of the Qt shell to be testable.)
struct VolumeHistogram {
  std::vector<float> bins;  // display-resolution histogram over [dispMin, dispMax]
  float dispMin = 0.0f;
  float dispMax = 1.0f;
  float winLo = 0.0f;       // default window handles (low/high)
  float winHi = 1.0f;
};

// Grayscale volume: robust (99.5th-percentile-capped) intensity axis + histogram,
// FSLeyes-style auto contrast. winLo/winHi default to the full [dispMin, dispMax].
VolumeHistogram ComputeVolumeHistogram(const Volume& v);

// Signed fMRI stat map (z/t/r/…): |stat| histogram over the non-zero voxels + a
// [threshold, cap] window; threshold seeded by NIfTI intent_code (z/t≈2.3, r≈0.3,
// else a robust percentile). dispMin is 0 (the axis is |stat|).
VolumeHistogram ComputeStatHistogram(const Volume& v, int intentCode);

// Dense RGBA colour table indexed by integer label value (index 0 = transparent
// background); distinct golden-ratio hues per label. width = maxLabel+1 (capped).
struct LabelLut {
  std::vector<float> rgba;  // width*4 floats
  int width = 0;
};
LabelLut ComputeLabelLut(const Volume& v);

}  // namespace tracto
