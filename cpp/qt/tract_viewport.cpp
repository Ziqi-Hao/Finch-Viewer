#include "tract_viewport.hpp"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>

#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>

namespace tracto {
namespace {

constexpr char kVertexShader[] = R"glsl(
  #version 330 core
  layout(location = 0) in vec3 inPos;
  layout(location = 1) in vec3 inColor;
  uniform mat4 uMvp;
  uniform float uPointSize;   // 0 for lines; >0 for GL_POINTS handles
  out vec3 color;
  void main() {
    color = inColor;
    gl_PointSize = uPointSize;
    gl_Position = uMvp * vec4(inPos, 1.0);
  }
)glsl";

constexpr char kFragmentShader[] = R"glsl(
  #version 330 core
  in vec3 color;
  out vec4 outColor;
  void main() { outColor = vec4(color, 1.0); }
)glsl";

// volume slice program: a quad carries voxel coordinates; the affine places it in
// RAS mm, and the 3D texture is sampled in the fragment shader (no CPU slice
// extraction). Grayscale + alpha ramp mirror the VTK editor's lookup table.
constexpr char kSliceVertexShader[] = R"glsl(
  #version 330 core
  layout(location = 0) in vec3 inVoxel;
  uniform mat4 uMvp;
  uniform mat4 uVoxToWorld;
  uniform vec3 uInvDims;
  out vec3 vTex;
  void main() {
    vTex = (inVoxel + vec3(0.5)) * uInvDims;     // sample voxel centres
    gl_Position = uMvp * (uVoxToWorld * vec4(inVoxel, 1.0));
  }
)glsl";

constexpr char kSliceFragmentShader[] = R"glsl(
  #version 330 core
  in vec3 vTex;
  uniform sampler3D uVol;
  uniform float uValueMin;
  uniform float uValueRange;
  out vec4 outColor;
  void main() {
    float t = clamp((texture(uVol, vTex).r - uValueMin) / uValueRange, 0.0, 1.0);
    float a = t < 0.025 ? 0.0 : clamp(0.12 + 0.58 * t, 0.0, 1.0);
    if (a <= 0.0) discard;
    outColor = vec4(vec3(t), a);
  }
)glsl";

void PushVertex(std::vector<float>& out, Vec3 p, std::array<float, 3> rgb) {
  out.insert(out.end(), {p.x, p.y, p.z, rgb[0], rgb[1], rgb[2]});
}

void PushLine(std::vector<float>& out, Vec3 a, Vec3 b, std::array<float, 3> rgb) {
  PushVertex(out, a, rgb);
  PushVertex(out, b, rgb);
}

}  // namespace

TractViewport::TractViewport(QWidget* parent) : QOpenGLWidget(parent) {
  setFocusPolicy(Qt::StrongFocus);  // needed for keyPressEvent (reset camera)
}

TractViewport::~TractViewport() {
  // Tear down GL objects with the context current.
  if (context()) {
    makeCurrent();
    if (lineVbo_) glDeleteBuffers(1, &lineVbo_);
    if (lineVao_) glDeleteVertexArrays(1, &lineVao_);
    if (cageVbo_) glDeleteBuffers(1, &cageVbo_);
    if (cageVao_) glDeleteVertexArrays(1, &cageVao_);
    if (sliceVbo_) glDeleteBuffers(1, &sliceVbo_);
    if (sliceVao_) glDeleteVertexArrays(1, &sliceVao_);
    if (boxVbo_) glDeleteBuffers(1, &boxVbo_);
    if (boxVao_) glDeleteVertexArrays(1, &boxVao_);
    if (handleVbo_) glDeleteBuffers(1, &handleVbo_);
    if (handleVao_) glDeleteVertexArrays(1, &handleVao_);
    if (volTexture_) glDeleteTextures(1, &volTexture_);
    if (program_) glDeleteProgram(program_);
    if (sliceProgram_) glDeleteProgram(sliceProgram_);
    doneCurrent();
  }
}

