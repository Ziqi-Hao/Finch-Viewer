#include "statistics.hpp"

#include "nifti_io.hpp"  // Volume (full type for the histogram/LUT compute)

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace tracto {
namespace {

NumericSummary Summarize(std::vector<double> values) {
  NumericSummary summary;
  if (values.empty()) {
    return summary;
  }

  summary.valid = true;
  summary.min = values.front();
  summary.max = values.front();

  long double sum = 0.0L;
  for (double value : values) {
    sum += value;
    summary.min = std::min(summary.min, value);
    summary.max = std::max(summary.max, value);
  }
  summary.mean = static_cast<double>(sum / static_cast<long double>(values.size()));

  long double squaredError = 0.0L;
  for (double value : values) {
    const long double d = static_cast<long double>(value) - summary.mean;
    squaredError += d * d;
  }
  summary.stddev =
      std::sqrt(static_cast<double>(squaredError / static_cast<long double>(values.size())));

  const std::size_t mid = values.size() / 2;
  std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(mid), values.end());
  summary.median = values[mid];
  if (values.size() % 2 == 0) {
    const auto lowerIt =
        std::max_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(mid));
    summary.median = 0.5 * (*lowerIt + summary.median);
  }

  return summary;
}

}  // namespace

BasicStats ComputeBasicStats(const TractogramStore& store,
                             const std::vector<uint8_t>& aliveFull) {
  if (aliveFull.size() != store.StreamlineCount()) {
    throw std::runtime_error("alive mask size does not match tractogram");
  }
  if (store.lengthsMm.size() != store.StreamlineCount() ||
      store.offsets.size() != store.StreamlineCount() + 1) {
    throw std::runtime_error("tractogram statistics arrays are not initialized");
  }

  BasicStats stats;
  stats.fullCount = store.StreamlineCount();

  std::vector<double> lengths;
  std::vector<double> pointCounts;
  lengths.reserve(stats.fullCount);
  pointCounts.reserve(stats.fullCount);

  for (std::size_t i = 0; i < stats.fullCount; ++i) {
    if (!aliveFull[i]) {
      continue;
    }
    ++stats.aliveCount;
    lengths.push_back(store.lengthsMm[i]);
    pointCounts.push_back(static_cast<double>(store.offsets[i + 1] - store.offsets[i]));
  }

  stats.deadCount = stats.fullCount - stats.aliveCount;
  if (stats.fullCount > 0) {
    stats.deletedPercent =
        100.0 * static_cast<double>(stats.deadCount) / static_cast<double>(stats.fullCount);
  }

  stats.lengthMm = Summarize(std::move(lengths));
  stats.pointsPerLine = Summarize(std::move(pointCounts));
  return stats;
}

NumericSummary SampleVolumeAlongTracts(const TractogramStore& store, const Volume& volume,
                                       const std::vector<uint8_t>& aliveFull) {
  if (volume.data.empty() || store.x.empty() ||
      aliveFull.size() != store.StreamlineCount()) {
    return NumericSummary{};  // invalid: nothing to sample
  }

  const Mat4 inv = InverseAffine(volume.voxelToWorld);  // world RAS mm -> voxel index
  const int nx = volume.dims[0], ny = volume.dims[1], nz = volume.dims[2];
  const std::size_t total = store.TotalPointCount();

  std::vector<double> vals;
  vals.reserve(total);
  for (std::size_t p = 0; p < total; ++p) {
    if (!aliveFull[static_cast<std::size_t>(store.sid[p])]) continue;  // point of a deleted line
    const float X = store.x[p], Y = store.y[p], Z = store.z[p];
    // Nearest voxel (np.round-style via std::lround); drop points outside the grid
    // (Python samples NaN there and discards it).
    const long i = std::lround(inv.m[0] * X + inv.m[1] * Y + inv.m[2] * Z + inv.m[3]);
    const long j = std::lround(inv.m[4] * X + inv.m[5] * Y + inv.m[6] * Z + inv.m[7]);
    const long k = std::lround(inv.m[8] * X + inv.m[9] * Y + inv.m[10] * Z + inv.m[11]);
    if (i < 0 || i >= nx || j < 0 || j >= ny || k < 0 || k >= nz) continue;
    const std::size_t idx = static_cast<std::size_t>(i) +
                            static_cast<std::size_t>(nx) *
                                (static_cast<std::size_t>(j) +
                                 static_cast<std::size_t>(ny) * static_cast<std::size_t>(k));
    vals.push_back(static_cast<double>(volume.data[idx]));  // x-fastest layout (nifti_io)
  }
  return Summarize(std::move(vals));
}

