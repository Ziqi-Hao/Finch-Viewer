#include "glfw_tract_viewer.hpp"

#include "streamline_ops.hpp"
#include "trk_io.hpp"
#include "utils.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define GLFW_EXPOSE_NATIVE_WIN32
#endif

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#ifdef _WIN32
#include <GLFW/glfw3native.h>
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace tracto {
namespace {

constexpr float kPi = 3.14159265358979323846f;

struct Vec3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

struct Mat4 {
  float m[16] = {};
};

Vec3 operator+(Vec3 a, Vec3 b) {
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 operator-(Vec3 a, Vec3 b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 operator*(Vec3 v, float s) {
  return {v.x * s, v.y * s, v.z * s};
}

float Dot(Vec3 a, Vec3 b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 Cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y,
          a.z * b.x - a.x * b.z,
          a.x * b.y - a.y * b.x};
}

Vec3 Normalize(Vec3 v) {
  const float n = std::sqrt(std::max(1e-12f, Dot(v, v)));
  return {v.x / n, v.y / n, v.z / n};
}

Mat4 Multiply(const Mat4& a, const Mat4& b) {
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

Mat4 Identity() {
  Mat4 out;
  out.m[0] = 1.0f;
  out.m[5] = 1.0f;
  out.m[10] = 1.0f;
  out.m[15] = 1.0f;
  return out;
}

Mat4 Perspective(float fovyRadians, float aspect, float zNear, float zFar) {
  const float f = 1.0f / std::tan(0.5f * fovyRadians);
  Mat4 out;
  out.m[0] = f / aspect;
  out.m[5] = f;
  out.m[10] = (zFar + zNear) / (zNear - zFar);
  out.m[11] = (2.0f * zFar * zNear) / (zNear - zFar);
  out.m[14] = -1.0f;
  return out;
}

Mat4 LookAt(Vec3 eye, Vec3 center, Vec3 up) {
  const Vec3 f = Normalize(center - eye);
  const Vec3 s = Normalize(Cross(f, up));
  const Vec3 u = Cross(s, f);

  Mat4 out;
  out.m[0] = s.x;
  out.m[1] = s.y;
  out.m[2] = s.z;
  out.m[3] = -Dot(s, eye);
  out.m[4] = u.x;
  out.m[5] = u.y;
  out.m[6] = u.z;
  out.m[7] = -Dot(u, eye);
  out.m[8] = -f.x;
  out.m[9] = -f.y;
  out.m[10] = -f.z;
  out.m[11] = Dot(f, eye);
  out.m[15] = 1.0f;
  return out;
}

unsigned int CompileShader(unsigned int type, const char* source) {
  const unsigned int shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);

  int ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048] = {};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    throw std::runtime_error(std::string("GL shader compile failed: ") + log);
  }
  return shader;
}

unsigned int CreateLineProgram() {
  const char* vertexSource = R"glsl(
    #version 330 core
    layout(location = 0) in vec3 inPos;
    layout(location = 1) in vec3 inColor;
    uniform mat4 uMvp;
    out vec3 color;
    void main() {
      color = inColor;
      gl_Position = uMvp * vec4(inPos, 1.0);
    }
  )glsl";

  const char* fragmentSource = R"glsl(
    #version 330 core
    in vec3 color;
    out vec4 outColor;
    void main() {
      outColor = vec4(color, 1.0);
    }
  )glsl";

  const unsigned int vs = CompileShader(GL_VERTEX_SHADER, vertexSource);
  const unsigned int fs = CompileShader(GL_FRAGMENT_SHADER, fragmentSource);
  const unsigned int program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glLinkProgram(program);
  glDeleteShader(vs);
  glDeleteShader(fs);

  int ok = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[2048] = {};
    glGetProgramInfoLog(program, sizeof(log), nullptr, log);
    throw std::runtime_error(std::string("GL program link failed: ") + log);
  }
  return program;
}

