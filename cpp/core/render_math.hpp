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

// Row-major GL-style orthographic projection (z in [-1, 1]); like Perspective,
// it is corrected for RHI clip space by clipSpaceCorrMatrix() at upload.
inline Mat4 Ortho(float l, float r, float b, float t, float n, float f) {
  Mat4 out;
  out.m[0] = 2.0f / (r - l);   out.m[3] = -(r + l) / (r - l);
  out.m[5] = 2.0f / (t - b);   out.m[7] = -(t + b) / (t - b);
  out.m[10] = -2.0f / (f - n); out.m[11] = -(f + n) / (f - n);
  out.m[15] = 1.0f;
  return out;
}

// Inverse of a row-major affine (last row [0 0 0 1]) — invert the 3x3 then the
// translation. Used for world->voxel (focus point -> slice index). Identity on a
// singular matrix.
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

// General row-major 4x4 inverse (Gauss-Jordan). Used to unproject the cursor to a
// world-space ray. Returns identity on a singular matrix.
inline Mat4 Inverse(const Mat4& m) {
  double a[4][8];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) { a[r][c] = m.m[r * 4 + c]; a[r][4 + c] = (r == c) ? 1.0 : 0.0; }
  for (int col = 0; col < 4; ++col) {
    int piv = col;
    double best = std::abs(a[col][col]);
    for (int r = col + 1; r < 4; ++r)
      if (std::abs(a[r][col]) > best) { best = std::abs(a[r][col]); piv = r; }
    if (best < 1e-20) return Identity();
    if (piv != col)
      for (int c = 0; c < 8; ++c) { const double t = a[col][c]; a[col][c] = a[piv][c]; a[piv][c] = t; }
    const double d = a[col][col];
    for (int c = 0; c < 8; ++c) a[col][c] /= d;
    for (int r = 0; r < 4; ++r) {
      if (r == col) continue;
      const double f = a[r][col];
      if (f != 0.0)
        for (int c = 0; c < 8; ++c) a[r][c] -= f * a[col][c];
    }
  }
  Mat4 out;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) out.m[r * 4 + c] = static_cast<float>(a[r][4 + c]);
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
  // Default: a 45° superior-oblique view. The eye sits in the anterior-right-
  // superior octant (yaw -45°, pitch -45°) so in RAS the frontal lobe (+Y) lands
  // bottom-right and the occipital (-Y) top-left, looking 45° down from above.
  float yaw = -0.785398f;    // -pi/4
  float pitch = -0.785398f;  // -pi/4 (negative = eye above, looking down)

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
    yaw = -kPi / 4.0f;    // superior-oblique default (see member init above)
    pitch = -kPi / 4.0f;
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

// Axis-locked orthographic camera for the 2-D slice panes (sagittal/coronal/
// axial). It looks straight down one world axis at a focus point and only pans,
// zooms, and scrubs the slice — no rotation. axis: 0=X (sagittal), 1=Y
// (coronal), 2=Z (axial). `focus` is the shared linked crosshair; the pane shows
// the slice at focus[axis]. Right()/Up() are the on-screen world axes (matched to
// LookAt's basis so panning moves the focus in true screen directions).
struct OrthoSliceCamera {
  int axis = 2;
  Vec3 focus{0.0f, 0.0f, 0.0f};
  float halfH = 1.0f;       // half view height in world mm (zoom)
  float depth = 100.0f;     // half ortho depth; large enough to span the data

  void Frame(const Bounds& b, int ax) {
    axis = ax;
    focus = {static_cast<float>(0.5 * (b.v[0] + b.v[1])),
             static_cast<float>(0.5 * (b.v[2] + b.v[3])),
             static_cast<float>(0.5 * (b.v[4] + b.v[5]))};
    const float ex = static_cast<float>(b.v[1] - b.v[0]);
    const float ey = static_cast<float>(b.v[3] - b.v[2]);
    const float ez = static_cast<float>(b.v[5] - b.v[4]);
    const Vec3 r = Right(), u = Up();
    const float halfW = 0.5f * (std::abs(r.x) * ex + std::abs(r.y) * ey + std::abs(r.z) * ez);
    const float halfHt = 0.5f * (std::abs(u.x) * ex + std::abs(u.y) * ey + std::abs(u.z) * ez);
    halfH = std::max(1.0f, std::max(halfW, halfHt)) * 1.05f;  // margin; aspect added in ViewProj
    depth = std::max(1.0f, ex + ey + ez);
  }

  // Screen-right / screen-up / into-screen in world, for the locked axis. These
  // equal LookAt(Eye(), focus, Up())'s basis (verified per axis).
  Vec3 Right() const {
    if (axis == 0) return {0.0f, 1.0f, 0.0f};   // sagittal: +Y to the right
    if (axis == 1) return {-1.0f, 0.0f, 0.0f};  // coronal:  -X to the right
    return {1.0f, 0.0f, 0.0f};                  // axial:    +X to the right
  }
  Vec3 Up() const { return axis == 2 ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{0.0f, 0.0f, 1.0f}; }
  Vec3 Forward() const {
    return {axis == 0 ? -1.0f : 0.0f, axis == 1 ? -1.0f : 0.0f, axis == 2 ? -1.0f : 0.0f};
  }
  Vec3 Eye() const { return focus - Forward() * depth; }

  void Zoom(double steps) {
    halfH *= std::pow(0.88f, static_cast<float>(steps));
    halfH = std::clamp(halfH, 0.1f, 1.0e5f);
  }

  // Grab-style pan: content follows the cursor (focus moves opposite the drag in
  // x, with it in y since screen-y points down). dx/dy are pixels; px the pane size.
  void Pan(double dx, double dy, int pxW, int pxH) {
    const float halfW = halfH * (pxH > 0 ? static_cast<float>(pxW) / pxH : 1.0f);
    const float worldPerPxX = (2.0f * halfW) / std::max(1, pxW);
    const float worldPerPxY = (2.0f * halfH) / std::max(1, pxH);
    focus = focus - Right() * static_cast<float>(dx * worldPerPxX) +
            Up() * static_cast<float>(dy * worldPerPxY);
  }

  void Scrub(float worldDelta) {  // move the slice along the locked axis
    if (axis == 0) focus.x += worldDelta;
    else if (axis == 1) focus.y += worldDelta;
    else focus.z += worldDelta;
  }
  float SliceCoord() const { return axis == 0 ? focus.x : axis == 1 ? focus.y : focus.z; }

  Mat4 ViewProj(float aspect) const {
    const Mat4 view = LookAt(Eye(), focus, Up());
    const float halfW = halfH * std::max(1.0e-3f, aspect);
    const Mat4 proj = Ortho(-halfW, halfW, -halfH, halfH, 0.0f, 2.0f * depth);
    return Multiply(proj, view);
  }
};

}  // namespace tracto
