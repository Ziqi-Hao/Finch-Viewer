#pragma once

// Minimal row-major linear algebra plus an orbit camera, shared by the OpenGL
// renderers. Header-only and UI-free on purpose: pure math, no VTK and no Qt,
// so any backend (Qt viewport, GLFW viewer) can draw the same picture with the
// same camera feel. Matrices are row-major; upload with transpose = GL_TRUE.

#include "bounds.hpp"

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

inline Mat4 Perspective(float fovyRadians, float aspect, float zNear, float zFar) {
  const float f = 1.0f / std::tan(0.5f * fovyRadians);
  Mat4 out;
  out.m[0] = f / aspect;
  out.m[5] = f;
  out.m[10] = (zFar + zNear) / (zNear - zFar);
  out.m[11] = (2.0f * zFar * zNear) / (zNear - zFar);
  out.m[14] = -1.0f;
  return out;
}

inline Mat4 LookAt(Vec3 eye, Vec3 center, Vec3 up) {
  const Vec3 f = Normalize(center - eye);
  const Vec3 s = Normalize(Cross(f, up));
  const Vec3 u = Cross(s, f);

  Mat4 out;
  out.m[0] = s.x;  out.m[1] = s.y;  out.m[2] = s.z;  out.m[3] = -Dot(s, eye);
  out.m[4] = u.x;  out.m[5] = u.y;  out.m[6] = u.z;  out.m[7] = -Dot(u, eye);
  out.m[8] = -f.x; out.m[9] = -f.y; out.m[10] = -f.z; out.m[11] = Dot(f, eye);
  out.m[15] = 1.0f;
  return out;
}

constexpr float kPi = 3.14159265358979323846f;
constexpr float kFovYRadians = 50.0f * kPi / 180.0f;  // shared by ViewProj + ZoomToCursor

// Wrap an angle into (-pi, pi] so yaw/pitch stay bounded over a long session.
inline float WrapAngle(float a) {
  a = std::fmod(a + kPi, 2.0f * kPi);
  if (a < 0.0f) a += 2.0f * kPi;
  return a - kPi;
}

// Turntable camera orbiting a target point. The angle/zoom/pan constants mirror
// the GLFW viewer so the Qt and GLFW renderers feel identical to drive.
struct OrbitCamera {
  Vec3 target{0.0f, 0.0f, 0.0f};
  float radius = 1.0f;
  float distance = 3.0f;
  float yaw = 0.0f;
  float pitch = 0.22f;

  // Centre on the data and back off to fit its bounding diagonal.
  void Frame(const Bounds& b) {
    target = {static_cast<float>(0.5 * (b.v[0] + b.v[1])),
              static_cast<float>(0.5 * (b.v[2] + b.v[3])),
              static_cast<float>(0.5 * (b.v[4] + b.v[5]))};
    const float dx = static_cast<float>(b.v[1] - b.v[0]);
    const float dy = static_cast<float>(b.v[3] - b.v[2]);
    const float dz = static_cast<float>(b.v[5] - b.v[4]);
    radius = std::max(1.0f, 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz));
    distance = radius * 3.0f;
    yaw = 0.0f;
    pitch = 0.22f;
  }

  void Rotate(double dx, double dy) {
    // Grab-style: the object follows the cursor. pitch is negated (drag down
    // tilts the top toward you); yaw too — but only while upright. Removing the
    // pitch clamp let the orbit pass over a pole, where the view turns
    // upside-down (cos pitch < 0) and the on-screen horizontal flips; invert the
    // yaw delta there so left/right stays consistent in every orientation.
    const float yawSign = std::cos(pitch) < 0.0f ? 1.0f : -1.0f;
    yaw = WrapAngle(yaw + yawSign * static_cast<float>(dx) * 0.006f);
    pitch = WrapAngle(pitch - static_cast<float>(dy) * 0.006f);
  }

  void Zoom(double steps) {
    distance *= std::pow(0.88f, static_cast<float>(steps));
    distance = std::clamp(distance, radius * 0.25f, radius * 30.0f);
  }

  // Dolly while keeping the world point under the cursor fixed on screen
  // (zoom-to-cursor). ndc in [-1, 1]; ZoomToCursor(steps, 0, 0, aspect) == Zoom.
  // It shifts target by the cursor's focus-plane offset times (1 - zoomFactor):
  // a focus-plane point's screen position is offset/distance, which is then
  // invariant under the dolly, so it stays under the cursor.
  void ZoomToCursor(double steps, float ndcx, float ndcy, float aspect) {
    const float newDist = std::clamp(distance * std::pow(0.88f, static_cast<float>(steps)),
                                     radius * 0.25f, radius * 30.0f);
    const float zoomFactor = (distance > 0.0f) ? newDist / distance : 1.0f;
    const float halfH = distance * std::tan(0.5f * kFovYRadians);
    const float halfW = aspect * halfH;
    const Vec3 offset = Right() * (ndcx * halfW) + Up() * (ndcy * halfH);
    target = target + offset * (1.0f - zoomFactor);
    distance = newDist;
  }

  void Pan(double dx, double dy) {
    const float scale = 0.0018f * distance;
    const Vec3 delta =
        Right() * static_cast<float>(-dx * scale) + Up() * static_cast<float>(dy * scale);
    target = target + delta;
  }

  Vec3 Forward() const {
    const float cp = std::cos(pitch);
    return {std::sin(yaw) * cp, -std::cos(yaw) * cp, std::sin(pitch)};
  }

  // Screen right/up of the current orientation, derived straight from yaw/pitch
  // (not Cross(forward, worldUp)) so they stay valid past pitch = ±90° — that is
  // what lets rotation pass over the poles without the view flipping. At pitch 0
  // they reduce to (-cos yaw, -sin yaw, 0) and world-up, matching the old basis.
  Vec3 Right() const { return {-std::cos(yaw), -std::sin(yaw), 0.0f}; }
  Vec3 Up() const {
    const float sp = std::sin(pitch);
    return {-sp * std::sin(yaw), sp * std::cos(yaw), std::cos(pitch)};
  }

  Vec3 Eye() const { return target - Forward() * distance; }

  // Combined projection * view, ready to upload (row-major, transpose on upload).
  Mat4 ViewProj(float aspect) const {
    const Mat4 view = LookAt(Eye(), target, Up());
    const Mat4 proj =
        Perspective(kFovYRadians, std::max(1e-3f, aspect), radius * 0.01f, radius * 80.0f);
    return Multiply(proj, view);
  }
};

}  // namespace tracto
