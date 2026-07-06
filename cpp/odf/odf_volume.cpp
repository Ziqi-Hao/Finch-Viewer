#include "odf_volume.hpp"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>

// 4D SH-coefficient ODF NIfTI-1 reader. Self-contained on purpose: it mirrors
// the byte offsets / datatype dispatch / sform>qform>pixdim precedence of
// cpp/core/nifti_io.cpp but is INDEPENDENT code (does not include it), because
// that loader collapses any 4D file to a single 3D scalar volume — exactly the
// behaviour we must NOT have here. We need the full dim[4] coefficient block.
//
// Dependencies: C++17 stdlib + zlib only. No GL, no Qt.

namespace tracto {
namespace odf {
namespace {

// NIfTI-1 field byte offsets within the 348-byte header (NIfTI-1.1 spec).
constexpr int kHeaderSize = 348;
constexpr int kOffDim = 40;         // short[8]: dim[0]=ndim, dim[1..7]=sizes
constexpr int kOffDatatype = 70;    // short
constexpr int kOffPixdim = 76;      // float[8]: pixdim[0]=qfac, [1..3]=voxel mm
constexpr int kOffVoxOffset = 108;  // float: byte offset to voxel data
constexpr int kOffSclSlope = 112;   // float
constexpr int kOffSclInter = 116;   // float
constexpr int kOffQformCode = 252;  // short
constexpr int kOffSformCode = 254;  // short
constexpr int kOffQuatern = 256;    // float[3]: quaternion b, c, d
constexpr int kOffQoffset = 268;    // float[3]: translation x, y, z
constexpr int kOffSrowX = 280;      // float[4]: sform row 0
constexpr int kOffSrowY = 296;      // float[4]: sform row 1
constexpr int kOffSrowZ = 312;      // float[4]: sform row 2
constexpr int kOffMagic = 344;      // char[4]: "n+1\0" for single-file NIfTI-1

// NIfTI datatype codes (subset we convert to float).
enum : int16_t {
  DT_UINT8 = 2, DT_INT16 = 4, DT_INT32 = 8, DT_FLOAT32 = 16,
  DT_FLOAT64 = 64, DT_INT8 = 256, DT_UINT16 = 512, DT_UINT32 = 768,
};

uint16_t Swap16(uint16_t v) { return static_cast<uint16_t>((v >> 8) | (v << 8)); }
uint32_t Swap32(uint32_t v) {
  return (v >> 24) | ((v >> 8) & 0x0000FF00u) | ((v << 8) & 0x00FF0000u) | (v << 24);
}

// Reads endian-correct primitives out of the fixed 348-byte header buffer.
// `swap` is set when the file's byte order differs from the host's.
struct HeaderReader {
  const char* base;
  bool swap;

