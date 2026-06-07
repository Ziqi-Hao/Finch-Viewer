#include "tract_viewport.hpp"

#include <rhi/qrhi.h>
#include <QFile>
#include <QKeyEvent>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <utility>

namespace tracto {
namespace {

// UBO layouts. Mirror the std140 blocks in cpp/qt/rhi/{line,point,slice}.vert.
// Sizes are taken from sizeof(struct) when allocating the GPU buffer (not a
// hardcoded 256), guarded by static_asserts so a struct change can't silently
// under-allocate.
struct LineUbo {
  float mvp[16];
};
struct PointUbo {
  float mvp[16];
  float params[4];  // params.x = point size (px)
};
struct SliceUbo {
  float mvp[16];
  float voxToWorld[16];
  float invDims[4];      // xyz = 1/dims
  float valueParams[4];  // x = valueMin, y = valueRange
};
static_assert(sizeof(LineUbo) == 64, "LineUbo std140 size");
static_assert(sizeof(PointUbo) == 80, "PointUbo std140 size");
static_assert(sizeof(SliceUbo) == 160, "SliceUbo std140 size");

QShader LoadShader(const QString& path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    std::fprintf(stderr, "TractViewport: cannot open shader %s\n", qPrintable(path));
    return {};
  }
  return QShader::fromSerialized(f.readAll());
}

void PushVertex(std::vector<float>& out, Vec3 p, std::array<float, 3> rgb) {
  out.insert(out.end(), {p.x, p.y, p.z, rgb[0], rgb[1], rgb[2]});
}

void PushLine(std::vector<float>& out, Vec3 a, Vec3 b, std::array<float, 3> rgb) {
  PushVertex(out, a, rgb);
  PushVertex(out, b, rgb);
}

// 12 edges of an axis-aligned box from its Bounds, as colored GL-line vertices.
void BuildBoxEdges(std::vector<float>& out, const Bounds& box, std::array<float, 3> rgb) {
  const Vec3 lo{static_cast<float>(box.v[0]), static_cast<float>(box.v[2]),
                static_cast<float>(box.v[4])};
  const Vec3 hi{static_cast<float>(box.v[1]), static_cast<float>(box.v[3]),
                static_cast<float>(box.v[5])};
  const Vec3 c[8] = {
      {lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z},
      {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z},
  };
  const int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                            {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
  out.clear();
  for (const auto& e : edges) PushLine(out, c[e[0]], c[e[1]], rgb);
}

}  // namespace

TractViewport::TractViewport(QWidget* parent) : QRhiWidget(parent) {
  setApi(QRhiWidget::Api::Metal);  // RHI sets the backend on the widget (no QSurfaceFormat)
  setSampleCount(4);               // 4x MSAA to match the old GL viewport's look
  setFocusPolicy(Qt::StrongFocus); // needed for keyPressEvent (reset camera)
}

TractViewport::~TractViewport() {
  // QRhiWidget calls releaseResources() on teardown; ReleaseAll() is idempotent.
  ReleaseAll();
}

// ── Public setters: CPU staging only; the GPU upload is deferred to render() ──

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
  update();  // schedule a repaint; the upload happens in render()
}

void TractViewport::ResetCamera() {
  camera_.Frame(bounds_);
  update();
}

void TractViewport::SetVolume(Volume volume) {
  if (volume.Empty()) return;
  // If no streamlines frame the view yet, frame on the volume instead. Test the
  // staged line data, not lineVertexCount_ (which is only set at upload).
  const bool frameOnVolume = lineData_.empty();
  if (frameOnVolume) {
    bounds_ = WorldBounds(volume);
    camera_.Frame(bounds_);
    RebuildCage();
  }
  pendingVolume_ = std::move(volume);  // move: no multi-MB voxel copy
  volumeDirty_ = true;
  hasVolume_ = true;
  update();
}