VolumeHistogram ComputeVolumeHistogram(const Volume& v) {
  // Adaptive intensity axis: a few bright outliers shouldn't squash the bulk of
  // the data to the left, so cap the displayed max at the 99.5th percentile. The
  // default window uses that same robust range (FSLeyes-style auto contrast).
  VolumeHistogram out;
  const float dmin = v.valueMin;
  const float dmax = (v.valueMax > v.valueMin) ? v.valueMax : v.valueMin + 1.0f;
  constexpr int kFine = 1024;
  std::vector<double> fine(kFine, 0.0);
  const double finv = static_cast<double>(kFine - 1) / (static_cast<double>(dmax) - dmin);
  for (float s : v.data) fine[std::clamp(static_cast<int>((static_cast<double>(s) - dmin) * finv),
                                         0, kFine - 1)] += 1.0;
  const double total = std::max(1.0, static_cast<double>(v.data.size()));
  double cum = 0.0;
  int robBin = kFine - 1;
  for (int i = 0; i < kFine; ++i) { cum += fine[i]; if (cum >= 0.995 * total) { robBin = i; break; } }

  out.dispMin = dmin;
  out.dispMax = dmin + static_cast<float>(robBin + 1) / kFine * (dmax - dmin);
  if (out.dispMax <= out.dispMin) out.dispMax = dmax;
  out.winLo = out.dispMin;
  out.winHi = out.dispMax;

  // 160 display bins over [dispMin, dispMax], aggregated from the fine bins.
  constexpr int kDisp = 160;
  out.bins.assign(kDisp, 0.0f);
  const float span = std::max(1e-9f, out.dispMax - out.dispMin);
  for (int j = 0; j < kFine; ++j) {
    const double cj = dmin + (j + 0.5) / kFine * (static_cast<double>(dmax) - dmin);
    const int db = std::clamp(static_cast<int>((cj - out.dispMin) / span * kDisp), 0, kDisp - 1);
    out.bins[db] += static_cast<float>(fine[j]);
  }
  return out;
}