void AppendVertex(std::vector<float>& vertices,
                  const float* xyz,
                  const std::array<unsigned char, 3>& rgb) {
  vertices.push_back(xyz[0]);
  vertices.push_back(xyz[1]);
  vertices.push_back(xyz[2]);
  vertices.push_back(static_cast<float>(rgb[0]) / 255.0f);
  vertices.push_back(static_cast<float>(rgb[1]) / 255.0f);
  vertices.push_back(static_cast<float>(rgb[2]) / 255.0f);
}

void AppendSolidVertex(std::vector<float>& vertices,
                       Vec3 xyz,
                       std::array<float, 3> rgb) {
  vertices.push_back(xyz.x);
  vertices.push_back(xyz.y);
  vertices.push_back(xyz.z);
  vertices.push_back(rgb[0]);
  vertices.push_back(rgb[1]);
  vertices.push_back(rgb[2]);
}

void AppendLine(std::vector<float>& vertices,
                Vec3 a,
                Vec3 b,
                std::array<float, 3> rgb) {
  AppendSolidVertex(vertices, a, rgb);
  AppendSolidVertex(vertices, b, rgb);
}

void InitBuffer(unsigned int& vao,
                unsigned int& vbo,
                const std::vector<float>& vertices) {
  glGenVertexArrays(1, &vao);
  glGenBuffers(1, &vbo);
  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
               vertices.data(),
               GL_STATIC_DRAW);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), nullptr);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(1,
                        3,
                        GL_FLOAT,
                        GL_FALSE,
                        6 * sizeof(float),
                        reinterpret_cast<void*>(3 * sizeof(float)));
  glEnableVertexAttribArray(1);
  glBindVertexArray(0);
}

void WriteFramebufferPpm(const std::string& path, int width, int height) {
  if (width <= 0 || height <= 0) {
    throw std::runtime_error("invalid framebuffer size for screenshot");
  }

  std::vector<unsigned char> rgba(static_cast<std::size_t>(width) *
                                  static_cast<std::size_t>(height) * 4);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadBuffer(GL_BACK);
  glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("cannot write GLFW screenshot: " + path);
  }

  out << "P6\n" << width << " " << height << "\n255\n";
  for (int y = height - 1; y >= 0; --y) {
    const auto row = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
    for (int x = 0; x < width; ++x) {
      const auto i = row + static_cast<std::size_t>(x) * 4;
      const unsigned char rgb[3] = {rgba[i], rgba[i + 1], rgba[i + 2]};
      out.write(reinterpret_cast<const char*>(rgb), 3);
    }
  }
}

std::vector<unsigned char> ReadBackBufferBgraTopDown(int width, int height) {
  std::vector<unsigned char> rgba(static_cast<std::size_t>(width) *
                                  static_cast<std::size_t>(height) * 4);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadBuffer(GL_BACK);
  glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

  std::vector<unsigned char> bgra(static_cast<std::size_t>(width) *
                                  static_cast<std::size_t>(height) * 4);
  for (int y = 0; y < height; ++y) {
    const int srcY = height - 1 - y;
    for (int x = 0; x < width; ++x) {
      const auto src = (static_cast<std::size_t>(srcY) * width + x) * 4;
      const auto dst = (static_cast<std::size_t>(y) * width + x) * 4;
      bgra[dst] = rgba[src + 2];
      bgra[dst + 1] = rgba[src + 1];
      bgra[dst + 2] = rgba[src];
      bgra[dst + 3] = 255;
    }
  }
  return bgra;
}

#ifdef _WIN32
void BlitBgraToWin32Window(GLFWwindow* window,
                           const std::vector<unsigned char>& bgra,
                           int width,
                           int height) {
  HWND hwnd = glfwGetWin32Window(window);
  if (!hwnd || bgra.empty()) {
    return;
  }

  HDC dc = GetDC(hwnd);
  if (!dc) {
    return;
  }

  BITMAPINFO bmi = {};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = width;
  bmi.bmiHeader.biHeight = -height;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  RECT rect = {};
  GetClientRect(hwnd, &rect);
  const int clientW = std::max(1L, rect.right - rect.left);
  const int clientH = std::max(1L, rect.bottom - rect.top);

  StretchDIBits(dc,
                0,
                0,
                clientW,
                clientH,
                0,
                0,
                width,
                height,
                bgra.data(),
                &bmi,
                DIB_RGB_COLORS,
                SRCCOPY);
  ReleaseDC(hwnd, dc);
}
#else
void BlitBgraToWin32Window(GLFWwindow*,
                           const std::vector<unsigned char>&,
                           int,
                           int) {}