void TractViewport::SetVolumeVisible(bool visible) {
  showVolume_ = visible;
  update();
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

// ── CPU geometry builders (no GPU work; flagged for upload in render) ─────────

void TractViewport::RebuildCage() {
  const std::array<float, 3> yellow{1.0f, 0.86f, 0.0f};
  BuildBoxEdges(cageData_, bounds_, yellow);
  cageDirty_ = true;
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
  const std::array<float, 3> cyan{0.1f, 0.9f, 1.0f};
  BuildBoxEdges(boxData_, boxBounds_, cyan);

  handleData_.clear();
  const auto handles = HandlePositions();
  const std::array<float, 3> orange{1.0f, 0.55f, 0.1f};  // center (move)
  const std::array<float, 3> green{0.2f, 1.0f, 0.6f};    // faces (resize)
  for (std::size_t i = 0; i < handles.size(); ++i)
    PushVertex(handleData_, handles[i], i == 0 ? orange : green);

  boxDirty_ = true;  // re-upload in the next render()
}

// ── Renderer-agnostic camera math (unchanged from the GL viewport) ───────────
// WorldToScreen uses the widget's logical pixel size (width()/height()) — these
// are device-independent, exactly what PickHandle's mouse coords are in — so it
// is independent of the GL/RHI backend and needed no change.

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

// ── RHI lifecycle ────────────────────────────────────────────────────────────

void TractViewport::initialize(QRhiCommandBuffer*) {
  // A new (or replaced) rhi device means every resource is gone — drop and
  // rebuild. ReleaseAll() also resets the "uploaded" flags so the re-init
  // re-stages the geometry/texture (device-loss correctness).
  if (rhi_ != rhi()) {
    ReleaseAll();
    rhi_ = rhi();
  }
  if (!linePs_) CreateResources();
}

void TractViewport::CreateResources() {
  const QShader lineVs = LoadShader(QStringLiteral(":/shaders/line.vert.qsb"));
  const QShader lineFs = LoadShader(QStringLiteral(":/shaders/line.frag.qsb"));
  const QShader pointVs = LoadShader(QStringLiteral(":/shaders/point.vert.qsb"));
  const QShader sliceVs = LoadShader(QStringLiteral(":/shaders/slice.vert.qsb"));
  const QShader sliceFs = LoadShader(QStringLiteral(":/shaders/slice.frag.qsb"));
  if (!lineVs.isValid() || !lineFs.isValid() || !pointVs.isValid() ||
      !sliceVs.isValid() || !sliceFs.isValid()) {
    std::fprintf(stderr, "TractViewport: shader load failed — aborting resource creation\n");
    return;  // linePs_ stays null; render() short-circuits, nothing crashes
  }

  auto check = [](QRhiResource* r, const char* what) {
    if (!r) std::fprintf(stderr, "TractViewport: create failed for %s\n", what);
    return r != nullptr;
  };
  auto newUbo = [&](const char* what, quint32 sz) -> QRhiBuffer* {
    QRhiBuffer* b = rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sz);
    if (!b->create()) { check(nullptr, what); delete b; return nullptr; }
    return b;
  };

  lineUbo_ = newUbo("lineUbo", sizeof(LineUbo));
  pointUbo_ = newUbo("pointUbo", sizeof(PointUbo));
  sliceUbo_ = newUbo("sliceUbo", sizeof(SliceUbo));
  if (!lineUbo_ || !pointUbo_ || !sliceUbo_) return;

  // The 3D-texture sampler (used by the slice pipeline; created up front so the
  // SRB can reference it even before a volume is loaded).
  sampler_ = rhi_->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                              QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge,
                              QRhiSampler::ClampToEdge);
  if (!sampler_->create()) { check(nullptr, "sampler"); return; }

  // R32F 3D texture, sized 1^3 as a placeholder until the first SetVolume so the
  // slice SRB/pipeline are valid even with no volume. Re-created at real dims in
  // render() when volumeDirty_. (R32F = 2x VRAM vs the old GL R16F, but it is the
  // Metal-safe single-channel float format under RHI.)
  volTex_ = rhi_->newTexture(QRhiTexture::R32F, 1, 1, 1, 1, QRhiTexture::ThreeDimensional);
  if (!volTex_->create()) { check(nullptr, "volTex"); return; }

  using SRB = QRhiShaderResourceBinding;

  // Shared vertex layout for line/cage/box/handles: interleaved [pos.xyz, rgb].
  QRhiVertexInputLayout colorLayout;
  colorLayout.setBindings({QRhiVertexInputBinding(6 * sizeof(float))});
  colorLayout.setAttributes(
      {QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, 0),
       QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float3, 3 * sizeof(float))});

  const int samples = renderTarget()->sampleCount();
  QRhiRenderPassDescriptor* rp = renderTarget()->renderPassDescriptor();

  // Line/cage/box pipeline (Lines, opaque, depth test + write).
  lineSrb_ = rhi_->newShaderResourceBindings();
  lineSrb_->setBindings({SRB::uniformBuffer(0, SRB::VertexStage, lineUbo_)});
  if (!lineSrb_->create()) { check(nullptr, "lineSrb"); return; }
  linePs_ = rhi_->newGraphicsPipeline();
  linePs_->setShaderStages({{QRhiShaderStage::Vertex, lineVs}, {QRhiShaderStage::Fragment, lineFs}});
  linePs_->setVertexInputLayout(colorLayout);
  linePs_->setShaderResourceBindings(lineSrb_);
  linePs_->setRenderPassDescriptor(rp);
  linePs_->setSampleCount(samples);
  linePs_->setTopology(QRhiGraphicsPipeline::Lines);
  linePs_->setDepthTest(true);
  linePs_->setDepthWrite(true);
  if (!linePs_->create()) { check(nullptr, "linePs"); delete linePs_; linePs_ = nullptr; return; }

  // Handle-points pipeline (Points; gl_PointSize from point.vert; no depth so
  // handles are always grabbable, matching the old glDisable(GL_DEPTH_TEST)).
  pointSrb_ = rhi_->newShaderResourceBindings();
  pointSrb_->setBindings({SRB::uniformBuffer(0, SRB::VertexStage, pointUbo_)});
  if (!pointSrb_->create()) { check(nullptr, "pointSrb"); return; }
  pointPs_ = rhi_->newGraphicsPipeline();
  pointPs_->setShaderStages({{QRhiShaderStage::Vertex, pointVs}, {QRhiShaderStage::Fragment, lineFs}});
  pointPs_->setVertexInputLayout(colorLayout);
  pointPs_->setShaderResourceBindings(pointSrb_);
  pointPs_->setRenderPassDescriptor(rp);
  pointPs_->setSampleCount(samples);
  pointPs_->setTopology(QRhiGraphicsPipeline::Points);
  pointPs_->setDepthTest(false);
  pointPs_->setDepthWrite(false);
  if (!pointPs_->create()) { check(nullptr, "pointPs"); delete pointPs_; pointPs_ = nullptr; return; }

  // FA slice pipeline (Triangles; 3D texture; alpha blend; depth test, no write
  // so closer lines occlude slices but slices still blend over lines behind).
  sliceSrb_ = rhi_->newShaderResourceBindings();
  sliceSrb_->setBindings(
      {SRB::uniformBuffer(0, SRB::VertexStage | SRB::FragmentStage, sliceUbo_),
       SRB::sampledTexture(1, SRB::FragmentStage, volTex_, sampler_)});
  if (!sliceSrb_->create()) { check(nullptr, "sliceSrb"); return; }
  slicePs_ = rhi_->newGraphicsPipeline();
  slicePs_->setShaderStages({{QRhiShaderStage::Vertex, sliceVs}, {QRhiShaderStage::Fragment, sliceFs}});
  QRhiVertexInputLayout sliceLayout;
  sliceLayout.setBindings({QRhiVertexInputBinding(3 * sizeof(float))});
  sliceLayout.setAttributes({QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, 0)});
  slicePs_->setVertexInputLayout(sliceLayout);
  slicePs_->setShaderResourceBindings(sliceSrb_);
  slicePs_->setRenderPassDescriptor(rp);
  slicePs_->setSampleCount(samples);
  slicePs_->setTopology(QRhiGraphicsPipeline::Triangles);
  slicePs_->setCullMode(QRhiGraphicsPipeline::None);
  slicePs_->setDepthTest(true);
  slicePs_->setDepthWrite(false);
  QRhiGraphicsPipeline::TargetBlend tb;
  tb.enable = true;
  tb.srcColor = QRhiGraphicsPipeline::SrcAlpha;
  tb.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
  tb.srcAlpha = QRhiGraphicsPipeline::One;
  tb.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
  slicePs_->setTargetBlends({tb});
  if (!slicePs_->create()) { check(nullptr, "slicePs"); delete slicePs_; slicePs_ = nullptr; return; }

  // Force a re-upload of any geometry already staged before the device existed.
  lineDirty_ = !lineData_.empty();
  cageDirty_ = !cageData_.empty();
  boxDirty_ = hasBox_;
  if (hasVolume_) volumeDirty_ = true;
}