VolumeHistogram ComputeStatHistogram(const Volume& v, int intentCode) {
  // An fMRI stat map (z/t/r/…) is SIGNED and symmetric about 0, so the meaningful
  // axis is |stat|, not the raw min/max grayscale window (which a single outlier
  // would wreck). Build a |stat| histogram over the non-zero voxels (the map is
  // mostly exact-0 background) and seed a [threshold, cap] window that is data-aware
  // and editable: cap from a robust high percentile so a few hot peaks don't squash
  // the scale; threshold by statistic kind (z/t≈2.3, r≈0.3 — FSL/SPM conventions),
  // else a robust percentile so low-magnitude noise starts hidden.
  VolumeHistogram out;  // data is finite — LoadNifti sanitizes NaN/Inf to 0 at load
  float absMax = 0.0f;
  for (float s : v.data) absMax = std::max(absMax, std::abs(s));
  if (absMax <= 0.0f) absMax = 1.0f;

  constexpr int kFine = 1024;
  std::vector<double> fine(kFine, 0.0);
  const double finv = static_cast<double>(kFine - 1) / absMax;
  double nz = 0.0;  // count of non-zero voxels (background excluded from percentiles)
  for (float s : v.data) {
    const float a = std::abs(s);
    if (a <= 0.0f) continue;
    fine[std::clamp(static_cast<int>(a * finv), 0, kFine - 1)] += 1.0;
    nz += 1.0;
  }
  if (nz <= 0.0) nz = 1.0;
  auto percentile = [&](double frac) {
    double cum = 0.0;
    for (int i = 0; i < kFine; ++i) {
      cum += fine[i];
      if (cum >= frac * nz) return static_cast<float>((i + 1) / static_cast<double>(kFine) * absMax);
    }
    return absMax;
  };

  const float cap = percentile(0.98);                          // default high handle
  const float axisMax = std::max(cap, percentile(0.999));      // histogram axis (headroom over cap)
  float thr;
  switch (intentCode) {
    case 5: case 3: thr = 2.3f; break;            // ZSCORE / TTEST (FSL cluster-forming default)
    case 2:         thr = 0.3f; break;            // CORREL (Pearson r)
    default:        thr = percentile(0.80); break;  // unknown signed map: hide the low-magnitude bulk
  }
  thr = std::clamp(thr, 0.0f, 0.9f * cap);        // keep threshold below the cap (winLo < winHi)

  out.dispMin = 0.0f;
  out.dispMax = axisMax;
  out.winLo = thr;   // |stat| threshold (low handle)
  out.winHi = cap;   // |stat| cap       (high handle)

  // 160 display bins over [0, axisMax], aggregated from the fine |stat| bins.
  constexpr int kDisp = 160;
  out.bins.assign(kDisp, 0.0f);
  for (int j = 0; j < kFine; ++j) {
    const double cj = (j + 0.5) / kFine * absMax;
    const int db = std::clamp(static_cast<int>(cj / axisMax * kDisp), 0, kDisp - 1);
    out.bins[db] += static_cast<float>(fine[j]);
  }
  return out;
}

LabelLut ComputeLabelLut(const Volume& v) {
  // Build a dense RGBA table indexed by integer label value (the shader does
  // texelFetch(uLut, idx)). Index 0 is background → transparent; each non-zero
  // label gets a distinct hue from the golden-ratio walk (maximally spread,
  // stable per index). Width = maxLabel+1, capped so a stray huge value can't
  // allocate an absurd texture (the shader clamps out-of-range indices).
  LabelLut out;
  constexpr int kMaxLut = 4096;  // covers FreeSurfer aseg (~2035) and atlases
  int maxLabel = 0;
  for (float s : v.data) {
    const int idx = static_cast<int>(s + 0.5f);
    if (idx > maxLabel) maxLabel = idx;
  }
  const int width = std::clamp(maxLabel + 1, 2, kMaxLut);
  out.rgba.assign(static_cast<std::size_t>(width) * 4, 0.0f);  // index 0 stays (0,0,0,0)

  auto hsvToRgb = [](float h, float s, float val, float& r, float& g, float& b) {
    const float i = std::floor(h * 6.0f);
    const float f = h * 6.0f - i;
    const float p = val * (1.0f - s), q = val * (1.0f - f * s), t = val * (1.0f - (1.0f - f) * s);
    switch (static_cast<int>(i) % 6) {
      case 0: r = val; g = t; b = p; break;
      case 1: r = q; g = val; b = p; break;
      case 2: r = p; g = val; b = t; break;
      case 3: r = p; g = q; b = val; break;
      case 4: r = t; g = p; b = val; break;
      default: r = val; g = p; b = q; break;
    }
  };
  constexpr float kGolden = 0.61803398875f;
  for (int idx = 1; idx < width; ++idx) {
    const float hue = std::fmod(static_cast<float>(idx) * kGolden, 1.0f);
    float r, g, b;
    hsvToRgb(hue, 0.65f, 0.95f, r, g, b);
    float* px = &out.rgba[static_cast<std::size_t>(idx) * 4];
    px[0] = r; px[1] = g; px[2] = b; px[3] = 1.0f;
  }
  out.width = width;
  return out;
}

}  // namespace tracto