#endif

void ExpandBounds(Bounds& b, const float* xyz, bool& initialized) {
  if (!initialized) {
    b.v[0] = b.v[1] = xyz[0];
    b.v[2] = b.v[3] = xyz[1];
    b.v[4] = b.v[5] = xyz[2];
    initialized = true;
    return;
  }
  b.v[0] = std::min<double>(b.v[0], xyz[0]);
  b.v[1] = std::max<double>(b.v[1], xyz[0]);
  b.v[2] = std::min<double>(b.v[2], xyz[1]);
  b.v[3] = std::max<double>(b.v[3], xyz[1]);
  b.v[4] = std::min<double>(b.v[4], xyz[2]);
  b.v[5] = std::max<double>(b.v[5], xyz[2]);
}

}  // namespace

GlfwTractViewer::GlfwTractViewer(Args args) : args_(std::move(args)) {}

void GlfwTractViewer::Load() {
  std::cout << "Loading OR   : " << args_.trkPath << "\n";
  tractogram_.streamlines = LoadTrk(args_.trkPath, tractogram_.header);
  BuildSoA(tractogram_);
  std::cout << "  " << FormatCount(tractogram_.StreamlineCount()) << " streamlines, "
            << FormatCount(tractogram_.TotalPointCount()) << " points\n";
  BuildGpuVertices();
}

void GlfwTractViewer::BuildGpuVertices() {
  std::vector<unsigned char> alive(tractogram_.StreamlineCount(), 1);
  const std::vector<int> display = MakeDisplayIndicesFromAlive(alive, args_.displayN, args_.seed);

  lineVertices_.clear();
  referenceVertices_.clear();
  bool boundsInitialized = false;

  for (int fullIndex : display) {
    const Streamline& sl = tractogram_.streamlines[static_cast<std::size_t>(fullIndex)];
    if (sl.pointCount < 2) {
      continue;
    }

    std::vector<float> points;
    points.reserve((static_cast<std::size_t>(sl.pointCount) / args_.dispStep + 2) * 3);
    int32_t lastPushed = -1;
    for (int32_t p = 0; p < sl.pointCount; p += args_.dispStep) {
      const std::size_t src = static_cast<std::size_t>(p) * 3;
      points.push_back(sl.rasPoints[src]);
      points.push_back(sl.rasPoints[src + 1]);
      points.push_back(sl.rasPoints[src + 2]);
      lastPushed = p;
    }
    if (lastPushed != sl.pointCount - 1) {
      const std::size_t src = static_cast<std::size_t>(sl.pointCount - 1) * 3;
      points.push_back(sl.rasPoints[src]);
      points.push_back(sl.rasPoints[src + 1]);
      points.push_back(sl.rasPoints[src + 2]);
    }

    const std::size_t count = points.size() / 3;
    if (count < 2) {
      continue;
    }

    for (std::size_t p = 0; p + 1 < count; ++p) {
      const float* a = points.data() + p * 3;
      const float* b = points.data() + (p + 1) * 3;
      AppendVertex(lineVertices_, a, DirectionRgbFromPoints(points.data(), count, p));
      AppendVertex(lineVertices_, b, DirectionRgbFromPoints(points.data(), count, p + 1));
      ExpandBounds(displayBounds_, a, boundsInitialized);
      ExpandBounds(displayBounds_, b, boundsInitialized);
    }
  }

  if (!boundsInitialized) {
    displayBounds_.v[0] = displayBounds_.v[2] = displayBounds_.v[4] = -1.0;
    displayBounds_.v[1] = displayBounds_.v[3] = displayBounds_.v[5] = 1.0;
  }

  const Vec3 lo{static_cast<float>(displayBounds_.v[0]),
                static_cast<float>(displayBounds_.v[2]),
                static_cast<float>(displayBounds_.v[4])};
  const Vec3 hi{static_cast<float>(displayBounds_.v[1]),
                static_cast<float>(displayBounds_.v[3]),
                static_cast<float>(displayBounds_.v[5])};
  const Vec3 corners[8] = {
      {lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z},
      {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z},
  };
  const std::array<float, 3> yellow = {1.0f, 0.86f, 0.0f};
  AppendLine(referenceVertices_, corners[0], corners[1], yellow);
  AppendLine(referenceVertices_, corners[1], corners[2], yellow);
  AppendLine(referenceVertices_, corners[2], corners[3], yellow);
  AppendLine(referenceVertices_, corners[3], corners[0], yellow);
  AppendLine(referenceVertices_, corners[4], corners[5], yellow);
  AppendLine(referenceVertices_, corners[5], corners[6], yellow);
  AppendLine(referenceVertices_, corners[6], corners[7], yellow);
  AppendLine(referenceVertices_, corners[7], corners[4], yellow);
  AppendLine(referenceVertices_, corners[0], corners[4], yellow);
  AppendLine(referenceVertices_, corners[1], corners[5], yellow);
  AppendLine(referenceVertices_, corners[2], corners[6], yellow);
  AppendLine(referenceVertices_, corners[3], corners[7], yellow);

  ResetCamera();

  std::cout << "Packing streamlines into OpenGL buffers ...\n";
  std::cout << "  " << FormatCount(display.size()) << " streamlines  ("
            << FormatCount(lineVertices_.size() / 6) << " GL line vertices)\n";
}

