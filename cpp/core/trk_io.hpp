#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace tracto {

constexpr int kTrkHeaderSize = 1000;

struct TrkHeader {
  std::array<char, kTrkHeaderSize> raw{};
  int16_t nScalars = 0;
  int16_t nProperties = 0;
  int32_t nCount = 0;
  std::array<float, 3> voxelSize{1.0f, 1.0f, 1.0f};
  std::array<float, 3> origin{0.0f, 0.0f, 0.0f};
  std::array<double, 16> voxToRas{};
};

struct Streamline {
  int32_t pointCount = 0;
  std::vector<float> rawPointData;  // x,y,z plus per-point scalars, as stored (kept for save).
  std::vector<float> properties;
};

std::vector<Streamline> LoadTrk(const std::string& path, TrkHeader& header);

bool WriteTrkSubset(const std::string& path,
                    const TrkHeader& header,
                    const std::vector<Streamline>& streamlines,
                    const std::vector<uint8_t>& keep);

}  // namespace tracto
