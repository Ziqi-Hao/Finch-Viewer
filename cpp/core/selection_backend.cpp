#include "selection_backend.hpp"

#ifdef HAVE_OPENMP
#include <omp.h>
#endif

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tracto {
namespace {

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
    const std::size_t n = tractogram.StreamlineCount();
    const std::size_t totalPoints = tractogram.TotalPointCount();
    std::vector<uint8_t> flags(n, 0);

#ifdef HAVE_OPENMP
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
        if (tractogram.x[idx] >= bounds.v[0] && tractogram.x[idx] <= bounds.v[1] &&
            tractogram.y[idx] >= bounds.v[2] && tractogram.y[idx] <= bounds.v[3] &&
            tractogram.z[idx] >= bounds.v[4] && tractogram.z[idx] <= bounds.v[5]) {
          local[static_cast<std::size_t>(tractogram.sid[idx])] = 1;
        }
      }
    }

#pragma omp parallel for schedule(static)
    for (int64_t i = 0; i < static_cast<int64_t>(n); ++i) {
      const auto si = static_cast<std::size_t>(i);
      uint8_t hit = 0;
      for (const auto& local : partial) {
        hit = static_cast<uint8_t>(hit || local[si]);
      }
      flags[si] = hit;
    }
#else
    for (std::size_t p = 0; p < totalPoints; ++p) {
      if (tractogram.x[p] >= bounds.v[0] && tractogram.x[p] <= bounds.v[1] &&
          tractogram.y[p] >= bounds.v[2] && tractogram.y[p] <= bounds.v[3] &&
          tractogram.z[p] >= bounds.v[4] && tractogram.z[p] <= bounds.v[5]) {
        flags[static_cast<std::size_t>(tractogram.sid[p])] = 1;
      }
    }
#endif

    return flags;
  }
};

}  // namespace

std::unique_ptr<SelectionBackend> CreateCpuSelectionBackend() {
  return std::make_unique<CpuSelectionBackend>();
}

}  // namespace tracto