  int16_t I16(int off) const {
    uint16_t v;
    std::memcpy(&v, base + off, 2);
    if (swap) v = Swap16(v);
    return static_cast<int16_t>(v);
  }
  float F32(int off) const {
    uint32_t v;
    std::memcpy(&v, base + off, 4);
    if (swap) v = Swap32(v);
    float f;
    std::memcpy(&f, &v, 4);  // bit-cast: avoids the strict-aliasing UB of *(float*)
    return f;
  }
};

// gzread caps each call at ~2 GB and may short-read; loop until `bytes` are in.
// fODF volumes are tiny today (the demo is ~9 KB) but a whole-brain fODF at
// high l_max is hundreds of MB, so the chunked full-read is not speculative.
void GzReadFull(gzFile f, void* dst, std::size_t bytes, const char* what) {
  auto* p = static_cast<char*>(dst);
  std::size_t done = 0;
  while (done < bytes) {
    const unsigned chunk =
        static_cast<unsigned>(std::min<std::size_t>(bytes - done, 1u << 26));  // 64 MB
    const int got = gzread(f, p + done, chunk);
    if (got <= 0) {
      throw std::runtime_error(std::string("truncated ODF NIfTI while reading ") + what);
    }
    done += static_cast<std::size_t>(got);
  }
}

// One stored element of type T -> float, byte-swapped if needed. T is the
// on-disk type; we always emit float so the SH matvec downstream is single
// precision. The doubles in the demo fODF lose mantissa here, which is fine:
// glyph radii are display values, not analysis values.
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
  } else {  // 8-byte double: swap as two 32-bit halves AND reverse their order.
    uint32_t lo, hi;
    std::memcpy(&lo, p, 4);
    std::memcpy(&hi, p + 4, 4);
    if (swap) {
      const uint32_t a = Swap32(lo), b = Swap32(hi);
      lo = b;
      hi = a;
    }
    const uint64_t bits = (static_cast<uint64_t>(hi) << 32) | lo;
    double v;
    std::memcpy(&v, &bits, 8);
    return static_cast<float>(v);
  }
}

// Affine from the quaternion qform. Mirrors nifti1_io's quatern_to_mat44:
// (b,c,d) are the vector part; the scalar a is recovered from unit-norm, and
// the rotation goes into columns scaled by the voxel sizes. pixdim[0] (qfac)
// is the left-handedness flag applied to the k axis.
Affine QformToAffine(const HeaderReader& h, const float pixdim[4]) {
  float b = h.F32(kOffQuatern), c = h.F32(kOffQuatern + 4), d = h.F32(kOffQuatern + 8);
  float a = 1.0f - (b * b + c * c + d * d);
  if (a < 1e-7f) {  // improper / non-unit quaternion -> renormalise (b,c,d), a=0
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
  const float qfac = pixdim[0] == 0.0f ? 1.0f : pixdim[0];  // qfac 0 means +1
  const float sx = pixdim[1], sy = pixdim[2], sz = pixdim[3] * qfac;
  const float t[3] = {h.F32(kOffQoffset), h.F32(kOffQoffset + 4), h.F32(kOffQoffset + 8)};

  Affine m;  // defaults to identity (incl. bottom row 0,0,0,1)
  for (int row = 0; row < 3; ++row) {
    m.at(row, 0) = r[row][0] * sx;
    m.at(row, 1) = r[row][1] * sy;
    m.at(row, 2) = r[row][2] * sz;
    m.at(row, 3) = t[row];
  }
  return m;
}

// World AABB of the 8 voxel-grid corners. We use the LAST sample index per axis
// (dims-1), matching cpp/core/nifti_io.cpp's WorldBounds so this volume's bounds
// line up with the scalar volume / streamlines in the shared RAS space.
AABB ComputeWorldBounds(const Affine& aff, const int dims[3]) {
  const float ex = static_cast<float>(dims[0] - 1);
  const float ey = static_cast<float>(dims[1] - 1);
  const float ez = static_cast<float>(dims[2] - 1);
  AABB box;
  bool init = false;
  for (float x : {0.0f, ex})
    for (float y : {0.0f, ey})
      for (float z : {0.0f, ez}) {
        const Vec3 w = VoxelToWorld(aff, x, y, z);
        if (!init) {
          box.min = box.max = w;
          init = true;
        } else {
          box.min = {std::min(box.min.x, w.x), std::min(box.min.y, w.y),
                     std::min(box.min.z, w.z)};
          box.max = {std::max(box.max.x, w.x), std::max(box.max.y, w.y),
                     std::max(box.max.z, w.z)};
        }
      }
  return box;
}

}  // namespace

OdfVolume LoadOdfNifti(const std::string& path) {
  gzFile f = gzopen(path.c_str(), "rb");  // gzopen transparently reads plain .nii too
  if (!f) {
    throw std::runtime_error("cannot open ODF NIfTI: " + path);
  }

  char hdr[kHeaderSize];
  if (gzread(f, hdr, kHeaderSize) != kHeaderSize) {
    gzclose(f);
    throw std::runtime_error("truncated ODF NIfTI header: " + path);
  }

  // Endianness: sizeof_hdr (first int32) must read as 348 in the file's order.
  // If it only reads as 348 after a byte swap, the file is the other endianness.
  int32_t sizeofHdr;
  std::memcpy(&sizeofHdr, hdr, 4);
  bool swap = false;
  if (sizeofHdr != kHeaderSize) {
    if (static_cast<int32_t>(Swap32(static_cast<uint32_t>(sizeofHdr))) == kHeaderSize) {
      swap = true;
    } else {
      gzclose(f);
      throw std::runtime_error(
          "not a NIfTI-1 file (bad sizeof_hdr; NIfTI-2 unsupported): " + path);
    }
  }
  // "n+1\0" => single-file NIfTI-1. We reject "ni1" (.hdr/.img pairs) because the
  // voxel data then lives in a separate file we are not given.
  if (std::memcmp(hdr + kOffMagic, "n+1", 3) != 0) {
    gzclose(f);
    throw std::runtime_error("unsupported ODF NIfTI (need single-file 'n+1'): " + path);
  }

  const HeaderReader h{hdr, swap};
  const int ndim = h.I16(kOffDim);
  if (ndim < 1 || ndim > 7) {  // sanity: corrupt header or wrong-endian misread
    gzclose(f);
    throw std::runtime_error(
        "ODF NIfTI has invalid dim[0]=" + std::to_string(ndim) + ": " + path);
  }

  const int nx = h.I16(kOffDim + 2);
  const int ny = h.I16(kOffDim + 4);
  const int nz = h.I16(kOffDim + 6);
  // dim[4] is the SH coefficient axis. A genuine 4D file declares ndim>=4; some
  // tools leave dim[4]=1 for a 3D file, so we read dim[4] but require it >= 1.
  const int nt = (ndim >= 4) ? h.I16(kOffDim + 8) : 1;
  if (nx <= 0 || ny <= 0 || nz <= 0) {
    gzclose(f);
    throw std::runtime_error("ODF NIfTI has non-positive spatial dimensions: " + path);
  }
  if (nt <= 0) {
    gzclose(f);
    throw std::runtime_error("ODF NIfTI has non-positive coefficient count dim[4]: " + path);
  }
  // This loader exists to read the FULL coefficient block. A 1-coefficient file
  // is not an ODF (a single SH band-0 value is just a scalar) — but we accept it
  // rather than fail, so a degenerate input still round-trips; downstream
  // InferShOrder will accept nCoeffs==1 (l_max=0). Dimensions 5..7 (NIfTI's
  // u/v/w axes) are not supported for ODF data and we ignore them: if any is >1
  // the data block would be larger than nx*ny*nz*nt and the read would either
  // truncate this block or leave trailing data, so reject that explicitly.
  if (ndim >= 5) {
    for (int axis = 5; axis <= ndim; ++axis) {
      if (h.I16(kOffDim + 2 * axis) > 1) {
        gzclose(f);
        throw std::runtime_error(
            "ODF NIfTI has unsupported >4D extent on axis " + std::to_string(axis) + ": " + path);
      }
    }
  }

  const int16_t datatype = h.I16(kOffDatatype);
  const float pixdim[4] = {h.F32(kOffPixdim), h.F32(kOffPixdim + 4), h.F32(kOffPixdim + 8),
                           h.F32(kOffPixdim + 12)};
  const float voxOffset = h.F32(kOffVoxOffset);
  float sclSlope = h.F32(kOffSclSlope);
  const float sclInter = h.F32(kOffSclInter);
  if (sclSlope == 0.0f) sclSlope = 1.0f;  // NIfTI: slope 0 means "no scaling"

  int elemBytes;
  switch (datatype) {
    case DT_UINT8: case DT_INT8: elemBytes = 1; break;
    case DT_INT16: case DT_UINT16: elemBytes = 2; break;
    case DT_INT32: case DT_UINT32: case DT_FLOAT32: elemBytes = 4; break;
    case DT_FLOAT64: elemBytes = 8; break;
    default:
      gzclose(f);
      throw std::runtime_error(
          "unsupported ODF NIfTI datatype code " + std::to_string(datatype) + ": " + path);
  }

  const std::size_t spatial =
      static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny) * static_cast<std::size_t>(nz);
  const std::size_t nElem = spatial * static_cast<std::size_t>(nt);

