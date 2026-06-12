#include "selection_backend.hpp"
#include "statistics.hpp"
#include "tractogram_store.hpp"
#include "trk_io.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Check(bool ok, const std::string& what) {
  if (ok) {
    std::cout << "[ ok ] " << what << "\n";
  } else {
    std::cout << "[FAIL] " << what << "\n";
    ++failures;
  }
}

bool Near(double a, double b, double tol = 1.0e-6) {
  return std::abs(a - b) <= tol;
}

void WriteI32(char* dst, int32_t value) {
  std::memcpy(dst, &value, sizeof(value));
}

void WriteF32(char* dst, float value) {
  std::memcpy(dst, &value, sizeof(value));
}

tracto::TrkHeader MakeHeader(int32_t nCount) {
  tracto::TrkHeader h;
  std::memcpy(h.raw.data(), "TRACK", 5);
  h.nCount = nCount;
  h.nScalars = 0;
  h.nProperties = 0;
  h.voxelSize = {1.0f, 1.0f, 1.0f};
  h.origin = {0.0f, 0.0f, 0.0f};
  h.voxToRas.fill(0.0);
  h.voxToRas[0] = h.voxToRas[5] = h.voxToRas[10] = h.voxToRas[15] = 1.0;
  WriteI32(h.raw.data() + 988, nCount);
  WriteI32(h.raw.data() + 996, tracto::kTrkHeaderSize);
  WriteF32(h.raw.data() + 12, 1.0f);
  WriteF32(h.raw.data() + 16, 1.0f);
  WriteF32(h.raw.data() + 20, 1.0f);
  for (int i = 0; i < 16; ++i) {
    WriteF32(h.raw.data() + 440 + i * 4, static_cast<float>(h.voxToRas[i]));
  }
  return h;
}

tracto::Streamline MakeStreamline(const std::vector<float>& xyz) {
  tracto::Streamline sl;
  sl.pointCount = static_cast<int32_t>(xyz.size() / 3);
  sl.rawPointData = xyz;  // identity voxToRas in the test header -> SoA == these coords
  return sl;
}

tracto::TractogramStore MakeStore() {
  tracto::TractogramStore store;
  store.header = MakeHeader(3);
  store.streamlines = {
      MakeStreamline({0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f}),
      MakeStreamline({10.0f, 10.0f, 10.0f, 11.0f, 10.0f, 10.0f}),
      MakeStreamline({-2.0f, 0.0f, 0.0f, -2.0f, 1.0f, 0.0f, -2.0f, 2.0f, 0.0f}),
  };
  tracto::BuildSoA(store);
  return store;
}

void TestStoreAndStats() {
  tracto::TractogramStore store = MakeStore();
  Check(store.StreamlineCount() == 3, "synthetic store has 3 streamlines");
  Check(store.TotalPointCount() == 8, "synthetic store has 8 points");
  Check(store.offsets == std::vector<int64_t>({0, 3, 5, 8}), "SoA offsets are correct");
  Check(Near(store.lengthsMm[0], 2.0) && Near(store.lengthsMm[1], 1.0) &&
            Near(store.lengthsMm[2], 2.0),
        "streamline lengths are correct");

  const std::vector<uint8_t> alive = {1, 0, 1};
  const tracto::BasicStats stats = tracto::ComputeBasicStats(store, alive);
  Check(stats.fullCount == 3 && stats.aliveCount == 2 && stats.deadCount == 1,
        "basic counts are correct");
  Check(Near(stats.lengthMm.mean, 2.0) && Near(stats.pointsPerLine.mean, 3.0),
        "basic summaries use alive streamlines only");
}

void TestSlimRehydrate() {
  // Slimming then rehydrating must reproduce the exact SoA cloud (the in-memory
  // memory-saving path for inactive tractograms must be lossless).
  tracto::TractogramStore store = MakeStore();
  const std::vector<float> x0 = store.x, y0 = store.y, z0 = store.z;
  const std::vector<int32_t> sid0 = store.sid;
  const std::vector<int64_t> off0 = store.offsets;

  tracto::SlimStore(store);
  Check(store.x.empty() && store.sid.empty(), "SlimStore frees the SoA point arrays");

  tracto::RehydrateSoA(store);
  Check(store.x == x0 && store.y == y0 && store.z == z0 && store.sid == sid0 &&
            store.offsets == off0,
        "RehydrateSoA reproduces the exact SoA from rawPointData");

  tracto::RehydrateSoA(store);  // idempotent: a present cloud is left untouched
  Check(store.x == x0, "RehydrateSoA is a no-op when the cloud is already present");
}

void TestSelectionBackends() {
  tracto::TractogramStore store = MakeStore();
  auto cpu = tracto::CreateCpuSelectionBackend();
  auto grid = tracto::CreateGridSelectionBackend();
  cpu->Build(store);
  grid->Build(store);

  const std::vector<tracto::Bounds> boxes = {
      {{-0.5, 1.5, -0.5, 0.5, -0.5, 0.5}},
      {{-3.0, -1.0, 0.5, 1.5, -0.5, 0.5}},
      {{-100.0, 100.0, -100.0, 100.0, -100.0, 100.0}},
      {{20.0, 21.0, 20.0, 21.0, 20.0, 21.0}},
  };
  for (std::size_t i = 0; i < boxes.size(); ++i) {
    Check(cpu->SelectInBox(store, boxes[i]) == grid->SelectInBox(store, boxes[i]),
          "CPU and grid selection agree for box " + std::to_string(i));
  }
}

void TestTrkSubsetRoundTrip() {
  tracto::TractogramStore store = MakeStore();
  const std::vector<uint8_t> keep = {1, 0, 1};
  const std::string path =
      (std::filesystem::temp_directory_path() / "tracto_core_selftest.trk").string();
  Check(tracto::WriteTrkSubset(path, store.header, store.streamlines, keep),
        "WriteTrkSubset returns success");

  tracto::TrkHeader loadedHeader;
  const std::vector<tracto::Streamline> loaded = tracto::LoadTrk(path, loadedHeader);
  Check(loadedHeader.nCount == 2, "roundtrip header count is updated");
  Check(loaded.size() == 2, "roundtrip loads kept streamlines only");
  Check(loaded.size() == 2 && loaded[0].pointCount == 3 && loaded[1].pointCount == 3,
        "roundtrip preserves kept streamline point counts");
  std::remove(path.c_str());
}

}  // namespace

int main() {
  TestStoreAndStats();
  TestSlimRehydrate();
  TestSelectionBackends();
  TestTrkSubsetRoundTrip();
  if (failures != 0) {
    std::cout << failures << " core self-test failure(s)\n";
    return 1;
  }
  std::cout << "core self-test passed\n";
  return 0;
}
