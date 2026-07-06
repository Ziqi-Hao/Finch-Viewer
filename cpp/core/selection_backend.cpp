#include "selection_backend.hpp"

#ifdef HAVE_OPENMP
#include <omp.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace tracto {
namespace {

// One full pass over the SoA point cloud, OR-ing every in-box point into its
// owning streamline's flag. OpenMP-parallel (per-thread partials, then reduced)
// when available; the serial path is result-identical. `flags` must be sized to
// StreamlineCount() and zero-initialized by the caller. Shared by the linear
// backend and the grid backend's large-box fallback so the benchmarked ~5x
// parallel scan (selection_bench.cpp: 29.5ms->5.5ms @ 24M points) is used on
// both paths instead of being dead code behind the grid.
void SelectInBoxScan(const TractogramStore& tg, const Bounds& box,
                     std::vector<uint8_t>& flags) {
  const std::size_t totalPoints = tg.TotalPointCount();
  const auto inBox = [&](std::size_t p) {
    return tg.x[p] >= box.v[0] && tg.x[p] <= box.v[1] && tg.y[p] >= box.v[2] &&
           tg.y[p] <= box.v[3] && tg.z[p] >= box.v[4] && tg.z[p] <= box.v[5];
  };

#ifdef HAVE_OPENMP
  const std::size_t n = tg.StreamlineCount();
  const int threadCount = omp_get_max_threads();
  std::vector<std::vector<uint8_t>> partial(
      static_cast<std::size_t>(threadCount), std::vector<uint8_t>(n, 0));

#pragma omp parallel
  {
    const int tid = omp_get_thread_num();
    std::vector<uint8_t>& local = partial[static_cast<std::size_t>(tid)];

#pragma omp for schedule(static)
    for (int64_t p = 0; p < static_cast<int64_t>(totalPoints); ++p) {
      const auto idx = static_cast<std::size_t>(p);
      if (inBox(idx)) local[static_cast<std::size_t>(tg.sid[idx])] = 1;
    }
  }

#pragma omp parallel for schedule(static)
  for (int64_t i = 0; i < static_cast<int64_t>(n); ++i) {
    const auto si = static_cast<std::size_t>(i);
    uint8_t hit = 0;
    for (const auto& local : partial) hit = static_cast<uint8_t>(hit || local[si]);
    flags[si] |= hit;
  }
#else
  for (std::size_t p = 0; p < totalPoints; ++p)
    if (inBox(p)) flags[static_cast<std::size_t>(tg.sid[p])] = 1;
#endif
}

class CpuSelectionBackend final : public SelectionBackend {
 public:
  std::string Name() const override {
#ifdef HAVE_OPENMP
    return "CPU/OpenMP SoA scan";
#else
    return "CPU SoA scan";
#endif
  }

  std::vector<uint8_t> SelectInBox(const TractogramStore& tractogram,
                                   const Bounds& bounds) const override {
    std::vector<uint8_t> flags(tractogram.StreamlineCount(), 0);
    SelectInBoxScan(tractogram, bounds, flags);
    return flags;
  }
};

// Uniform-voxel grid index (ported from the Python GridSelector). Build() bins
// every point into `cell`-mm voxels laid out as a CSR bucket array; a query
// gathers only the buckets overlapping the box, so localized selections cost
// O(box) instead of O(all points).
class GridSelectionBackend final : public SelectionBackend {
 public:
  std::string Name() const override { return "Grid (uniform-voxel CSR)"; }
  bool IndexBuilt() const override { return built_; }

  // INVARIANT: Build indexes the FULL point cloud once. Editing mutates only the
  // caller's alive mask — it never moves or removes points — so the index stays
  // valid across edits and is deliberately NOT rebuilt on edit (callers intersect
  // query flags with the alive mask). Do not add a rebuild-on-edit: it would cost
  // an O(N) counting sort per edit for zero correctness gain.
  void Build(const TractogramStore& tg, const Bounds* bounds = nullptr) override {
    built_ = false;
    order_.clear();
    start_.clear();
    const std::size_t total = tg.TotalPointCount();
    if (total == 0) return;

    // Reuse the caller's cached RasBounds when supplied — it is the identical
    // x/y/z minmax the index would compute, and CellIndex clamps every point
    // into range, so the bucket layout is byte-identical either way.
    double hi[3];
    if (bounds != nullptr) {
      lo_[0] = bounds->v[0]; lo_[1] = bounds->v[2]; lo_[2] = bounds->v[4];
      hi[0] = bounds->v[1]; hi[1] = bounds->v[3]; hi[2] = bounds->v[5];
    } else {
      const auto xr = std::minmax_element(tg.x.begin(), tg.x.end());
      const auto yr = std::minmax_element(tg.y.begin(), tg.y.end());
      const auto zr = std::minmax_element(tg.z.begin(), tg.z.end());
      lo_[0] = *xr.first; lo_[1] = *yr.first; lo_[2] = *zr.first;
      hi[0] = *xr.second; hi[1] = *yr.second; hi[2] = *zr.second;
    }
    for (int a = 0; a < 3; ++a)
      dims_[a] = std::max<int64_t>(
          1, static_cast<int64_t>(std::floor((hi[a] - lo_[a]) / cell_)) + 1);

    const int64_t nb = dims_[0] * dims_[1] * dims_[2];
    // Guard against a pathological cell count (e.g. an outlier/corrupt coordinate
    // blowing up one axis): start_ would otherwise allocate gigabytes. Leaving
    // built_ = false makes SelectInBox fall back to the (always-correct) linear scan.
    if (nb <= 0 || nb > (1LL << 25)) return;  // ~33M cells cap (~256 MB for start_)
    std::vector<int64_t> bucket(total);        // int64: the bucket id ranges over [0, nb)
    start_.assign(static_cast<std::size_t>(nb) + 1, 0);
    for (std::size_t p = 0; p < total; ++p) {
      const int64_t b = (CellIndex(tg.x[p], 0) * dims_[1] + CellIndex(tg.y[p], 1)) * dims_[2] +
                        CellIndex(tg.z[p], 2);
      bucket[p] = b;
      ++start_[static_cast<std::size_t>(b) + 1];
    }
    for (int64_t b = 0; b < nb; ++b)  // counts -> CSR offsets (prefix sum)
      start_[static_cast<std::size_t>(b) + 1] += start_[static_cast<std::size_t>(b)];

    order_.resize(total);  // point indices sorted by bucket (counting sort)
    std::vector<int64_t> cursor(start_.begin(), start_.end() - 1);
    for (std::size_t p = 0; p < total; ++p)
      order_[static_cast<std::size_t>(cursor[static_cast<std::size_t>(bucket[p])]++)] =
          static_cast<int32_t>(p);
    built_ = true;
  }

