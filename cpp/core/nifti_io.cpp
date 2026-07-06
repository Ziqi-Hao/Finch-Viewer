#include "nifti_io.hpp"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace tracto {
namespace {

// NIfTI-1 field byte offsets within the 348-byte header.
constexpr int kHeaderSize = 348;
constexpr int kOffDim = 40;        // short[8]
constexpr int kOffIntentCode = 68; // short (NIFTI_INTENT_*)
constexpr int kOffDatatype = 70;   // short
constexpr int kOffBitpix = 72;     // short
constexpr int kOffPixdim = 76;     // float[8]
constexpr int kOffVoxOffset = 108; // float
constexpr int kOffSclSlope = 112;  // float
constexpr int kOffSclInter = 116;  // float
constexpr int kOffQformCode = 252; // short
constexpr int kOffSformCode = 254; // short
constexpr int kOffQuatern = 256;   // float[3] b,c,d
constexpr int kOffQoffset = 268;   // float[3] x,y,z
constexpr int kOffSrowX = 280;     // float[4]
constexpr int kOffSrowY = 296;     // float[4]
constexpr int kOffSrowZ = 312;     // float[4]
constexpr int kOffMagic = 344;     // char[4]

// NIfTI datatype codes.
enum : int16_t {
  DT_UINT8 = 2, DT_INT16 = 4, DT_INT32 = 8, DT_FLOAT32 = 16,
  DT_FLOAT64 = 64, DT_INT8 = 256, DT_UINT16 = 512, DT_UINT32 = 768,
};

uint16_t Swap16(uint16_t v) { return static_cast<uint16_t>((v >> 8) | (v << 8)); }
uint32_t Swap32(uint32_t v) {
  return (v >> 24) | ((v >> 8) & 0x0000FF00u) | ((v << 8) & 0x00FF0000u) | (v << 24);
}

// Reads little/big-endian primitives out of the fixed header buffer.
struct HeaderReader {
  const char* base;
  bool swap;

  int16_t I16(int off) const {
    uint16_t v;
    std::memcpy(&v, base + off, 2);
    if (swap) v = Swap16(v);
    return static_cast<int16_t>(v);
  }
  int32_t I32(int off) const {
    uint32_t v;
    std::memcpy(&v, base + off, 4);
    if (swap) v = Swap32(v);
    return static_cast<int32_t>(v);
  }
  float F32(int off) const {
    uint32_t v;
    std::memcpy(&v, base + off, 4);
    if (swap) v = Swap32(v);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
  }
};

void GzReadFull(gzFile f, void* dst, std::size_t bytes, const char* what) {
  auto* p = static_cast<char*>(dst);
  std::size_t done = 0;
  while (done < bytes) {
    const unsigned chunk =
        static_cast<unsigned>(std::min<std::size_t>(bytes - done, 1u << 26));  // 64 MB
    const int got = gzread(f, p + done, chunk);
    if (got <= 0) {
      throw std::runtime_error(std::string("truncated NIfTI while reading ") + what);
    }
    done += static_cast<std::size_t>(got);
  }
}

// One element of `code` -> float, byte-swapped if needed.
template <typename T>
float ReadElem(const char* p, bool swap) {
  if constexpr (sizeof(T) == 1) {
    T v;
    std::memcpy(&v, p, 1);
    return static_cast<float>(v);
  } else if constexpr (sizeof(T) == 2) {
    uint16_t u;
    std::memcpy(&u, p, 2);
    if (swap) u = Swap16(u);
    T v;
    std::memcpy(&v, &u, 2);
    return static_cast<float>(v);
  } else if constexpr (sizeof(T) == 4) {
    uint32_t u;
    std::memcpy(&u, p, 4);
    if (swap) u = Swap32(u);
    T v;
    std::memcpy(&v, &u, 4);
    return static_cast<float>(v);
  } else {  // 8-byte double
    uint32_t lo, hi;
    std::memcpy(&lo, p, 4);
    std::memcpy(&hi, p + 4, 4);
    if (swap) {
      const uint32_t a = Swap32(lo), b = Swap32(hi);
      lo = b;
      hi = a;
    }
    uint64_t bits = (static_cast<uint64_t>(hi) << 32) | lo;
    double v;
    std::memcpy(&v, &bits, 8);
    return static_cast<float>(v);
  }
}

// Affine from the quaternion (qform). Mirrors nifti1_io's quatern_to_mat44.
Mat4 QformToAffine(const HeaderReader& h, const float pixdim[4]) {
  float b = h.F32(kOffQuatern), c = h.F32(kOffQuatern + 4), d = h.F32(kOffQuatern + 8);
  float a = 1.0f - (b * b + c * c + d * d);
  if (a < 1e-7f) {  // improper quaternion -> normalise b,c,d
    const float n = 1.0f / std::sqrt(b * b + c * c + d * d);
    b *= n; c *= n; d *= n;
    a = 0.0f;
  } else {
    a = std::sqrt(a);
  }
  const float r[3][3] = {
      {a * a + b * b - c * c - d * d, 2 * (b * c - a * d), 2 * (b * d + a * c)},
      {2 * (b * c + a * d), a * a + c * c - b * b - d * d, 2 * (c * d - a * b)},
      {2 * (b * d - a * c), 2 * (c * d + a * b), a * a + d * d - b * b - c * c}};
  const float qfac = pixdim[0] == 0.0f ? 1.0f : pixdim[0];
  const float sx = pixdim[1], sy = pixdim[2], sz = pixdim[3] * qfac;
  const float t[3] = {h.F32(kOffQoffset), h.F32(kOffQoffset + 4), h.F32(kOffQoffset + 8)};

  Mat4 m = Identity();
  for (int row = 0; row < 3; ++row) {
    m.m[row * 4 + 0] = r[row][0] * sx;
    m.m[row * 4 + 1] = r[row][1] * sy;
    m.m[row * 4 + 2] = r[row][2] * sz;
    m.m[row * 4 + 3] = t[row];
  }
  return m;
}

}  // namespace

