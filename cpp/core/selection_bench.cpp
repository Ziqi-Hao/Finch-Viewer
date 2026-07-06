// Selection backend microbenchmark — the "number" behind enabling OpenMP.
// Times the parallelized CpuSelectionBackend full-set box scan. Run it serial
// (OMP_NUM_THREADS=1) vs all-cores from the same binary to see the speedup:
//
//   ./tools/build_mac.sh selection_bench
//   OMP_NUM_THREADS=1 build/selection_bench data/ORedited.trk   # serial baseline
//   build/selection_bench data/ORedited.trk                     # all cores
//
// Measured 2026-06-29, Apple M4 Pro, data/ORedited.trk (63k streamlines /
// 24M points): 29.5 ms/query serial -> 5.5 ms at 12 threads (~5.3x; ~6.8x at 8).
#include "selection_backend.hpp"
#include "tractogram_store.hpp"
#include "trk_io.hpp"

#include <chrono>
#include <cstdio>
#ifdef HAVE_OPENMP
#include <omp.h>
#endif

using namespace tracto;

int main(int argc, char** argv) {
  const char* path = (argc > 1) ? argv[1] : "data/ORedited.trk";

  TrkHeader hdr;
  TractogramStore store;
  store.streamlines = LoadTrk(path, hdr);
  store.header = hdr;
  BuildSoA(store);
#ifdef HAVE_OPENMP
  const int threads = omp_get_max_threads();
#else
  const int threads = 1;
#endif
  std::printf("threads=%d  loaded %zu streamlines / %zu points from %s\n",
              threads, store.StreamlineCount(), store.TotalPointCount(), path);

  auto cpu = CreateCpuSelectionBackend();
  const Bounds full = RasBounds(store);

  auto warm = cpu->SelectInBox(store, full);  // warmup + sanity (full box = all)
  std::size_t sel = 0;
  for (auto f : warm) sel += f;

  const int iters = 50;
  volatile std::size_t sink = 0;
  const auto t0 = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < iters; ++i) sink += cpu->SelectInBox(store, full).size();
  const auto t1 = std::chrono::high_resolution_clock::now();
  (void)sink;

  const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / iters;
  const double mptsPerSec = store.TotalPointCount() / (ms * 1e3);
  std::printf("CpuSelectionBackend SelectInBox: %.3f ms/query · %.0f Mpts/s · %zu/%zu selected\n",
              ms, mptsPerSec, sel, store.StreamlineCount());
  return 0;
}