void TractViewport::SetLineGeometry(std::vector<float> interleaved, const Bounds& bounds) {
  // Geometry only — does NOT re-frame the camera, so an edit (which rebuilds the
  // display) preserves the user's view. Camera framing is driven explicitly by
  // MainWindow::ResetCamera on new-data load. The selection box is placed once
  // (first geometry) and then persists across edits.
  lineData_ = std::move(interleaved);
  bounds_ = bounds;
  lineDirty_ = true;
  RebuildCage();
  if (!hasBox_) PlaceSelectionBoxInBounds(0.6);
  if (isValid()) {
    update();  // schedule a repaint; the upload happens in paintGL
  }
}

void TractViewport::ResetCamera() {
  camera_.Frame(bounds_);
  update();
}

void TractViewport::SetVolume(Volume volume) {
  if (volume.Empty()) return;
  // If no streamlines frame the view yet, frame on the volume instead. Test the
  // staged line data, not lineVertexCount_ (which is only set at upload/paint).
  const bool frameOnVolume = lineData_.empty();
  if (frameOnVolume) {
    bounds_ = WorldBounds(volume);
    camera_.Frame(bounds_);
    RebuildCage();
  }
  pendingVolume_ = std::move(volume);  // move: no multi-MB voxel copy
  volumeDirty_ = true;
  hasVolume_ = true;
  if (isValid()) update();
}

void TractViewport::SetVolumeVisible(bool visible) {
  showVolume_ = visible;
  update();
}

namespace {
unsigned int CompileShader(QOpenGLFunctions_3_3_Core& gl, unsigned int type, const char* src) {
  const unsigned int shader = gl.glCreateShader(type);
  gl.glShaderSource(shader, 1, &src, nullptr);
  gl.glCompileShader(shader);
  int ok = 0;
  gl.glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048] = {};
    gl.glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    throw std::runtime_error(std::string("GL shader compile failed: ") + log);
  }
  return shader;
}

unsigned int LinkProgram(QOpenGLFunctions_3_3_Core& gl, const char* vsrc, const char* fsrc) {
  const unsigned int vs = CompileShader(gl, GL_VERTEX_SHADER, vsrc);
  const unsigned int fs = CompileShader(gl, GL_FRAGMENT_SHADER, fsrc);
  const unsigned int program = gl.glCreateProgram();
  gl.glAttachShader(program, vs);
  gl.glAttachShader(program, fs);
  gl.glLinkProgram(program);
  gl.glDeleteShader(vs);
  gl.glDeleteShader(fs);
  int ok = 0;
  gl.glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[2048] = {};
    gl.glGetProgramInfoLog(program, sizeof(log), nullptr, log);
    throw std::runtime_error(std::string("GL program link failed: ") + log);
  }
  return program;
}
}  // namespace