void GlfwTractViewer::ResetCamera() {
  target_ = {static_cast<float>(0.5 * (displayBounds_.v[0] + displayBounds_.v[1])),
             static_cast<float>(0.5 * (displayBounds_.v[2] + displayBounds_.v[3])),
             static_cast<float>(0.5 * (displayBounds_.v[4] + displayBounds_.v[5]))};
  const float dx = static_cast<float>(displayBounds_.v[1] - displayBounds_.v[0]);
  const float dy = static_cast<float>(displayBounds_.v[3] - displayBounds_.v[2]);
  const float dz = static_cast<float>(displayBounds_.v[5] - displayBounds_.v[4]);
  radius_ = std::max(1.0f, 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz));
  distance_ = radius_ * 3.0f;
  yaw_ = 0.0f;
  pitch_ = 0.22f;
}

void GlfwTractViewer::RotateCamera(double dx, double dy) {
  // Grab-style, matching OrbitCamera::Rotate. No pitch clamp (full 360°); invert
  // the yaw delta when the view is upside-down (cos pitch < 0) so left/right
  // stays consistent past the poles.
  const float yawSign = std::cos(pitch_) < 0.0f ? 1.0f : -1.0f;
  yaw_ += yawSign * static_cast<float>(dx) * 0.006f;
  pitch_ -= static_cast<float>(dy) * 0.006f;
}

void GlfwTractViewer::ZoomCamera(double delta) {
  distance_ *= std::pow(0.88f, static_cast<float>(delta));
  distance_ = std::clamp(distance_, radius_ * 0.25f, radius_ * 30.0f);
}

void GlfwTractViewer::PanCamera(double dx, double dy) {
  // Screen right/up derived from yaw/pitch (valid past ±90°); matches OrbitCamera.
  const float sp = std::sin(pitch_);
  const Vec3 right{-std::cos(yaw_), -std::sin(yaw_), 0.0f};
  const Vec3 up{-sp * std::sin(yaw_), sp * std::cos(yaw_), std::cos(pitch_)};
  const float scale = 0.0018f * distance_;
  const Vec3 delta = right * static_cast<float>(-dx * scale) + up * static_cast<float>(dy * scale);
  target_[0] += delta.x;
  target_[1] += delta.y;
  target_[2] += delta.z;
}