  std::vector<uint8_t> SelectInBox(const TractogramStore& tg,
                                   const Bounds& box) const override {
    std::vector<uint8_t> flags(tg.StreamlineCount(), 0);
    if (tg.TotalPointCount() == 0) return flags;
    if (!built_) { SelectInBoxScan(tg, box, flags); return flags; }  // safety fallback

    const int64_t ix0 = RangeLo(box.v[0], 0), ix1 = RangeHi(box.v[1], 0);
    const int64_t iy0 = RangeLo(box.v[2], 1), iy1 = RangeHi(box.v[3], 1);
    const int64_t iz0 = RangeLo(box.v[4], 2), iz1 = RangeHi(box.v[5], 2);
    if (ix0 > ix1 || iy0 > iy1 || iz0 > iz1) return flags;  // box misses the grid

    // Estimate how many points the box would gather (cheap: CSR offset math, no
    // point access). For a box covering a large fraction of the cloud, the
    // cache-friendly linear sweep beats the grid's random-access gather
    // (measured crossover ~ total/8 on the reference set), so fall back to it.
    int64_t candidates = 0;
    for (int64_t ix = ix0; ix <= ix1; ++ix)
      for (int64_t iy = iy0; iy <= iy1; ++iy) {
        const int64_t base = (ix * dims_[1] + iy) * dims_[2];
        candidates += start_[static_cast<std::size_t>(base + iz1 + 1)] -
                      start_[static_cast<std::size_t>(base + iz0)];
      }
    if (candidates > static_cast<int64_t>(tg.TotalPointCount() / 8)) {
      SelectInBoxScan(tg, box, flags);
      return flags;
    }

    for (int64_t ix = ix0; ix <= ix1; ++ix) {
      for (int64_t iy = iy0; iy <= iy1; ++iy) {
        const int64_t base = (ix * dims_[1] + iy) * dims_[2];
        const int64_t s = start_[static_cast<std::size_t>(base + iz0)];
        const int64_t e = start_[static_cast<std::size_t>(base + iz1 + 1)];  // contiguous iz span
        for (int64_t k = s; k < e; ++k) {
          const std::size_t p = static_cast<std::size_t>(order_[static_cast<std::size_t>(k)]);
          if (tg.x[p] >= box.v[0] && tg.x[p] <= box.v[1] && tg.y[p] >= box.v[2] &&
              tg.y[p] <= box.v[3] && tg.z[p] >= box.v[4] && tg.z[p] <= box.v[5])
            flags[static_cast<std::size_t>(tg.sid[p])] = 1;
        }
      }
    }
    return flags;
  }

 private:
  int64_t CellIndex(double c, int axis) const {
    return std::clamp<int64_t>(static_cast<int64_t>(std::floor((c - lo_[axis]) / cell_)), 0,
                               dims_[axis] - 1);
  }
  int64_t RangeLo(double v, int axis) const {
    return std::max<int64_t>(0, static_cast<int64_t>(std::floor((v - lo_[axis]) / cell_)));
  }
  int64_t RangeHi(double v, int axis) const {
    return std::min<int64_t>(dims_[axis] - 1,
                             static_cast<int64_t>(std::floor((v - lo_[axis]) / cell_)));
  }

  double cell_ = 3.0;       // voxel size (mm), matches the Python default
  double lo_[3] = {0, 0, 0};
  int64_t dims_[3] = {1, 1, 1};
  std::vector<int32_t> order_;  // point indices, sorted by bucket (CSR payload)
  std::vector<int64_t> start_;  // CSR bucket offsets, size nbuckets + 1
  bool built_ = false;
};

}  // namespace

std::unique_ptr<SelectionBackend> CreateCpuSelectionBackend() {
  return std::make_unique<CpuSelectionBackend>();
}

std::unique_ptr<SelectionBackend> CreateGridSelectionBackend() {
  return std::make_unique<GridSelectionBackend>();
}

}  // namespace tracto