void TractViewport::initializeGL() {
  initializeOpenGLFunctions();

  std::printf("Qt/OpenGL viewport\n");
  std::printf("  OpenGL vendor  : %s\n", glGetString(GL_VENDOR));
  std::printf("  OpenGL renderer: %s\n", glGetString(GL_RENDERER));
  std::printf("  OpenGL version : %s\n", glGetString(GL_VERSION));
  std::fflush(stdout);

  // Line/cage program: interleaved [pos.xyz, rgb].
  program_ = LinkProgram(*this, kVertexShader, kFragmentShader);
  mvpLoc_ = glGetUniformLocation(program_, "uMvp");
  pointSizeLoc_ = glGetUniformLocation(program_, "uPointSize");

  auto initColorBuffer = [this](unsigned int& vao, unsigned int& vbo) {
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                          reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
  };
  initColorBuffer(lineVao_, lineVbo_);
  initColorBuffer(cageVao_, cageVbo_);
  initColorBuffer(boxVao_, boxVbo_);
  initColorBuffer(handleVao_, handleVbo_);

  // volume slice program: quads carry only a voxel-coordinate position.
  sliceProgram_ = LinkProgram(*this, kSliceVertexShader, kSliceFragmentShader);
  sliceMvpLoc_ = glGetUniformLocation(sliceProgram_, "uMvp");
  sliceVoxToWorldLoc_ = glGetUniformLocation(sliceProgram_, "uVoxToWorld");
  sliceInvDimsLoc_ = glGetUniformLocation(sliceProgram_, "uInvDims");
  sliceValueMinLoc_ = glGetUniformLocation(sliceProgram_, "uValueMin");
  sliceValueRangeLoc_ = glGetUniformLocation(sliceProgram_, "uValueRange");
  glUseProgram(sliceProgram_);
  glUniform1i(glGetUniformLocation(sliceProgram_, "uVol"), 0);  // sampler -> texture unit 0
  glUseProgram(0);
  glGenVertexArrays(1, &sliceVao_);
  glGenBuffers(1, &sliceVbo_);
  glBindVertexArray(sliceVao_);
  glBindBuffer(GL_ARRAY_BUFFER, sliceVbo_);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
  glEnableVertexAttribArray(0);
  glBindVertexArray(0);
  glGenTextures(1, &volTexture_);

  glEnable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glEnable(GL_PROGRAM_POINT_SIZE);  // handle points size themselves via gl_PointSize

  // The context now exists; flush anything handed over before init.
  if (lineDirty_) UploadGeometry();
  if (!cageData_.empty()) {
    glBindBuffer(GL_ARRAY_BUFFER, cageVbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<long>(cageData_.size() * sizeof(float)),
                 cageData_.data(), GL_STATIC_DRAW);
    cageVertexCount_ = cageData_.size() / 6;
  }
  if (volumeDirty_) UploadVolume();
  if (hasBox_) RebuildSelectionGeometry();
}

void TractViewport::UploadVolume() {
  const Volume& v = pendingVolume_;
  voxToWorld_ = v.voxelToWorld;
  invDims_ = {1.0f / v.dims[0], 1.0f / v.dims[1], 1.0f / v.dims[2]};
  volValueMin_ = v.valueMin;
  volValueRange_ = std::max(1e-6f, v.valueMax - v.valueMin);

  // Upload the whole volume once as a 16-bit float 3D texture.
  glBindTexture(GL_TEXTURE_3D, volTexture_);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
  glTexImage3D(GL_TEXTURE_3D, 0, GL_R16F, v.dims[0], v.dims[1], v.dims[2], 0, GL_RED,
               GL_FLOAT, v.data.data());
  glBindTexture(GL_TEXTURE_3D, 0);

  // Three mid-index slice quads (sagittal/coronal/axial), in voxel coordinates.
  const float mx = static_cast<float>(v.dims[0] / 2);
  const float my = static_cast<float>(v.dims[1] / 2);
  const float mz = static_cast<float>(v.dims[2] / 2);
  const float ex = static_cast<float>(v.dims[0] - 1);
  const float ey = static_cast<float>(v.dims[1] - 1);
  const float ez = static_cast<float>(v.dims[2] - 1);
  auto quad = [](std::vector<float>& out, Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
    const Vec3 q[6] = {a, b, c, a, c, d};
    for (const Vec3& p : q) out.insert(out.end(), {p.x, p.y, p.z});
  };
  std::vector<float> slices;
  quad(slices, {mx, 0, 0}, {mx, ey, 0}, {mx, ey, ez}, {mx, 0, ez});  // X = mid
  quad(slices, {0, my, 0}, {ex, my, 0}, {ex, my, ez}, {0, my, ez});  // Y = mid
  quad(slices, {0, 0, mz}, {ex, 0, mz}, {ex, ey, mz}, {0, ey, mz});  // Z = mid
  glBindBuffer(GL_ARRAY_BUFFER, sliceVbo_);
  glBufferData(GL_ARRAY_BUFFER, static_cast<long>(slices.size() * sizeof(float)),
               slices.data(), GL_STATIC_DRAW);
  sliceVertexCount_ = slices.size() / 3;

  pendingVolume_.data.clear();   // free the CPU copy; the texture owns it now
  pendingVolume_.data.shrink_to_fit();
  volumeDirty_ = false;
}

