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

template <typename T>
T ByteSwap(T value) {
  char* p = reinterpret_cast<char*>(&value);
  std::reverse(p, p + sizeof(T));
  return value;
}

// Reverse the bytes of `count` little/big-endian elements of `elemSize` bytes at
// `offset` within the raw header — in place.
void SwapHeaderField(std::array<char, kTrkHeaderSize>& raw, std::size_t offset,
                     std::size_t elemSize, std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) {
    char* p = raw.data() + offset + i * elemSize;
    std::reverse(p, p + elemSize);
  }
}

// Convert a big-endian TrackVis header to little-endian in place: byte-swap every
// numeric field the format defines (char/text fields are byte-order-agnostic).
// After this the header reads correctly via ReadLE AND WriteTrkSubset re-emits a
// valid little-endian file — swapping only the fields we read would otherwise
// leave a corrupt mixed-endian header on save.
void NormalizeTrkHeaderToLE(std::array<char, kTrkHeaderSize>& raw) {
  SwapHeaderField(raw, 6, 2, 3);     // dim[3]                      int16
  SwapHeaderField(raw, 12, 4, 3);    // voxel_size[3]              float32
  SwapHeaderField(raw, 24, 4, 3);    // origin[3]                  float32
  SwapHeaderField(raw, 36, 2, 1);    // n_scalars                   int16
  SwapHeaderField(raw, 238, 2, 1);   // n_properties                int16
  SwapHeaderField(raw, 440, 4, 16);  // vox_to_ras[4][4]           float32
  SwapHeaderField(raw, 956, 4, 6);   // image_orientation_patient  float32
  SwapHeaderField(raw, 988, 4, 1);   // n_count                     int32
  SwapHeaderField(raw, 992, 4, 1);   // version                     int32
  SwapHeaderField(raw, 996, 4, 1);   // hdr_size                    int32
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

// Parses the 1000-byte header. `bigEndian` is set true when the source was a
// big-endian .trk (its numeric fields are normalized to LE in header.raw here);
// LoadTrk then byte-swaps the point/property floats to match.
TrkHeader ParseTrkHeader(std::array<char, kTrkHeaderSize> raw, bool& bigEndian) {
  TrkHeader header;
  bigEndian = false;

  if (std::memcmp(raw.data(), "TRACK", 5) != 0) {
    throw std::runtime_error("input is not a TrackVis .trk file");
  }

  int32_t hdrSize = ReadLE<int32_t>(raw.data() + 996);
  if (hdrSize != kTrkHeaderSize) {
    // A big-endian .trk stores hdr_size = 1000 byte-swapped. Normalize the whole
    // header to little-endian so the reads below and any later save stay valid.
    if (ByteSwap<int32_t>(hdrSize) == kTrkHeaderSize) {
      bigEndian = true;
      NormalizeTrkHeaderToLE(raw);
    } else {
      throw std::runtime_error("unsupported .trk endian/header size");
    }
  }
  header.raw = raw;

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
  bool bigEndian = false;
  header = ParseTrkHeader(rawHeader, bigEndian);

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
    if (bigEndian) pointCount = ByteSwap(pointCount);
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
    if (bigEndian)
      for (float& f : sl.rawPointData) f = ByteSwap(f);

    sl.properties.resize(static_cast<std::size_t>(header.nProperties));
    if (!sl.properties.empty()) {
      in.read(reinterpret_cast<char*>(sl.properties.data()),
              static_cast<std::streamsize>(sl.properties.size() * sizeof(float)));
      if (!in) {
        throw std::runtime_error("truncated .trk property data");
      }
      if (bigEndian)
        for (float& f : sl.properties) f = ByteSwap(f);
    }

    // RAS points are derived on demand in BuildSoA (transform applied straight
    // into the SoA), so we keep only rawPointData here.
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