void TractViewport::ReleaseAll() {
  for (QRhiResource** r : {
           reinterpret_cast<QRhiResource**>(&slicePs_),
           reinterpret_cast<QRhiResource**>(&sliceSrb_),
           reinterpret_cast<QRhiResource**>(&sampler_),
           reinterpret_cast<QRhiResource**>(&volTex_),
           reinterpret_cast<QRhiResource**>(&sliceUbo_),
           reinterpret_cast<QRhiResource**>(&sliceVbo_),
           reinterpret_cast<QRhiResource**>(&pointPs_),
           reinterpret_cast<QRhiResource**>(&pointSrb_),
           reinterpret_cast<QRhiResource**>(&pointUbo_),
           reinterpret_cast<QRhiResource**>(&linePs_),
           reinterpret_cast<QRhiResource**>(&lineSrb_),
           reinterpret_cast<QRhiResource**>(&lineUbo_),
           reinterpret_cast<QRhiResource**>(&handleVbo_),
           reinterpret_cast<QRhiResource**>(&boxVbo_),
           reinterpret_cast<QRhiResource**>(&cageVbo_),
           reinterpret_cast<QRhiResource**>(&lineVbo_),
       }) {
    if (*r) { (*r)->destroy(); delete *r; *r = nullptr; }
  }
  // Bug fix: reset the upload/capacity state so a device-loss re-init re-uploads
  // everything instead of skipping it because the old flags said "done".
  lineVboCap_ = cageVboCap_ = boxVboCap_ = handleVboCap_ = sliceVboCap_ = 0;
  lineVertexCount_ = cageVertexCount_ = boxVertexCount_ = handleVertexCount_ = sliceVertexCount_ = 0;
  volTexUploaded_ = false;
  lineDirty_ = !lineData_.empty();
  cageDirty_ = !cageData_.empty();
  boxDirty_ = hasBox_;
  if (hasVolume_) volumeDirty_ = true;
}

