#pragma once

// Minimal row-major linear algebra shared across the toolbox: the 3-vector, the
// 4x4 matrix, and the general affine helpers core needs (voxel<->world affines
// for NIfTI, track density, and volume sampling). UI-free — no Qt, no GL. The
// camera/projection math that only the renderer needs lives in cpp/qt/camera_math.hpp
// so cpp/core stays free of viewport concerns. Matrices are row-major: row r,
// column c is m[r*4 + c].

#include <algorithm>
#include <cmath>

namespace tracto {

struct Vec3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 v, float s) { return {v.x * s, v.y * s, v.z * s}; }

inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

inline Vec3 Cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline Vec3 Normalize(Vec3 v) {
  const float n = std::sqrt(std::max(1e-12f, Dot(v, v)));
  return {v.x / n, v.y / n, v.z / n};
}

struct Mat4 {
  float m[16] = {};
};

inline Mat4 Identity() {
  Mat4 out;
  out.m[0] = out.m[5] = out.m[10] = out.m[15] = 1.0f;
  return out;
}

inline Mat4 Multiply(const Mat4& a, const Mat4& b) {
  Mat4 out;
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      float sum = 0.0f;
      for (int k = 0; k < 4; ++k) {
        sum += a.m[r * 4 + k] * b.m[k * 4 + c];
      }
      out.m[r * 4 + c] = sum;
    }
  }
  return out;
}

// Inverse of a row-major affine (last row [0 0 0 1]) — invert the 3x3 then the
// translation. Used for world->voxel (e.g. a focus point -> slice index, or
// sampling a scalar volume along streamlines). Identity on a singular matrix.
inline Mat4 InverseAffine(const Mat4& m) {
  const float a = m.m[0], b = m.m[1], c = m.m[2];
  const float d = m.m[4], e = m.m[5], f = m.m[6];
  const float g = m.m[8], h = m.m[9], i = m.m[10];
  const float A = e * i - f * h, B = f * g - d * i, C = d * h - e * g;
  const float det = a * A + b * B + c * C;
  if (std::abs(det) < 1e-12f) return Identity();
  const float id = 1.0f / det;
  const float r0 = A * id, r1 = (c * h - b * i) * id, r2 = (b * f - c * e) * id;
  const float r3 = B * id, r4 = (a * i - c * g) * id, r5 = (c * d - a * f) * id;
  const float r6 = C * id, r7 = (b * g - a * h) * id, r8 = (a * e - b * d) * id;
  const float tx = m.m[3], ty = m.m[7], tz = m.m[11];
  Mat4 out;
  out.m[0] = r0; out.m[1] = r1; out.m[2] = r2;  out.m[3] = -(r0 * tx + r1 * ty + r2 * tz);
  out.m[4] = r3; out.m[5] = r4; out.m[6] = r5;  out.m[7] = -(r3 * tx + r4 * ty + r5 * tz);
  out.m[8] = r6; out.m[9] = r7; out.m[10] = r8; out.m[11] = -(r6 * tx + r7 * ty + r8 * tz);
  out.m[15] = 1.0f;
  return out;
}

}  // namespace tracto