void GlfwTractViewer::DrawFrame(int width, int height) {
  const float aspect = std::max(1.0f, static_cast<float>(width)) /
                       std::max(1.0f, static_cast<float>(height));
  const float cp = std::cos(pitch_);
  const float sp = std::sin(pitch_);
  const Vec3 forward{std::sin(yaw_) * cp, -std::cos(yaw_) * cp, sp};
  const Vec3 center{target_[0], target_[1], target_[2]};
  const Vec3 eye = center - forward * distance_;
  // Pitch-aware up so rotation can pass over the poles (matches OrbitCamera::Up).
  const Vec3 up{-sp * std::sin(yaw_), sp * std::cos(yaw_), cp};
  const Mat4 view = LookAt(eye, center, up);
  const Mat4 proj = Perspective(50.0f * kPi / 180.0f, aspect, radius_ * 0.01f, radius_ * 80.0f);
  const Mat4 mvp = Multiply(proj, view);

  glViewport(0, 0, std::max(1, width), std::max(1, height));
  const float t = static_cast<float>(glfwGetTime());
  glClearColor(0.025f + 0.025f * std::sin(t * 0.8f),
               0.030f,
               0.045f + 0.025f * std::cos(t * 0.6f),
               1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  glUseProgram(program_);
  const int loc = glGetUniformLocation(program_, "uMvp");
  glUniformMatrix4fv(loc, 1, GL_TRUE, mvp.m);

  glBindVertexArray(lineVao_);
  glLineWidth(1.2f);
  glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(lineVertexCount_));

  glBindVertexArray(refVao_);
  glLineWidth(2.6f);
  glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(refVertexCount_));

  glDisable(GL_DEPTH_TEST);
  const Mat4 identity = Identity();
  glUniformMatrix4fv(loc, 1, GL_TRUE, identity.m);
  glBindVertexArray(debugVao_);
  glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(debugVertexCount_));
  glEnable(GL_DEPTH_TEST);

  glBindVertexArray(0);
}