void TractViewport::UploadGeometry() {
  glBindBuffer(GL_ARRAY_BUFFER, lineVbo_);
  glBufferData(GL_ARRAY_BUFFER, static_cast<long>(lineData_.size() * sizeof(float)),
               lineData_.data(), GL_STATIC_DRAW);
  lineVertexCount_ = lineData_.size() / 6;
  lineDirty_ = false;
}

void TractViewport::RebuildCage() {
  cageData_.clear();
  const Vec3 lo{static_cast<float>(bounds_.v[0]), static_cast<float>(bounds_.v[2]),
                static_cast<float>(bounds_.v[4])};
  const Vec3 hi{static_cast<float>(bounds_.v[1]), static_cast<float>(bounds_.v[3]),
                static_cast<float>(bounds_.v[5])};
  const Vec3 c[8] = {
      {lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z},
      {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z},
  };
  const std::array<float, 3> yellow{1.0f, 0.86f, 0.0f};
  const int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                            {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
  for (const auto& e : edges) {
    PushLine(cageData_, c[e[0]], c[e[1]], yellow);
  }

  if (isValid() && cageVbo_) {
    makeCurrent();
    glBindBuffer(GL_ARRAY_BUFFER, cageVbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<long>(cageData_.size() * sizeof(float)),
                 cageData_.data(), GL_STATIC_DRAW);
    cageVertexCount_ = cageData_.size() / 6;
    doneCurrent();
  }
}

void TractViewport::ResetSelectionBox() {
  PlaceSelectionBoxInBounds(0.6);
  update();
}

void TractViewport::SetSelectionBox(const Bounds& box) {
  // Normalize so min <= max per axis (panel spinboxes may cross over).
  for (int a = 0; a < 3; ++a) {
    boxBounds_.v[a * 2 + 0] = std::min(box.v[a * 2 + 0], box.v[a * 2 + 1]);
    boxBounds_.v[a * 2 + 1] = std::max(box.v[a * 2 + 0], box.v[a * 2 + 1]);
  }
  hasBox_ = true;
  RebuildSelectionGeometry();
  update();
}

void TractViewport::PlaceSelectionBoxInBounds(double frac) {
  const double cx = 0.5 * (bounds_.v[0] + bounds_.v[1]);
  const double cy = 0.5 * (bounds_.v[2] + bounds_.v[3]);
  const double cz = 0.5 * (bounds_.v[4] + bounds_.v[5]);
  const double hx = 0.5 * frac * (bounds_.v[1] - bounds_.v[0]);
  const double hy = 0.5 * frac * (bounds_.v[3] - bounds_.v[2]);
  const double hz = 0.5 * frac * (bounds_.v[5] - bounds_.v[4]);
  boxBounds_.v[0] = cx - hx; boxBounds_.v[1] = cx + hx;
  boxBounds_.v[2] = cy - hy; boxBounds_.v[3] = cy + hy;
  boxBounds_.v[4] = cz - hz; boxBounds_.v[5] = cz + hz;
  hasBox_ = true;
  RebuildSelectionGeometry();
}

std::array<Vec3, 7> TractViewport::HandlePositions() const {
  const float cx = static_cast<float>(0.5 * (boxBounds_.v[0] + boxBounds_.v[1]));
  const float cy = static_cast<float>(0.5 * (boxBounds_.v[2] + boxBounds_.v[3]));
  const float cz = static_cast<float>(0.5 * (boxBounds_.v[4] + boxBounds_.v[5]));
  return {{
      {cx, cy, cz},                                          // 0 center
      {static_cast<float>(boxBounds_.v[0]), cy, cz},         // 1 X-
      {static_cast<float>(boxBounds_.v[1]), cy, cz},         // 2 X+
      {cx, static_cast<float>(boxBounds_.v[2]), cz},         // 3 Y-
      {cx, static_cast<float>(boxBounds_.v[3]), cz},         // 4 Y+
      {cx, cy, static_cast<float>(boxBounds_.v[4])},         // 5 Z-
      {cx, cy, static_cast<float>(boxBounds_.v[5])},         // 6 Z+
  }};
}

void TractViewport::RebuildSelectionGeometry() {
  if (!hasBox_) return;
  const Vec3 lo{static_cast<float>(boxBounds_.v[0]), static_cast<float>(boxBounds_.v[2]),
                static_cast<float>(boxBounds_.v[4])};
  const Vec3 hi{static_cast<float>(boxBounds_.v[1]), static_cast<float>(boxBounds_.v[3]),
                static_cast<float>(boxBounds_.v[5])};
  const Vec3 c[8] = {
      {lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z},
      {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z},
  };
  const int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                            {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
  const std::array<float, 3> cyan{0.1f, 0.9f, 1.0f};
  boxData_.clear();
  for (const auto& e : edges) PushLine(boxData_, c[e[0]], c[e[1]], cyan);

  handleData_.clear();
  const auto handles = HandlePositions();
  const std::array<float, 3> orange{1.0f, 0.55f, 0.1f};  // center (move)
  const std::array<float, 3> green{0.2f, 1.0f, 0.6f};    // faces (resize)
  for (std::size_t i = 0; i < handles.size(); ++i)
    PushVertex(handleData_, handles[i], i == 0 ? orange : green);

  boxVertexCount_ = boxData_.size() / 6;
  handleVertexCount_ = handleData_.size() / 6;
  boxGeomDirty_ = true;  // uploaded in the next paintGL (context already current)
}

QPointF TractViewport::WorldToScreen(const Vec3& p) const {
  const float aspect = static_cast<float>(width()) / std::max(1, height());
  const Mat4 m = camera_.ViewProj(aspect);
  const float cx = m.m[0] * p.x + m.m[1] * p.y + m.m[2] * p.z + m.m[3];
  const float cy = m.m[4] * p.x + m.m[5] * p.y + m.m[6] * p.z + m.m[7];
  float cw = m.m[12] * p.x + m.m[13] * p.y + m.m[14] * p.z + m.m[15];
  if (std::abs(cw) < 1e-6f) cw = (cw < 0.0f ? -1e-6f : 1e-6f);
  const float ndcx = cx / cw, ndcy = cy / cw;
  return QPointF((ndcx * 0.5f + 0.5f) * width(), (1.0f - (ndcy * 0.5f + 0.5f)) * height());
}

float TractViewport::WorldDeltaAlongAxis(const Vec3& origin, const Vec3& axis,
                                         const QPointF& mouseDelta) const {
  // Project a small world step along `axis` to screen, then read how many of
  // those steps the mouse moved — a stable world-per-pixel along that axis.
  const float eps = std::max(1.0f, 0.05f * camera_.radius);
  const Vec3 a = origin, b = origin + axis * eps;
  // Reject behind-camera samples: WorldToScreen mirrors them (it keeps cw's
  // sign), which would invert the drag near the near plane.
  const float aspect = static_cast<float>(width()) / std::max(1, height());
  const Mat4 m = camera_.ViewProj(aspect);
  const float wa = m.m[12] * a.x + m.m[13] * a.y + m.m[14] * a.z + m.m[15];
  const float wb = m.m[12] * b.x + m.m[13] * b.y + m.m[14] * b.z + m.m[15];
  if (wa <= 1e-4f || wb <= 1e-4f) return 0.0f;
  const QPointF s0 = WorldToScreen(a);
  const QPointF s1 = WorldToScreen(b);
  const QPointF d = s1 - s0;
  const double len2 = d.x() * d.x() + d.y() * d.y();
  if (len2 < 1e-9) return 0.0f;
  const double mult = (mouseDelta.x() * d.x() + mouseDelta.y() * d.y()) / len2;
  return static_cast<float>(mult) * eps;
}

int TractViewport::PickHandle(const QPoint& pos) const {
  if (!hasBox_) return -1;
  const auto handles = HandlePositions();
  const float aspect = static_cast<float>(width()) / std::max(1, height());
  const Mat4 m = camera_.ViewProj(aspect);
  int best = -1;
  double bestD2 = 16.0 * 16.0;  // 16-pixel pick radius
  for (int i = 0; i < static_cast<int>(handles.size()); ++i) {
    const Vec3& p = handles[i];
    const float cw = m.m[12] * p.x + m.m[13] * p.y + m.m[14] * p.z + m.m[15];
    if (cw <= 0.0f) continue;  // behind the camera
    const QPointF s = WorldToScreen(p);
    const double dx = s.x() - pos.x(), dy = s.y() - pos.y();
    const double d2 = dx * dx + dy * dy;
    if (d2 < bestD2) { bestD2 = d2; best = i; }
  }
  return best;
}

void TractViewport::DragSelectionHandle(const QPointF& mouseDelta) {
  const auto handles = HandlePositions();
  if (activeHandle_ == 0) {  // translate the whole box in the view plane
    // Use the camera's pole-stable basis (same as Pan); deriving it from
    // Cross(Forward, worldZ) would degenerate when looking down ±Z (no clamp).
    const Vec3 right = camera_.Right();
    const Vec3 up = camera_.Up();
    const Vec3 c = handles[0];
    const Vec3 wd = right * WorldDeltaAlongAxis(c, right, mouseDelta) +
                    up * WorldDeltaAlongAxis(c, up, mouseDelta);
    boxBounds_.v[0] += wd.x; boxBounds_.v[1] += wd.x;
    boxBounds_.v[2] += wd.y; boxBounds_.v[3] += wd.y;
    boxBounds_.v[4] += wd.z; boxBounds_.v[5] += wd.z;
  } else {  // move one face along its world axis (resize that side)
    const int axis = (activeHandle_ - 1) / 2;
    const int side = (activeHandle_ - 1) % 2;
    const Vec3 axisDir{axis == 0 ? 1.0f : 0.0f, axis == 1 ? 1.0f : 0.0f, axis == 2 ? 1.0f : 0.0f};
    const float wd = WorldDeltaAlongAxis(handles[activeHandle_], axisDir, mouseDelta);
    const int ci = axis * 2 + side;
    boxBounds_.v[ci] += wd;
    const double minSize = 0.5;  // keep min < max
    if (side == 0)
      boxBounds_.v[ci] = std::min(boxBounds_.v[ci], boxBounds_.v[axis * 2 + 1] - minSize);
    else
      boxBounds_.v[ci] = std::max(boxBounds_.v[ci], boxBounds_.v[axis * 2 + 0] + minSize);
  }
  RebuildSelectionGeometry();
}

void TractViewport::resizeGL(int, int) {}  // viewport set per-frame in paintGL

void TractViewport::paintGL() {
  if (lineDirty_) UploadGeometry();
  if (volumeDirty_) UploadVolume();
  if (boxGeomDirty_ && boxVbo_) {
    glBindBuffer(GL_ARRAY_BUFFER, boxVbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<long>(boxData_.size() * sizeof(float)),
                 boxData_.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, handleVbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<long>(handleData_.size() * sizeof(float)),
                 handleData_.data(), GL_DYNAMIC_DRAW);
    boxGeomDirty_ = false;
  }

  const float dpr = static_cast<float>(devicePixelRatio());
  glViewport(0, 0, static_cast<int>(width() * dpr), static_cast<int>(height() * dpr));
  glClearColor(0.035f, 0.035f, 0.04f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  const float aspect = static_cast<float>(width()) / std::max(1, height());
  const Mat4 mvp = camera_.ViewProj(aspect);

  // Opaque streamlines + cage first (write depth).
  glUseProgram(program_);
  glUniformMatrix4fv(mvpLoc_, 1, GL_TRUE, mvp.m);  // row-major -> transpose

  glBindVertexArray(lineVao_);
  glLineWidth(1.4f);
  glDrawArrays(GL_LINES, 0, static_cast<int>(lineVertexCount_));

  glBindVertexArray(cageVao_);
  glLineWidth(2.4f);
  glDrawArrays(GL_LINES, 0, static_cast<int>(cageVertexCount_));

  // Translucent volume slices last: depth-tested (so closer lines occlude them) but
  // depth-write off (so they blend over lines behind them), alpha-blended.
  if (hasVolume_ && showVolume_ && sliceVertexCount_ > 0) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glUseProgram(sliceProgram_);
    glUniformMatrix4fv(sliceMvpLoc_, 1, GL_TRUE, mvp.m);
    glUniformMatrix4fv(sliceVoxToWorldLoc_, 1, GL_TRUE, voxToWorld_.m);
    glUniform3f(sliceInvDimsLoc_, invDims_.x, invDims_.y, invDims_.z);
    glUniform1f(sliceValueMinLoc_, volValueMin_);
    glUniform1f(sliceValueRangeLoc_, volValueRange_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_3D, volTexture_);
    glBindVertexArray(sliceVao_);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<int>(sliceVertexCount_));
    glBindTexture(GL_TEXTURE_3D, 0);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
  }

  // Selection box: cyan wireframe (depth-tested) + handle points drawn on top.
  if (hasBox_ && boxVertexCount_ > 0) {
    glUseProgram(program_);
    glUniformMatrix4fv(mvpLoc_, 1, GL_TRUE, mvp.m);
    glUniform1f(pointSizeLoc_, 0.0f);
    glBindVertexArray(boxVao_);
    glLineWidth(2.0f);
    glDrawArrays(GL_LINES, 0, static_cast<int>(boxVertexCount_));

    glDisable(GL_DEPTH_TEST);  // handles always grabbable, even when occluded
    glUniform1f(pointSizeLoc_, 13.0f);
    glBindVertexArray(handleVao_);
    glDrawArrays(GL_POINTS, 0, static_cast<int>(handleVertexCount_));
    glEnable(GL_DEPTH_TEST);
  }

  glBindVertexArray(0);
}

void TractViewport::mousePressEvent(QMouseEvent* event) {
  lastPos_ = event->pos();
  if (event->button() == Qt::LeftButton) {
    activeHandle_ = PickHandle(event->pos());  // grab a box handle if near one
    if (activeHandle_ < 0) rotating_ = true;    // otherwise orbit the camera
  }
  // Middle (wheel) is mapped too, but macOS/mouse drivers often intercept it
  // before Qt sees it, so right-drag is the reliable pan.
  if (event->button() == Qt::RightButton || event->button() == Qt::MiddleButton) panning_ = true;
}

void TractViewport::mouseMoveEvent(QMouseEvent* event) {
  const QPoint d = event->pos() - lastPos_;
  if (activeHandle_ >= 0) {
    DragSelectionHandle(QPointF(d.x(), d.y()));
    update();
  } else if (rotating_) {
    camera_.Rotate(d.x(), d.y());
    update();
  } else if (panning_) {
    camera_.Pan(d.x(), d.y());
    update();
  }
  lastPos_ = event->pos();
}

void TractViewport::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    rotating_ = false;
    activeHandle_ = -1;
  }
  if (event->button() == Qt::RightButton || event->button() == Qt::MiddleButton) panning_ = false;
}

void TractViewport::wheelEvent(QWheelEvent* event) {
  // Zoom toward the point under the cursor (not just the target).
  const QPointF pos = event->position();  // logical widget pixels
  const float ndcx = 2.0f * static_cast<float>(pos.x()) / std::max(1, width()) - 1.0f;
  const float ndcy = 1.0f - 2.0f * static_cast<float>(pos.y()) / std::max(1, height());
  const float aspect = static_cast<float>(width()) / std::max(1, height());
  camera_.ZoomToCursor(event->angleDelta().y() / 120.0, ndcx, ndcy, aspect);
  update();
}

void TractViewport::keyPressEvent(QKeyEvent* event) {
  // volume toggle (H) is owned by the View-menu QAction shortcut; only camera reset
  // lives here.
  if (event->key() == Qt::Key_R) {
    ResetCamera();
  } else {
    QOpenGLWidget::keyPressEvent(event);
  }
}

}  // namespace tracto