Volume LoadNifti(const std::string& path) {
  gzFile f = gzopen(path.c_str(), "rb");  // gzopen also reads plain .nii
  if (!f) {
    throw std::runtime_error("cannot open NIfTI: " + path);
  }

  char hdr[kHeaderSize];
  if (gzread(f, hdr, kHeaderSize) != kHeaderSize) {
    gzclose(f);
    throw std::runtime_error("truncated NIfTI header: " + path);
  }

  // Endianness: sizeof_hdr must read as 348 in the file's byte order.
  int32_t sizeofHdr;
  std::memcpy(&sizeofHdr, hdr, 4);
  bool swap = false;
  if (sizeofHdr != kHeaderSize) {
    if (static_cast<int32_t>(Swap32(static_cast<uint32_t>(sizeofHdr))) == kHeaderSize) {
      swap = true;
    } else {
      gzclose(f);
      throw std::runtime_error("not a NIfTI-1 file (bad sizeof_hdr; NIfTI-2 unsupported): " + path);
    }
  }
  if (std::memcmp(hdr + kOffMagic, "n+1", 3) != 0) {
    gzclose(f);
    throw std::runtime_error("unsupported NIfTI (need single-file 'n+1'): " + path);
  }

  const HeaderReader h{hdr, swap};
  const int ndim = h.I16(kOffDim);
  if (ndim < 1 || ndim > 7) {  // sanity: catches corrupt headers / wrong endianness
    gzclose(f);
    throw std::runtime_error("NIfTI has invalid dim[0]=" + std::to_string(ndim) + ": " + path);
  }
  const int nx = h.I16(kOffDim + 2);
  const int ny = h.I16(kOffDim + 4);
  const int nz = h.I16(kOffDim + 6);
  if (nx <= 0 || ny <= 0 || nz <= 0) {
    gzclose(f);
    throw std::runtime_error("NIfTI has non-positive dimensions: " + path);
  }
  // For 4D+ files (ndim>3) we load only the first volume (nx*ny*nz) — the
  // intended behaviour for an scalar background.
  const int16_t datatype = h.I16(kOffDatatype);
  const float pixdim[4] = {h.F32(kOffPixdim), h.F32(kOffPixdim + 4), h.F32(kOffPixdim + 8),
                           h.F32(kOffPixdim + 12)};
  const float voxOffset = h.F32(kOffVoxOffset);
  float sclSlope = h.F32(kOffSclSlope);
  const float sclInter = h.F32(kOffSclInter);
  if (sclSlope == 0.0f) sclSlope = 1.0f;  // 0 means "no scaling"

  const std::size_t nvox =
      static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny) * static_cast<std::size_t>(nz);

  int elemBytes;
  switch (datatype) {
    case DT_UINT8: case DT_INT8: elemBytes = 1; break;
    case DT_INT16: case DT_UINT16: elemBytes = 2; break;
    case DT_INT32: case DT_UINT32: case DT_FLOAT32: elemBytes = 4; break;
    case DT_FLOAT64: elemBytes = 8; break;
    default:
      gzclose(f);
      throw std::runtime_error("unsupported NIfTI datatype code " + std::to_string(datatype));
  }

  // Jump to the voxel data (vox_offset is past the 348-byte header + extensions).
  if (!(voxOffset >= static_cast<float>(kHeaderSize))) {  // also catches NaN / negative
    gzclose(f);
    throw std::runtime_error("NIfTI vox_offset seeks into the header: " + path);
  }
  if (gzseek(f, static_cast<z_off_t>(voxOffset), SEEK_SET) < 0) {
    gzclose(f);
    throw std::runtime_error("cannot seek to NIfTI voxel data: " + path);
  }
  Volume vol;
  vol.dims[0] = nx; vol.dims[1] = ny; vol.dims[2] = nz;
  vol.data.resize(nvox);

  // Decode the raw voxel bytes into vol.data, then free `raw` at the block's end
  // — before the sanitize/min-max pass below, which only touches vol.data. For a
  // float64 source this drops the transient footprint from 12 to 4 bytes/voxel
  // (only the float result survives), avoiding OOM headroom on large volumes.
  {
    std::vector<char> raw(nvox * static_cast<std::size_t>(elemBytes));
    GzReadFull(f, raw.data(), raw.size(), "voxel data");
    gzclose(f);

    auto convert = [&](auto tag) {
      using T = decltype(tag);
      const char* p = raw.data();
      for (std::size_t i = 0; i < nvox; ++i, p += sizeof(T)) {
        vol.data[i] = ReadElem<T>(p, swap) * sclSlope + sclInter;
      }
    };
    switch (datatype) {
      case DT_UINT8: convert(uint8_t{}); break;
      case DT_INT8: convert(int8_t{}); break;
      case DT_INT16: convert(int16_t{}); break;
      case DT_UINT16: convert(uint16_t{}); break;
      case DT_INT32: convert(int32_t{}); break;
      case DT_UINT32: convert(uint32_t{}); break;
      case DT_FLOAT32: convert(float{}); break;
      case DT_FLOAT64: convert(double{}); break;
    }
  }

  // Affine: prefer sform, then qform, else diagonal pixdim scaling.
  if (h.I16(kOffSformCode) > 0) {
    vol.voxelToWorld = Identity();
    for (int col = 0; col < 4; ++col) {
      vol.voxelToWorld.m[0 * 4 + col] = h.F32(kOffSrowX + col * 4);
      vol.voxelToWorld.m[1 * 4 + col] = h.F32(kOffSrowY + col * 4);
      vol.voxelToWorld.m[2 * 4 + col] = h.F32(kOffSrowZ + col * 4);
    }
  } else if (h.I16(kOffQformCode) > 0) {
    vol.voxelToWorld = QformToAffine(h, pixdim);
  } else {
    vol.voxelToWorld = Identity();
    vol.voxelToWorld.m[0] = pixdim[1];
    vol.voxelToWorld.m[5] = pixdim[2];
    vol.voxelToWorld.m[10] = pixdim[3];
  }

  // Sanitize non-finite voxels to 0 in the same pass that scans min/max. Stat maps
  // (z/t/r) routinely store NaN in masked-out voxels (FSL/SPM/AFNI), and Inf can
  // appear in ratio maps; this is the single chokepoint, so downstream (the R32F
  // texture + slice shader, the contrast histogram, the signed-vs-anatomy detection)
  // never has to special-case NaN — which a GPU would otherwise paint as garbage and
  // a static_cast<int> would turn into UB. 0 reads as background everywhere.
  float lo = std::numeric_limits<float>::max();
  float hi = std::numeric_limits<float>::lowest();
  for (float& v : vol.data) {
    if (!std::isfinite(v)) v = 0.0f;
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }
  vol.valueMin = lo;
  vol.valueMax = (hi > lo) ? hi : lo + 1.0f;
  return vol;
}

