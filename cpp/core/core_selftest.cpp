#include "nifti_io.hpp"
#include "selection_backend.hpp"
#include "statistics.hpp"
#include "tractogram_store.hpp"
#include "trk_io.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
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
  auto gridCached = tracto::CreateGridSelectionBackend();
  cpu->Build(store);
  grid->Build(store);
  // Build() with a caller-supplied AABB must produce a byte-identical index to
  // the internal-minmax path (guards the cached-RasBounds fast path).
  const tracto::Bounds aabb = tracto::RasBounds(store);
  gridCached->Build(store, &aabb);

  const std::vector<tracto::Bounds> boxes = {
      {{-0.5, 1.5, -0.5, 0.5, -0.5, 0.5}},
      {{-3.0, -1.0, 0.5, 1.5, -0.5, 0.5}},
      {{-100.0, 100.0, -100.0, 100.0, -100.0, 100.0}},  // large box -> parallel-scan fallback
      {{20.0, 21.0, 20.0, 21.0, 20.0, 21.0}},
  };
  for (std::size_t i = 0; i < boxes.size(); ++i) {
    const std::vector<uint8_t> expect = cpu->SelectInBox(store, boxes[i]);
    Check(expect == grid->SelectInBox(store, boxes[i]),
          "CPU and grid selection agree for box " + std::to_string(i));
    Check(expect == gridCached->SelectInBox(store, boxes[i]),
          "cached-bounds grid agrees with CPU for box " + std::to_string(i));
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

// Grayscale histogram, signed-stat histogram, and label LUT — the numeric policy
// that moved out of the Qt shell (main_window) into cpp/core/statistics.
void TestVolumeHistograms() {
  // Grayscale ramp 0..99: every voxel counted, default window == [dispMin,dispMax].
  tracto::Volume ramp;
  ramp.dims[0] = 100; ramp.dims[1] = 1; ramp.dims[2] = 1;
  ramp.data.resize(100);
  for (int i = 0; i < 100; ++i) ramp.data[static_cast<std::size_t>(i)] = static_cast<float>(i);
  ramp.valueMin = 0.0f; ramp.valueMax = 99.0f;

  const tracto::VolumeHistogram gh = tracto::ComputeVolumeHistogram(ramp);
  double binSum = 0.0;
  for (float b : gh.bins) binSum += b;
  Check(Near(binSum, 100.0), "volume histogram bins sum to voxel count");
  Check(Near(gh.dispMin, 0.0) && gh.dispMax > gh.dispMin, "volume disp range ordered from 0");
  Check(Near(gh.winLo, gh.dispMin) && Near(gh.winHi, gh.dispMax),
        "volume default window == [dispMin, dispMax]");

  // Signed stat map (z-score intent): |stat| axis from 0, threshold below cap,
  // histogram counts only the non-zero voxels (background excluded).
  tracto::Volume stat;
  stat.dims[0] = 8; stat.dims[1] = 1; stat.dims[2] = 1;
  stat.data = {0.0f, 0.0f, -1.0f, 2.0f, -4.0f, 5.0f, 8.0f, -9.0f};  // 6 non-zero
  stat.valueMin = -9.0f; stat.valueMax = 8.0f;
  const tracto::VolumeHistogram sh = tracto::ComputeStatHistogram(stat, /*intent=*/5);
  double statSum = 0.0;
  for (float b : sh.bins) statSum += b;
  Check(Near(statSum, 6.0), "stat histogram counts only non-zero voxels");
  Check(Near(sh.dispMin, 0.0), "stat axis starts at 0 (|stat|)");
  Check(sh.winLo < sh.winHi && sh.winLo >= 0.0f, "stat threshold below cap, non-negative");

  // Integer label volume 0..3: width == maxLabel+1, index 0 transparent, others opaque.
  tracto::Volume labels;
  labels.dims[0] = 6; labels.dims[1] = 1; labels.dims[2] = 1;
  labels.data = {0.0f, 1.0f, 2.0f, 3.0f, 0.0f, 1.0f};
  labels.valueMin = 0.0f; labels.valueMax = 3.0f;
  const tracto::LabelLut lut = tracto::ComputeLabelLut(labels);
  Check(lut.width == 4, "label LUT width == maxLabel + 1");
  Check(lut.rgba.size() == 16, "label LUT rgba size == width*4");
  Check(Near(lut.rgba[3], 0.0), "label 0 (background) is transparent");
  Check(Near(lut.rgba[7], 1.0) && Near(lut.rgba[11], 1.0), "non-zero labels are opaque");
}

// "Volume on tract" sampler: nearest-voxel sampling of a scalar volume along the
// alive streamline points, with out-of-bounds and dead-streamline points dropped.
void TestVolumeOnTract() {
  // Identity affine -> world coord == voxel index. Values 10/20/30/40 at x=0..3.
  tracto::Volume vol;
  vol.dims[0] = 4; vol.dims[1] = 1; vol.dims[2] = 1;
  vol.data = {10.0f, 20.0f, 30.0f, 40.0f};
  vol.voxelToWorld = tracto::Identity();
  vol.valueMin = 10.0f; vol.valueMax = 40.0f;

  // 6 points: 0..3 sample voxels 0..3 (sid 0, alive); point 4 is out of bounds;
  // point 5 belongs to a DEAD streamline (sid 1) and must be excluded.
  tracto::TractogramStore store;
  store.streamlines.resize(2);
  store.x = {0.0f, 1.0f, 2.0f, 3.0f, 100.0f, 0.0f};
  store.y = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  store.z = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  store.sid = {0, 0, 0, 0, 0, 1};
  const std::vector<uint8_t> alive = {1, 0};

  const tracto::NumericSummary vs = tracto::SampleVolumeAlongTracts(store, vol, alive);
  Check(vs.valid, "volume-on-tract summary is valid");
  Check(Near(vs.mean, 25.0), "volume-on-tract mean == 25 (10,20,30,40)");
  Check(Near(vs.min, 10.0) && Near(vs.max, 40.0), "volume-on-tract min/max == 10/40");
  Check(Near(vs.median, 25.0), "volume-on-tract median == 25");

  // Empty volume -> invalid (no crash).
  Check(!tracto::SampleVolumeAlongTracts(store, tracto::Volume{}, alive).valid,
        "empty volume -> invalid summary");
}

// Big-endian .trk load: a byte-swapped header must normalize and the point floats
// must swap to native values (interop with BE-written tractograms / nibabel).
void TestBigEndianTrk() {
  std::vector<char> hdr(static_cast<std::size_t>(tracto::kTrkHeaderSize), 0);
  std::memcpy(hdr.data(), "TRACK", 5);
  auto putBE = [&](std::size_t off, const void* src, std::size_t n) {
    std::memcpy(hdr.data() + off, src, n);
    std::reverse(hdr.data() + off, hdr.data() + off + n);
  };
  const int16_t zero16 = 0;
  const int32_t one32 = 1;
  const int32_t hdrSz = tracto::kTrkHeaderSize;
  putBE(36, &zero16, 2);   // n_scalars = 0
  putBE(238, &zero16, 2);  // n_properties = 0
  putBE(988, &one32, 4);   // n_count = 1
  putBE(996, &hdrSz, 4);   // hdr_size = 1000, byte-swapped (marks BE)

  const std::string path =
      (std::filesystem::temp_directory_path() / "tracto_be_selftest.trk").string();
  {
    std::ofstream out(path, std::ios::binary);
    out.write(hdr.data(), static_cast<std::streamsize>(hdr.size()));
    auto writeBE = [&](const void* src, std::size_t n) {
      char b[8];
      std::memcpy(b, src, n);
      std::reverse(b, b + n);
      out.write(b, static_cast<std::streamsize>(n));
    };
    const int32_t pc = 2;
    writeBE(&pc, 4);                              // 2 points, big-endian
    for (float f : {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f}) writeBE(&f, 4);
  }

  tracto::TrkHeader h;
  const std::vector<tracto::Streamline> sl = tracto::LoadTrk(path, h);
  Check(h.nCount == 1, "BE .trk: n_count normalized to 1");
  Check(sl.size() == 1 && sl[0].pointCount == 2, "BE .trk: one 2-point streamline");
  Check(sl.size() == 1 && sl[0].rawPointData.size() == 6 &&
            Near(sl[0].rawPointData[0], 1.0) && Near(sl[0].rawPointData[5], 6.0),
        "BE .trk: point floats byte-swapped to native");
  std::remove(path.c_str());
}

}  // namespace

int main() {
  TestStoreAndStats();
  TestSlimRehydrate();
  TestSelectionBackends();
  TestTrkSubsetRoundTrip();
  TestVolumeHistograms();
  TestVolumeOnTract();
  TestBigEndianTrk();
  if (failures != 0) {
    std::cout << failures << " core self-test failure(s)\n";
    return 1;
  }
  std::cout << "core self-test passed\n";
  return 0;
}