  // Jump to the voxel data. vox_offset is past the 348-byte header (+ optional
  // extensions); guard against NaN/negative/into-header values.
  if (!(voxOffset >= static_cast<float>(kHeaderSize))) {  // NaN-safe via negated >=
    gzclose(f);
    throw std::runtime_error("ODF NIfTI vox_offset seeks into the header: " + path);
  }
  if (gzseek(f, static_cast<z_off_t>(voxOffset), SEEK_SET) < 0) {
    gzclose(f);
    throw std::runtime_error("cannot seek to ODF NIfTI voxel data: " + path);
  }

  OdfVolume out;
  out.dims[0] = nx;
  out.dims[1] = ny;
  out.dims[2] = nz;
  out.nCoeffs = nt;
  out.coeffs.resize(nElem);

  // Decode the raw voxel bytes into out.coeffs, then free `raw` at the block's
  // end — before the value-range scan below, which only touches out.coeffs. For a
  // float64 source this drops the transient footprint from 12 to 4 bytes/element.
  //
  // The on-disk layout for a 4D NIfTI is coefficient-OUTERMOST already:
  //   flat = c*(nx*ny*nz) + k*(nx*ny) + j*nx + i
  // which is exactly OdfVolume::coeffIndex. So the linear element order on disk
  // equals our linear buffer order: a single straight scan converts in place,
  // no per-voxel gather needed. (Verified against nii_volume.h:95-118.)
  {
    std::vector<char> raw(nElem * static_cast<std::size_t>(elemBytes));
    GzReadFull(f, raw.data(), raw.size(), "voxel data");
    gzclose(f);

    auto convert = [&](auto tag) {
      using T = decltype(tag);
      const char* p = raw.data();
      for (std::size_t i = 0; i < nElem; ++i, p += sizeof(T)) {
        out.coeffs[i] = ReadElem<T>(p, swap) * sclSlope + sclInter;
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

  // Affine: prefer sform (sform_code>0), then qform (qform_code>0), else the
  // diagonal pixdim scaling. Same precedence as cpp/core/nifti_io.cpp so this
  // volume sits in the identical RAS-mm space as the scalar volume and streamlines.
  if (h.I16(kOffSformCode) > 0) {
    for (int col = 0; col < 4; ++col) {
      out.affine.at(0, col) = h.F32(kOffSrowX + col * 4);
      out.affine.at(1, col) = h.F32(kOffSrowY + col * 4);
      out.affine.at(2, col) = h.F32(kOffSrowZ + col * 4);
    }
  } else if (h.I16(kOffQformCode) > 0) {
    out.affine = QformToAffine(h, pixdim);
  } else {
    out.affine.at(0, 0) = pixdim[1];
    out.affine.at(1, 1) = pixdim[2];
    out.affine.at(2, 2) = pixdim[3];
  }

  out.worldBounds = ComputeWorldBounds(out.affine, out.dims);

  // Per-volume value range over every coefficient of every voxel. NaN-aware:
  // a NaN fails both comparisons, so it is silently skipped rather than poisoning
  // the range — fODF files occasionally carry NaN in background voxels.
  float lo = std::numeric_limits<float>::max();
  float hi = std::numeric_limits<float>::lowest();
  bool any = false;
  for (float v : out.coeffs) {
    if (v < lo) { lo = v; any = true; }
    if (v > hi) { hi = v; any = true; }
  }
  if (!any) {  // all-NaN or empty: fall back to a neutral [0,1] range
    lo = 0.0f;
    hi = 1.0f;
  }
  out.valueMin = lo;
  out.valueMax = (hi > lo) ? hi : lo + 1.0f;  // keep max>min so consumers can divide

  return out;
}

}  // namespace odf
}  // namespace tracto