Bounds WorldBounds(const Volume& v) {
  Bounds b;
  if (v.dims[0] <= 0 || v.dims[1] <= 0 || v.dims[2] <= 0) {
    b.v[0] = b.v[2] = b.v[4] = -1.0;
    b.v[1] = b.v[3] = b.v[5] = 1.0;
    return b;
  }
  const float* m = v.voxelToWorld.m;
  const float ex = static_cast<float>(v.dims[0] - 1);
  const float ey = static_cast<float>(v.dims[1] - 1);
  const float ez = static_cast<float>(v.dims[2] - 1);
  bool init = false;
  for (float x : {0.0f, ex})
    for (float y : {0.0f, ey})
      for (float z : {0.0f, ez}) {
        const double wx = m[0] * x + m[1] * y + m[2] * z + m[3];
        const double wy = m[4] * x + m[5] * y + m[6] * z + m[7];
        const double wz = m[8] * x + m[9] * y + m[10] * z + m[11];
        if (!init) {
          b.v[0] = b.v[1] = wx; b.v[2] = b.v[3] = wy; b.v[4] = b.v[5] = wz;
          init = true;
        } else {
          b.v[0] = std::min(b.v[0], wx); b.v[1] = std::max(b.v[1], wx);
          b.v[2] = std::min(b.v[2], wy); b.v[3] = std::max(b.v[3], wy);
          b.v[4] = std::min(b.v[4], wz); b.v[5] = std::max(b.v[5], wz);
        }
      }
  return b;
}

