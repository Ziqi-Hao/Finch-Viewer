#include "trk_io.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace tracto {
namespace {

template <typename T>
T ReadLE(const char* ptr) {
  T value{};
  std::memcpy(&value, ptr, sizeof(T));
  return value;
}

template <typename T>
void WriteLE(char* ptr, T value) {
  std::memcpy(ptr, &value, sizeof(T));
}

bool LooksLikeZeroMatrix(const std::array<double, 16>& m) {
  double sum = 0.0;
  for (double x : m) {
    sum += std::abs(x);
  }
  return sum < 1e-12;
}

void MakeFallbackVoxToRas(TrkHeader& header) {
  header.voxToRas.fill(0.0);
  header.voxToRas[0] = header.voxelSize[0];
  header.voxToRas[5] = header.voxelSize[1];
  header.voxToRas[10] = header.voxelSize[2];
  header.voxToRas[15] = 1.0;
  header.voxToRas[3] = header.origin[0];
  header.voxToRas[7] = header.origin[1];
  header.voxToRas[11] = header.origin[2];
}

std::array<float, 3> TransformPoint(const std::array<double, 16>& m,
                                    float x, float y, float z) {
  return {
      static_cast<float>(m[0] * x + m[1] * y + m[2] * z + m[3]),
      static_cast<float>(m[4] * x + m[5] * y + m[6] * z + m[7]),
      static_cast<float>(m[8] * x + m[9] * y + m[10] * z + m[11]),
  };
}

TrkHeader ParseTrkHeader(const std::array<char, kTrkHeaderSize>& raw) {
  TrkHeader header;
  header.raw = raw;

  if (std::memcmp(raw.data(), "TRACK", 5) != 0) {
    throw std::runtime_error("input is not a TrackVis .trk file");
  }

  const int32_t hdrSize = ReadLE<int32_t>(raw.data() + 996);
  if (hdrSize != kTrkHeaderSize) {
    throw std::runtime_error("unsupported .trk endian/header size");
  }

  header.voxelSize[0] = ReadLE<float>(raw.data() + 12);
  header.voxelSize[1] = ReadLE<float>(raw.data() + 16);
  header.voxelSize[2] = ReadLE<float>(raw.data() + 20);
  header.origin[0] = ReadLE<float>(raw.data() + 24);
  header.origin[1] = ReadLE<float>(raw.data() + 28);
  header.origin[2] = ReadLE<float>(raw.data() + 32);
  header.nScalars = ReadLE<int16_t>(raw.data() + 36);
  header.nProperties = ReadLE<int16_t>(raw.data() + 238);
  header.nCount = ReadLE<int32_t>(raw.data() + 988);

  for (int i = 0; i < 16; ++i) {
    header.voxToRas[i] = ReadLE<float>(raw.data() + 440 + i * 4);
  }
  if (LooksLikeZeroMatrix(header.voxToRas)) {
    MakeFallbackVoxToRas(header);
  }

  if (header.nScalars < 0 || header.nProperties < 0) {
    throw std::runtime_error("invalid .trk scalar/property counts");
  }

  return header;
}

}  // namespace

std::vector<Streamline> LoadTrk(const std::string& path, TrkHeader& header) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("cannot open .trk input: " + path);
  }

  std::array<char, kTrkHeaderSize> rawHeader{};
  in.read(rawHeader.data(), rawHeader.size());
  if (in.gcount() != kTrkHeaderSize) {
    throw std::runtime_error("truncated .trk header");
  }
  header = ParseTrkHeader(rawHeader);

  const int pointComponents = 3 + header.nScalars;
  std::vector<Streamline> streamlines;
  if (header.nCount > 0) {
    streamlines.reserve(static_cast<std::size_t>(header.nCount));
  }

  for (int32_t index = 0; header.nCount <= 0 || index < header.nCount; ++index) {
    int32_t pointCount = 0;
    in.read(reinterpret_cast<char*>(&pointCount), sizeof(pointCount));
    if (!in) {
      if (header.nCount <= 0 && in.eof()) {
        break;
      }
      throw std::runtime_error("truncated .trk streamline count");
    }
    if (pointCount < 0 || pointCount > 10000000) {
      throw std::runtime_error("invalid .trk streamline point count");
    }

    Streamline sl;
    sl.pointCount = pointCount;
    sl.rawPointData.resize(static_cast<std::size_t>(pointCount) * pointComponents);
    in.read(reinterpret_cast<char*>(sl.rawPointData.data()),
            static_cast<std::streamsize>(sl.rawPointData.size() * sizeof(float)));
    if (!in) {
      throw std::runtime_error("truncated .trk point data");
    }

    sl.properties.resize(static_cast<std::size_t>(header.nProperties));
    if (!sl.properties.empty()) {
      in.read(reinterpret_cast<char*>(sl.properties.data()),
              static_cast<std::streamsize>(sl.properties.size() * sizeof(float)));
      if (!in) {
        throw std::runtime_error("truncated .trk property data");
      }
    }

    sl.rasPoints.resize(static_cast<std::size_t>(pointCount) * 3);
    for (int32_t p = 0; p < pointCount; ++p) {
      const std::size_t src = static_cast<std::size_t>(p) * pointComponents;
      const auto ras = TransformPoint(header.voxToRas,
                                      sl.rawPointData[src],
                                      sl.rawPointData[src + 1],
                                      sl.rawPointData[src + 2]);
      const std::size_t dst = static_cast<std::size_t>(p) * 3;
      sl.rasPoints[dst] = ras[0];
      sl.rasPoints[dst + 1] = ras[1];
      sl.rasPoints[dst + 2] = ras[2];
    }

    streamlines.push_back(std::move(sl));
  }

  return streamlines;
}

bool WriteTrkSubset(const std::string& path,
                    const TrkHeader& header,
                    const std::vector<Streamline>& streamlines,
                    const std::vector<uint8_t>& keep) {
  if (streamlines.size() != keep.size()) {
    throw std::runtime_error("internal error: keep mask size mismatch");
  }

  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("cannot open output .trk: " + path);
  }

  std::array<char, kTrkHeaderSize> outHeader = header.raw;
  const int32_t outCount = static_cast<int32_t>(
      std::count(keep.begin(), keep.end(), static_cast<uint8_t>(1)));
  WriteLE<int32_t>(outHeader.data() + 988, outCount);
  WriteLE<int32_t>(outHeader.data() + 996, kTrkHeaderSize);
  out.write(outHeader.data(), outHeader.size());

  const int pointComponents = 3 + header.nScalars;
  for (std::size_t i = 0; i < streamlines.size(); ++i) {
    if (!keep[i]) {
      continue;
    }
    const Streamline& sl = streamlines[i];
    out.write(reinterpret_cast<const char*>(&sl.pointCount), sizeof(sl.pointCount));
    out.write(reinterpret_cast<const char*>(sl.rawPointData.data()),
              static_cast<std::streamsize>(
                  static_cast<std::size_t>(sl.pointCount) * pointComponents * sizeof(float)));
    if (!sl.properties.empty()) {
      out.write(reinterpret_cast<const char*>(sl.properties.data()),
                static_cast<std::streamsize>(sl.properties.size() * sizeof(float)));
    }
  }

  return static_cast<bool>(out);
}

}  // namespace tracto