void GlfwTractViewer::Run() {
  glfwSetErrorCallback([](int code, const char* message) {
    std::cerr << "GLFW error " << code << ": " << (message ? message : "") << "\n";
  });

  if (!glfwInit()) {
    throw std::runtime_error("glfwInit failed");
  }

  glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_DOUBLEBUFFER, GLFW_TRUE);
  glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
  glfwWindowHint(GLFW_SAMPLES, 0);

  GLFWwindow* window = glfwCreateWindow(1280, 900, "tractography GLFW editor", nullptr, nullptr);
  if (!window) {
    glfwTerminate();
    throw std::runtime_error("glfwCreateWindow failed");
  }

  glfwMakeContextCurrent(window);
  glfwShowWindow(window);
  glfwFocusWindow(window);
  glfwSwapInterval(0);
  glfwSetWindowUserPointer(window, this);

  if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
    glfwDestroyWindow(window);
    glfwTerminate();
    throw std::runtime_error("gladLoadGLLoader failed");
  }

  std::cout << "GLFW/OpenGL viewer\n";
  std::cout << "  OpenGL vendor  : " << glGetString(GL_VENDOR) << "\n";
  std::cout << "  OpenGL renderer: " << glGetString(GL_RENDERER) << "\n";
  std::cout << "  OpenGL version : " << glGetString(GL_VERSION) << "\n";
  std::cout << "Controls: left drag=rotate, right/middle drag=pan, wheel=zoom, r=reset, Esc=quit\n";
  if (args_.gdiBlit) {
    std::cout << "Display fallback: Win32 GDI blit is enabled (--gdi-blit).\n";
  }

  program_ = CreateLineProgram();
  InitBuffer(lineVao_, lineVbo_, lineVertices_);
  InitBuffer(refVao_, refVbo_, referenceVertices_);
  const std::vector<float> debugVertices = {
      -0.96f, 0.72f, 0.0f, 1.0f, 0.10f, 0.18f,
      -0.72f, 0.72f, 0.0f, 0.15f, 0.95f, 0.35f,
      -0.84f, 0.94f, 0.0f, 0.20f, 0.55f, 1.00f,
  };
  InitBuffer(debugVao_, debugVbo_, debugVertices);
  lineVertexCount_ = lineVertices_.size() / 6;
  refVertexCount_ = referenceVertices_.size() / 6;
  debugVertexCount_ = debugVertices.size() / 6;

  glEnable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_MULTISAMPLE);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

  glfwSetMouseButtonCallback(window, [](GLFWwindow* w, int button, int action, int) {
    auto* app = static_cast<GlfwTractViewer*>(glfwGetWindowUserPointer(w));
    if (!app) {
      return;
    }
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
      app->dragging_ = action == GLFW_PRESS;
    }
    if (button == GLFW_MOUSE_BUTTON_RIGHT || button == GLFW_MOUSE_BUTTON_MIDDLE) {
      app->panning_ = action == GLFW_PRESS;
    }
    glfwGetCursorPos(w, &app->lastX_, &app->lastY_);
  });

  glfwSetCursorPosCallback(window, [](GLFWwindow* w, double x, double y) {
    auto* app = static_cast<GlfwTractViewer*>(glfwGetWindowUserPointer(w));
    if (!app) {
      return;
    }
    const double dx = x - app->lastX_;
    const double dy = y - app->lastY_;
    if (app->dragging_) {
      app->RotateCamera(dx, dy);
    } else if (app->panning_) {
      app->PanCamera(dx, dy);
    }
    app->lastX_ = x;
    app->lastY_ = y;
  });

  glfwSetScrollCallback(window, [](GLFWwindow* w, double, double yoffset) {
    auto* app = static_cast<GlfwTractViewer*>(glfwGetWindowUserPointer(w));
    if (app) {
      app->ZoomCamera(yoffset);
    }
  });

  int frame = 0;
  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
      glfwSetWindowShouldClose(window, GLFW_TRUE);
    }
    if (glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS) {
      ResetCamera();
    }

    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    if (width <= 0 || height <= 0) {
      glfwGetWindowSize(window, &width, &height);
    }

    if (frame < 5) {
      std::cout << "  frame " << frame << " framebuffer=" << width << "x" << height
                << " lineVertices=" << FormatCount(lineVertexCount_) << "\n";
    }

    DrawFrame(width, height);
    const GLenum glError = glGetError();
    if (frame < 5 && glError != GL_NO_ERROR) {
      std::cerr << "  OpenGL error after DrawFrame: 0x" << std::hex << glError << std::dec << "\n";
    }

    std::vector<unsigned char> gdiFrame;
    if (args_.gdiBlit) {
      gdiFrame = ReadBackBufferBgraTopDown(width, height);
    }

    if (frame == 0 && !args_.screenshotPath.empty()) {
      WriteFramebufferPpm(args_.screenshotPath, width, height);
      std::cout << "GLFW screenshot -> " << args_.screenshotPath << "\n";
      glfwSetWindowShouldClose(window, GLFW_TRUE);
    }

    glfwSwapBuffers(window);
    if (args_.gdiBlit) {
      BlitBgraToWin32Window(window, gdiFrame, width, height);
    }
    glFinish();
    if (frame % 60 == 0) {
      std::string title = "tractography GLFW editor  " +
                          std::to_string(width) + "x" + std::to_string(height) +
                          "  vertices=" + std::to_string(lineVertexCount_);
      glfwSetWindowTitle(window, title.c_str());
    }
    ++frame;
  }

  glDeleteBuffers(1, &lineVbo_);
  glDeleteVertexArrays(1, &lineVao_);
  glDeleteBuffers(1, &refVbo_);
  glDeleteVertexArrays(1, &refVao_);
  glDeleteBuffers(1, &debugVbo_);
  glDeleteVertexArrays(1, &debugVao_);
  glDeleteProgram(program_);
  glfwDestroyWindow(window);
  glfwTerminate();
}

}  // namespace tracto