void TractViewport::releaseResources() { ReleaseAll(); rhi_ = nullptr; }

// ── Per-frame upload + draw ───────────────────────────────────────────────────

void TractViewport::render(QRhiCommandBuffer* cb) {
  if (!linePs_) return;  // resource creation failed (e.g. missing shaders)

  QRhiRenderTarget* rt = renderTarget();
  const QSize px = rt->pixelSize();
  QRhiResourceUpdateBatch* u = rhi_->nextResourceUpdateBatch();

  // (Re)create + (re)fill a vertex buffer only when the staged data changed or
  // outgrew its capacity. Immutable + uploadStaticBuffer matches the old
  // GL_STATIC_DRAW; growth recreates (cheap relative to the upload itself).
  auto syncVbo = [&](QRhiBuffer*& buf, std::size_t& cap, std::size_t& vtxCount,
                     const std::vector<float>& data, int floatsPerVertex, bool& dirty) {
    if (!dirty) return;
    const std::size_t bytes = data.size() * sizeof(float);
    if (!buf || cap < data.size()) {
      if (buf) { buf->destroy(); delete buf; buf = nullptr; }
      cap = std::max<std::size_t>(data.size(), 1);
      // Static (not Immutable): this VBO is re-uploaded whenever the staged
      // geometry changes (new tractogram, density/step change, delete/keep).
      // Immutable forbids re-upload.
      buf = rhi_->newBuffer(QRhiBuffer::Static, QRhiBuffer::VertexBuffer,
                            static_cast<quint32>(cap * sizeof(float)));
      if (!buf->create()) {
        std::fprintf(stderr, "TractViewport: vertex buffer create failed\n");
        delete buf; buf = nullptr; cap = 0; vtxCount = 0; dirty = false; return;
      }
    }
    // Upload EXACTLY `bytes`, not buf->size(): the buffer is kept at its high-water
    // `cap` and reused when the data shrinks, so the no-size overload (which copies
    // buf->size()) would read past the end of `data` -> SIGSEGV on step/density change.
    if (bytes > 0) u->uploadStaticBuffer(buf, 0, static_cast<quint32>(bytes), data.data());
    vtxCount = static_cast<std::size_t>(data.size() / floatsPerVertex);
    dirty = false;
  };

  syncVbo(lineVbo_, lineVboCap_, lineVertexCount_, lineData_, 6, lineDirty_);
  syncVbo(cageVbo_, cageVboCap_, cageVertexCount_, cageData_, 6, cageDirty_);
  // Box wireframe and handles are rebuilt together (RebuildSelectionGeometry
  // sets boxDirty_ once for both), so drive both uploads off one captured flag.
  bool boxDirty = boxDirty_;
  bool handleDirty = boxDirty_;
  syncVbo(boxVbo_, boxVboCap_, boxVertexCount_, boxData_, 6, boxDirty);
  syncVbo(handleVbo_, handleVboCap_, handleVertexCount_, handleData_, 6, handleDirty);
  boxDirty_ = false;

  // Volume: (re)size the 3D texture to the volume dims and upload it slice-by-z,
  // then build the three mid-index slice quads.
  if (volumeDirty_) {
    const Volume& v = pendingVolume_;
    voxToWorld_ = v.voxelToWorld;
    invDims_ = {1.0f / v.dims[0], 1.0f / v.dims[1], 1.0f / v.dims[2]};
    volValueMin_ = v.valueMin;
    volValueRange_ = std::max(1e-6f, v.valueMax - v.valueMin);
    const int nx = v.dims[0], ny = v.dims[1], nz = v.dims[2];

    const bool dimsChanged = nx != volDims_[0] || ny != volDims_[1] || nz != volDims_[2];
    if (dimsChanged || !volTex_) {
      // Re-create the texture at the real dims, then rebuild the SRB to point at
      // the new texture (an SRB caches the resource handle it was created with).
      if (volTex_) { volTex_->destroy(); delete volTex_; volTex_ = nullptr; }
      volTex_ = rhi_->newTexture(QRhiTexture::R32F, nx, ny, nz, 1, QRhiTexture::ThreeDimensional);
      if (!volTex_->create()) {
        std::fprintf(stderr, "TractViewport: 3D texture create failed\n");
      } else {
        using SRB = QRhiShaderResourceBinding;
        sliceSrb_->setBindings(
            {SRB::uniformBuffer(0, SRB::VertexStage | SRB::FragmentStage, sliceUbo_),
             SRB::sampledTexture(1, SRB::FragmentStage, volTex_, sampler_)});
        sliceSrb_->create();  // update() would also work; recreate is fine pre-frame
      }
      volDims_[0] = nx; volDims_[1] = ny; volDims_[2] = nz;
    }

    if (volTex_) {
      // 3D upload: one entry per z-slice; setDataStride gives the row pitch (x
      // fastest). Mirrors tract_viewer_rhi.cpp's per-slice upload pattern.
      std::vector<QRhiTextureUploadEntry> entries;
      entries.reserve(nz);
      for (int z = 0; z < nz; ++z) {
        QRhiTextureSubresourceUploadDescription sub(
            v.data.data() + static_cast<std::size_t>(z) * nx * ny,
            static_cast<quint32>(nx) * ny * sizeof(float));
        sub.setDataStride(static_cast<quint32>(nx) * sizeof(float));
        entries.emplace_back(z, 0, sub);
      }
      QRhiTextureUploadDescription desc;
      desc.setEntries(entries.begin(), entries.end());
      u->uploadTexture(volTex_, desc);
      volTexUploaded_ = true;
    }

    // Three mid-index slice quads (sagittal/coronal/axial) in voxel coordinates.
    const float mx = static_cast<float>(nx / 2), my = static_cast<float>(ny / 2),
                mz = static_cast<float>(nz / 2);
    const float ex = static_cast<float>(nx - 1), ey = static_cast<float>(ny - 1),
                ez = static_cast<float>(nz - 1);
    auto quad = [](std::vector<float>& out, Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
      const Vec3 q[6] = {a, b, c, a, c, d};
      for (const Vec3& p : q) out.insert(out.end(), {p.x, p.y, p.z});
    };
    sliceData_.clear();
    quad(sliceData_, {mx, 0, 0}, {mx, ey, 0}, {mx, ey, ez}, {mx, 0, ez});  // X = mid
    quad(sliceData_, {0, my, 0}, {ex, my, 0}, {ex, my, ez}, {0, my, ez});  // Y = mid
    quad(sliceData_, {0, 0, mz}, {ex, 0, mz}, {ex, ey, mz}, {0, ey, mz});  // Z = mid

    pendingVolume_.data.clear();   // free the CPU copy; the texture owns it now
    pendingVolume_.data.shrink_to_fit();
    volumeDirty_ = false;
    bool sliceDirty = true;
    syncVbo(sliceVbo_, sliceVboCap_, sliceVertexCount_, sliceData_, 3, sliceDirty);
  }

  // Per-frame camera matrices. OrbitCamera.ViewProj is ROW-MAJOR; left-multiply
  // by clipSpaceCorrMatrix() (RHI clip-space depth/Y fix) and memcpy the
  // column-major constData() into the UBO — exactly as tract_viewer_rhi.cpp.
  const float aspect = px.height() ? static_cast<float>(px.width()) / px.height() : 1.0f;
  const QMatrix4x4 mvp = rhi_->clipSpaceCorrMatrix() * QMatrix4x4(camera_.ViewProj(aspect).m);
  LineUbo lu{};
  std::memcpy(lu.mvp, mvp.constData(), sizeof(lu.mvp));
  u->updateDynamicBuffer(lineUbo_, 0, sizeof(lu), &lu);

  PointUbo pu{};
  std::memcpy(pu.mvp, mvp.constData(), sizeof(pu.mvp));
  pu.params[0] = 13.0f;  // handle point size (px); matches the old gl_PointSize
  u->updateDynamicBuffer(pointUbo_, 0, sizeof(pu), &pu);

  if (hasVolume_ && showVolume_ && sliceVertexCount_ > 0) {
    SliceUbo su{};
    std::memcpy(su.mvp, mvp.constData(), sizeof(su.mvp));
    const QMatrix4x4 v2w = QMatrix4x4(voxToWorld_.m);  // row-major -> Qt transposes on store
    std::memcpy(su.voxToWorld, v2w.constData(), sizeof(su.voxToWorld));
    su.invDims[0] = invDims_.x; su.invDims[1] = invDims_.y; su.invDims[2] = invDims_.z;
    su.valueParams[0] = volValueMin_; su.valueParams[1] = volValueRange_;
    u->updateDynamicBuffer(sliceUbo_, 0, sizeof(su), &su);
  }

  // Clear color matches the old GL viewport (0.035, 0.035, 0.04).
  cb->beginPass(rt, QColor::fromRgbF(0.035, 0.035, 0.04), {1.0f, 0}, u);

  // Opaque streamlines + yellow cage first (write depth).
  cb->setGraphicsPipeline(linePs_);
  cb->setViewport(QRhiViewport(0, 0, px.width(), px.height()));
  cb->setShaderResources(lineSrb_);
  if (lineVbo_ && lineVertexCount_ > 0) {
    QRhiCommandBuffer::VertexInput vin(lineVbo_, 0);
    cb->setVertexInput(0, 1, &vin);
    cb->draw(static_cast<quint32>(lineVertexCount_));
  }
  if (cageVbo_ && cageVertexCount_ > 0) {
    QRhiCommandBuffer::VertexInput vin(cageVbo_, 0);
    cb->setVertexInput(0, 1, &vin);
    cb->draw(static_cast<quint32>(cageVertexCount_));
  }

  // Translucent FA slices (depth-tested, depth-write off, alpha blended).
  if (hasVolume_ && showVolume_ && sliceVbo_ && sliceVertexCount_ > 0 && slicePs_) {
    cb->setGraphicsPipeline(slicePs_);
    cb->setShaderResources(sliceSrb_);
    QRhiCommandBuffer::VertexInput vin(sliceVbo_, 0);
    cb->setVertexInput(0, 1, &vin);
    cb->draw(static_cast<quint32>(sliceVertexCount_));
  }

  // Selection box: cyan wireframe (line pipeline, depth-tested) + handle points
  // drawn on top with the no-depth point pipeline (always grabbable).
  if (hasBox_) {
    if (boxVbo_ && boxVertexCount_ > 0) {
      cb->setGraphicsPipeline(linePs_);
      cb->setShaderResources(lineSrb_);
      QRhiCommandBuffer::VertexInput vin(boxVbo_, 0);
      cb->setVertexInput(0, 1, &vin);
      cb->draw(static_cast<quint32>(boxVertexCount_));
    }
    if (handleVbo_ && handleVertexCount_ > 0) {
      cb->setGraphicsPipeline(pointPs_);
      cb->setShaderResources(pointSrb_);
      QRhiCommandBuffer::VertexInput vin(handleVbo_, 0);
      cb->setVertexInput(0, 1, &vin);
      cb->draw(static_cast<quint32>(handleVertexCount_));
    }
  }

  cb->endPass();
}

// ── Interaction (renderer-agnostic; unchanged from the GL viewport) ───────────

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
    QRhiWidget::keyPressEvent(event);
  }
}

}  // namespace tracto