NiftiInfo PeekNifti(const std::string& path) {
  // Header-only and non-throwing (the router calls it speculatively to classify a
  // file): any failure just returns ok=false and the caller falls back. Reuses the
  // same offsets + endian detection as LoadNifti so the two never disagree.
  NiftiInfo info;
  gzFile f = gzopen(path.c_str(), "rb");  // transparently reads plain .nii too
  if (!f) return info;
  char hdr[kHeaderSize];
  const bool full = gzread(f, hdr, kHeaderSize) == kHeaderSize;
  gzclose(f);
  if (!full) return info;

  int32_t sizeofHdr;
  std::memcpy(&sizeofHdr, hdr, 4);
  bool swap = false;
  if (sizeofHdr != kHeaderSize) {
    if (static_cast<int32_t>(Swap32(static_cast<uint32_t>(sizeofHdr))) == kHeaderSize)
      swap = true;
    else
      return info;  // not a NIfTI-1 (NIfTI-2 / .hdr-img / garbage)
  }
  if (std::memcmp(hdr + kOffMagic, "n+1", 3) != 0) return info;

  const HeaderReader h{hdr, swap};
  const int ndim = h.I16(kOffDim);
  if (ndim < 1 || ndim > 7) return info;
  info.ndim = ndim;
  for (int d = 0; d <= 7; ++d) info.dim[d] = h.I16(kOffDim + 2 * d);
  info.dim[0] = ndim;  // dim[0] holds the dimensionality, not a size
  info.datatype = h.I16(kOffDatatype);
  info.intentCode = h.I16(kOffIntentCode);  // statistic hint (z/t/correl/…) for the Open router
  info.ok = true;
  return info;
}

}  // namespace tracto
