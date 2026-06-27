#include "tract_viewport.hpp"

#include "orientation_overlay.hpp"

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
struct GlyphUbo {
  float mvp[16];
  float lightDir[4];  // xyz = world-space directional light; w unused
};
static_assert(sizeof(LineUbo) == 64, "LineUbo std140 size");
static_assert(sizeof(PointUbo) == 80, "PointUbo std140 size");
static_assert(sizeof(SliceUbo) == 160, "SliceUbo std140 size");
static_assert(sizeof(GlyphUbo) == 80, "GlyphUbo std140 size");

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
  // Pick the RHI backend per platform (the widget owns it — no QSurfaceFormat).
  // macOS system GL is frozen at 4.1 so Metal is the only modern path there; on
  // Windows D3D11 is the universal default; elsewhere (Linux) OpenGL is the most
  // broadly available (the shipped editor needs no compute, so GL is fine).
#if defined(Q_OS_MACOS)
  setApi(QRhiWidget::Api::Metal);
#elif defined(Q_OS_WIN)
  setApi(QRhiWidget::Api::Direct3D11);
#else
  setApi(QRhiWidget::Api::OpenGL);
#endif
  setSampleCount(4);               // 4x MSAA to match the old GL viewport's look
  setFocusPolicy(Qt::StrongFocus); // needed for keyPressEvent (reset camera)
  setMouseTracking(true);          // hover updates hoverPane_ for arrow-key slice scrub
  highlightClock_.start();         // monotonic clock for throttling the live highlight
  orientOverlay_ = new OrientationOverlay(this);  // L/R·A/P·S/I labels, drawn over the slices
}

TractViewport::~TractViewport() {
  // QRhiWidget calls releaseResources() on teardown; ReleaseAll() is idempotent.
  ReleaseAll();
}

// ── Public setters: CPU staging only; the GPU upload is deferred to render() ──

void TractViewport::SetLineGeometry(std::vector<float> interleaved, std::vector<DisplaySpan> spans,
                                    const Bounds& bounds) {
  // Geometry only — does NOT re-frame the camera, so an edit (which rebuilds the
  // display) preserves the user's view. Camera framing is driven explicitly by
  // MainWindow::ResetCamera on new-data load. The selection box is placed once
  // (first geometry) and then persists across edits.
  lineData_ = std::move(interleaved);
  displaySpans_ = std::move(spans);
  bounds_ = bounds;
  lineDirty_ = true;
  RebuildCage();
  if (!hasBox_) PlaceSelectionBoxInBounds(0.6);
  UpdateHighlight();  // geometry/spans changed -> recompute the in-box overlay
  update();           // schedule a repaint; the upload happens in render()
}

void TractViewport::SetTractOverlays(std::vector<std::vector<float>> overlays) {
  // Free GPU buffers for slots that disappear, then move the new data in. Each
  // retained/new slot is re-uploaded in render() via its dirty flag.
  for (std::size_t i = overlays.size(); i < overlays_.size(); ++i)
    if (overlays_[i].vbo) { overlays_[i].vbo->destroy(); delete overlays_[i].vbo; overlays_[i].vbo = nullptr; }
  overlays_.resize(overlays.size());
  for (std::size_t i = 0; i < overlays.size(); ++i) {
    overlays_[i].data = std::move(overlays[i]);
    overlays_[i].dirty = true;
  }
  update();
}

void TractViewport::SetGlyphGeometry(std::vector<float> interleaved,
                                     std::vector<std::uint32_t> indices,
                                     const Bounds& bounds) {
  // Geometry only — like SetLineGeometry it does NOT re-frame the camera (the
  // MainWindow calls ResetCamera explicitly on a fresh load). Empty input clears
  // the layer. The upload is deferred to render() via glyphDirty_, so this is safe
  // before the rhi device exists.
  glyphData_ = std::move(interleaved);
  glyphIndices_ = std::move(indices);
  glyphBounds_ = bounds;
  hasGlyphs_ = !glyphData_.empty() && !glyphIndices_.empty();
  glyphDirty_ = true;
  update();
}

void TractViewport::SetGlyphsVisible(bool visible) {
  showGlyphs_ = visible;
  update();
}

void TractViewport::SetPeaksGeometry(std::vector<float> interleaved, const Bounds& bounds) {
  // [pos.xyz, rgb] line segments, same layout as the streamlines — drawn with the
  // line pipeline. Geometry only (no camera re-frame); empty input clears the layer.
  peaksData_ = std::move(interleaved);
  peaksBounds_ = bounds;
  hasPeaks_ = !peaksData_.empty();
  peaksDirty_ = true;
  update();
}

void TractViewport::SetPeaksVisible(bool visible) {
  showPeaks_ = visible;
  update();
}

void TractViewport::SetSelectionQuery(std::function<std::vector<uint8_t>(const Bounds&)> query) {
  selectionQuery_ = std::move(query);
  UpdateHighlight();
}

void TractViewport::UpdateHighlight() {
  // White overlay of the displayed streamlines inside the box: gather their
  // vertices from lineData_ (via the spans) and recolour white. Cleared when
  // there's no box/query. Throttled by the caller during a drag.
  highlightData_.clear();
  const std::size_t totalVerts = lineData_.size() / 6;
  if (editMode_ && selectionQuery_ && hasBox_ && !displaySpans_.empty() && totalVerts > 0) {
    const std::vector<uint8_t> inBox = selectionQuery_(boxBounds_);
    for (const DisplaySpan& sp : displaySpans_) {
      if (sp.fullId < 0 || static_cast<std::size_t>(sp.fullId) >= inBox.size()) continue;
      if (!inBox[static_cast<std::size_t>(sp.fullId)]) continue;
      const std::size_t end = std::min<std::size_t>(totalVerts, sp.firstVertex + sp.vertexCount);
      for (std::size_t v = sp.firstVertex; v < end; ++v) {
        const std::size_t base = v * 6;
        highlightData_.insert(highlightData_.end(), {lineData_[base], lineData_[base + 1],
                                                     lineData_[base + 2], 1.0f, 1.0f, 1.0f});
      }
    }
  }
  highlightDirty_ = true;
  update();
}

const Bounds& TractViewport::FramingBounds() const {
  // Priority: a background volume (its extent drives the view), else the
  // streamlines, else an ODF-only glyph or peaks-only scene.
  return hasVolumeBounds_      ? volumeBounds_
         : !lineData_.empty()  ? bounds_
         : hasGlyphs_          ? glyphBounds_
         : hasPeaks_           ? peaksBounds_
                               : bounds_;
}

void TractViewport::ResetCamera() {
  // Global reset: re-frame the 3-D orbit AND re-centre the shared slice focus +
  // re-fit all three ortho panes (the focusInit block in render() does the latter).
  camera_.Frame(FramingBounds());
  focusInit_ = false;
  update();
}

void TractViewport::ResetView(int pane) {
  // Per-pane reset: re-frame just one pane's camera, leaving the others and the
  // shared slice position alone. pane<0 means "no specific pane" -> reset everything.
  if (pane < 0) { ResetCamera(); return; }
  const Bounds& fb = FramingBounds();
  const int axis = PaneAxis(pane);
  if (axis < 0) camera_.Frame(fb);          // 3-D orbit pane
  else ortho_[axis].Frame(fb, axis);        // one ortho pane's pan + zoom
  update();
}

void TractViewport::ResetHoveredView() { ResetView(hoverPane_); }

void TractViewport::SetSliceFocusZ(float z) {
  focus_.z = z;
  focusInit_ = true;      // lock the focus so render()'s lazy re-centre won't override it
  focusDirty_ = true;     // slice quads follow
  emit sliceFocusChanged();
  update();
}

QString TractViewport::RendererName() const {
  if (!rhi()) return {};
  const QString name = QString::fromUtf8(rhi()->driverInfo().deviceName);
  const QString backend = QString::fromUtf8(rhi()->backendName());
  return name.isEmpty() ? backend : QStringLiteral("%1 · %2").arg(name, backend);
}

TractViewport::ImageSlot* TractViewport::FindImage(int id) {
  for (ImageSlot& s : images_) if (s.id == id) return &s;
  return nullptr;
}

void TractViewport::RecomputeDrawOrder() {
  // Blend order (bottom -> top), stable within each tier: base anatomy volumes
  // first, then labels, then heatmaps (track density), then stat overlays (fMRI)
  // on top so the signed activation reads over whatever anatomy/labels are loaded.
  drawOrder_.clear();
  for (int i = 0; i < static_cast<int>(images_.size()); ++i)
    if (!images_[i].isLabel && !images_[i].heatmap && !images_[i].statmap) drawOrder_.push_back(i);
  for (int i = 0; i < static_cast<int>(images_.size()); ++i)
    if (images_[i].isLabel) drawOrder_.push_back(i);
  for (int i = 0; i < static_cast<int>(images_.size()); ++i)
    if (images_[i].heatmap) drawOrder_.push_back(i);
  for (int i = 0; i < static_cast<int>(images_.size()); ++i)
    if (images_[i].statmap) drawOrder_.push_back(i);
}

void TractViewport::RecomputeVolumeBounds() {
  // The primary image (first non-label, else first loaded) is the framing/scrub
  // reference: its extent frames the camera, its voxel size is the arrow-key step.
  if (images_.empty()) { hasVolumeBounds_ = false; return; }
  // Prefer a real anatomical volume (non-label, non-heatmap, non-statmap) as the
  // framing/scrub reference; the density heatmap and stat activation map are derived
  // overlays, not the anatomy.
  const ImageSlot* p = &images_.front();
  for (const ImageSlot& s : images_)
    if (!s.isLabel && !s.heatmap && !s.statmap) { p = &s; break; }  // anatomy, not an overlay
  volumeBounds_ = WorldBounds(p->vol);
  hasVolumeBounds_ = true;
  for (int a = 0; a < 3; ++a) sliceScrubStep_[a] = p->voxelSpacing[a];
}

void TractViewport::SetImage(int id, Volume vol, bool isLabel,
                             std::vector<float> lut, int lutWidth) {
  if (vol.Empty()) return;
  ImageSlot* s = FindImage(id);
  const bool isFirst = images_.empty();
  if (!s) { images_.emplace_back(); s = &images_.back(); s->id = id; }

  s->isLabel = isLabel;
  s->lut = std::move(lut);
  s->lutWidth = std::max(1, lutWidth);
  s->valueMin = vol.valueMin;
  s->valueRange = std::max(1.0e-6f, vol.valueMax - vol.valueMin);
  s->voxToWorld = vol.voxelToWorld;
  s->worldToVox = InverseAffine(s->voxToWorld);
  for (int a = 0; a < 3; ++a) {
    s->dims[a] = vol.dims[a];
    // world mm per voxel along axis a = length of the affine's a-th column.
    s->voxelSpacing[a] = std::sqrt(s->voxToWorld.m[a] * s->voxToWorld.m[a] +
                                   s->voxToWorld.m[4 + a] * s->voxToWorld.m[4 + a] +
                                   s->voxToWorld.m[8 + a] * s->voxToWorld.m[8 + a]);
  }
  s->invDims = {1.0f / s->dims[0], 1.0f / s->dims[1], 1.0f / s->dims[2]};
  s->vol = std::move(vol);  // retained for RHI resource rebuild/re-upload
  s->texDirty = true;       // (re)create + upload tex (also re-arms lut + quads)

  RecomputeDrawOrder();
  RecomputeVolumeBounds();
  // Frame the camera on the FIRST image only; later loads must not yank the view.
  if (isFirst) {
    camera_.Frame(volumeBounds_);
    focusInit_ = false;          // re-centre focus / re-fit ortho on the volume
    if (lineData_.empty()) { bounds_ = volumeBounds_; RebuildCage(); }
  }
  update();
}

void TractViewport::SetImageParams(int id, float winLo, float winHi, float opacity) {
  ImageSlot* s = FindImage(id);
  if (!s) return;
  s->valueMin = winLo;
  s->valueRange = std::max(1.0e-6f, winHi - winLo);
  s->opacity = std::clamp(opacity, 0.0f, 1.0f);
  update();  // picked up by the per-frame UBO write; no texture re-upload
}

void TractViewport::SetImageVisible(int id, bool visible) {
  if (ImageSlot* s = FindImage(id)) { s->visible = visible; update(); }
}

void TractViewport::SetImageHeatmap(int id, bool on) {
  if (ImageSlot* s = FindImage(id)) {
    s->heatmap = on;
    RecomputeDrawOrder();      // heatmaps blend on top -> re-tier the draw order
    RecomputeVolumeBounds();   // and stop being the framing/scrub reference
    update();
  }
}

void TractViewport::SetImageStatmap(int id, bool on) {
  if (ImageSlot* s = FindImage(id)) {
    s->statmap = on;
    RecomputeDrawOrder();      // stat overlays blend on top -> re-tier the draw order
    RecomputeVolumeBounds();   // and stop being the framing/scrub reference (it's an overlay)
    update();
  }
}

void TractViewport::SetImageViridis(int id, bool on) {
  // Just a colormap choice for a plain scalar volume (mode 0 <-> 4); it stays in
  // the same draw tier and uses the same linear sampler, so only the UBO mode flips.
  if (ImageSlot* s = FindImage(id)) { s->viridis = on; update(); }
}

void TractViewport::SetImageOrthoOnly(int id, bool on) {
  if (ImageSlot* s = FindImage(id)) { s->orthoOnly = on; update(); }
}

void TractViewport::RemoveImage(int id) {
  for (auto it = images_.begin(); it != images_.end(); ++it) {
    if (it->id != id) continue;
    for (QRhiResource* r : {static_cast<QRhiResource*>(it->srb),
                            static_cast<QRhiResource*>(it->sliceVbo),
                            static_cast<QRhiResource*>(it->lutTex),
                            static_cast<QRhiResource*>(it->tex)})
      if (r) { r->destroy(); delete r; }
    images_.erase(it);
    RecomputeDrawOrder();
    RecomputeVolumeBounds();
    update();
    return;
  }
}

void TractViewport::SetVolumeVisible(bool visible) {
  showVolume_ = visible;
  update();
}

void TractViewport::SetTractsVisible(bool visible) {
  showTracts_ = visible;
  update();
}

void TractViewport::RebuildSliceSrb(ImageSlot& s) {
  if (!s.srb || !s.tex || !s.lutTex || !sliceUbo_) return;
  using SRB = QRhiShaderResourceBinding;
  // Labels sample the 3D texture nearest (exact label values); grayscale linear.
  QRhiSampler* volSamp = s.isLabel ? nearestSampler_ : sampler_;
  s.srb->setBindings(
      {SRB::uniformBufferWithDynamicOffset(0, SRB::VertexStage | SRB::FragmentStage, sliceUbo_,
                                           sizeof(SliceUbo)),
       SRB::sampledTexture(1, SRB::FragmentStage, s.tex, volSamp),
       SRB::sampledTexture(2, SRB::FragmentStage, s.lutTex, nearestSampler_)});
  s.srb->create();
}

void TractViewport::SetEditMode(bool on) {
  editMode_ = on;
  UpdateHighlight();  // highlight is an edit-time preview; clears when leaving edit
  update();
}

void TractViewport::ResetSelectionBox() {
  PlaceSelectionBoxInBounds(0.6);
  UpdateHighlight();
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
  UpdateHighlight();
  update();
}

// ── CPU geometry builders (no GPU work; flagged for upload in render) ─────────

void TractViewport::RebuildCage() {
  const std::array<float, 3> yellow{1.0f, 0.86f, 0.0f};
  BuildBoxEdges(cageData_, bounds_, yellow);
  cageDirty_ = true;
}

void TractViewport::RebuildSliceQuads(ImageSlot& s) {
  // Place this image's three slice quads at the shared focus point (world ->
  // voxel index in THIS image's space, clamped). Order X(0..5), Y(6..11),
  // Z(12..17); 6 verts each. Each image has its own affine, so its quads sit at
  // its own voxel coords even though they all pass through the shared focus.
  if (s.dims[0] <= 0) return;
  const float ex = static_cast<float>(s.dims[0] - 1);
  const float ey = static_cast<float>(s.dims[1] - 1);
  const float ez = static_cast<float>(s.dims[2] - 1);
  const Vec3 f = focus_;
  const Mat4& w = s.worldToVox;
  float vx = w.m[0] * f.x + w.m[1] * f.y + w.m[2] * f.z + w.m[3];
  float vy = w.m[4] * f.x + w.m[5] * f.y + w.m[6] * f.z + w.m[7];
  float vz = w.m[8] * f.x + w.m[9] * f.y + w.m[10] * f.z + w.m[11];
  vx = std::clamp(vx, 0.0f, ex);
  vy = std::clamp(vy, 0.0f, ey);
  vz = std::clamp(vz, 0.0f, ez);
  auto quad = [](std::vector<float>& out, Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
    const Vec3 q[6] = {a, b, c, a, c, d};
    for (const Vec3& p : q) out.insert(out.end(), {p.x, p.y, p.z});
  };
  s.sliceData.clear();
  quad(s.sliceData, {vx, 0, 0}, {vx, ey, 0}, {vx, ey, ez}, {vx, 0, ez});  // X @ vx
  quad(s.sliceData, {0, vy, 0}, {ex, vy, 0}, {ex, vy, ez}, {0, vy, ez});  // Y @ vy
  quad(s.sliceData, {0, 0, vz}, {ex, 0, vz}, {ex, ey, vz}, {0, ey, vz});  // Z @ vz
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
  // Project into the 3D pane's rect (where the box is edited), in logical px.
  int rx, ry, rw, rh;
  if (!ThreeDRectLogical(rx, ry, rw, rh)) return QPointF(-1.0e6, -1.0e6);
  const float aspect = static_cast<float>(rw) / std::max(1, rh);
  const Mat4 m = camera_.ViewProj(aspect);
  const float cx = m.m[0] * p.x + m.m[1] * p.y + m.m[2] * p.z + m.m[3];
  const float cy = m.m[4] * p.x + m.m[5] * p.y + m.m[6] * p.z + m.m[7];
  float cw = m.m[12] * p.x + m.m[13] * p.y + m.m[14] * p.z + m.m[15];
  if (std::abs(cw) < 1e-6f) cw = (cw < 0.0f ? -1e-6f : 1e-6f);
  const float ndcx = cx / cw, ndcy = cy / cw;
  return QPointF(rx + (ndcx * 0.5f + 0.5f) * rw, ry + (1.0f - (ndcy * 0.5f + 0.5f)) * rh);
}

float TractViewport::WorldDeltaAlongAxis(const Vec3& origin, const Vec3& axis,
                                         const QPointF& mouseDelta) const {
  // Project a small world step along `axis` to screen, then read how many of
  // those steps the mouse moved — a stable world-per-pixel along that axis.
  const float eps = std::max(1.0f, 0.05f * camera_.radius);
  const Vec3 a = origin, b = origin + axis * eps;
  // Reject behind-camera samples: WorldToScreen mirrors them (it keeps cw's
  // sign), which would invert the drag near the near plane.
  int rx, ry, rw, rh;
  if (!ThreeDRectLogical(rx, ry, rw, rh)) return 0.0f;
  const float aspect = static_cast<float>(rw) / std::max(1, rh);
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
  int rx, ry, rw, rh;
  if (!ThreeDRectLogical(rx, ry, rw, rh)) return -1;  // box editing only in the 3D pane
  const auto handles = HandlePositions();
  const float aspect = static_cast<float>(rw) / std::max(1, rh);
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

int TractViewport::PickSlicePlane(const QPoint& pos) const {
  if (!hasVolumeBounds_) return -1;
  int rx, ry, rw, rh;
  if (!ThreeDRectLogical(rx, ry, rw, rh)) return -1;          // 3D pane only
  if (pos.x() < rx || pos.x() >= rx + rw || pos.y() < ry || pos.y() >= ry + rh) return -1;

  // Unproject the cursor into a world-space ray (GL NDC, matching camera_.ViewProj).
  const float aspect = static_cast<float>(rw) / std::max(1, rh);
  const Mat4 invVp = Inverse(camera_.ViewProj(aspect));
  const float ndcx = 2.0f * static_cast<float>(pos.x() - rx) / std::max(1, rw) - 1.0f;
  const float ndcy = 1.0f - 2.0f * static_cast<float>(pos.y() - ry) / std::max(1, rh);
  auto unproject = [&](float ndcz) {
    const float x = invVp.m[0] * ndcx + invVp.m[1] * ndcy + invVp.m[2] * ndcz + invVp.m[3];
    const float y = invVp.m[4] * ndcx + invVp.m[5] * ndcy + invVp.m[6] * ndcz + invVp.m[7];
    const float z = invVp.m[8] * ndcx + invVp.m[9] * ndcy + invVp.m[10] * ndcz + invVp.m[11];
    const float w = invVp.m[12] * ndcx + invVp.m[13] * ndcy + invVp.m[14] * ndcz + invVp.m[15];
    const float iw = (std::abs(w) > 1e-12f) ? 1.0f / w : 0.0f;
    return Vec3{x * iw, y * iw, z * iw};
  };
  const Vec3 origin = unproject(-1.0f);
  const Vec3 dir = Normalize(unproject(1.0f) - origin);

  auto comp = [](const Vec3& v, int i) { return i == 0 ? v.x : i == 1 ? v.y : v.z; };
  const double* vb = volumeBounds_.v;  // [xmin,xmax, ymin,ymax, zmin,zmax]
  const float foc[3] = {focus_.x, focus_.y, focus_.z};
  int best = -1;
  float bestT = 1e30f;
  for (int a = 0; a < 3; ++a) {  // plane: coord a == focus[a]
    const float da = comp(dir, a);
    if (std::abs(da) < 1e-6f) continue;
    const float t = (foc[a] - comp(origin, a)) / da;
    if (t <= 0.0f || t >= bestT) continue;
    const Vec3 hit = origin + dir * t;
    bool inside = true;  // hit must lie within the volume on the other two axes
    for (int b = 0; b < 3; ++b) {
      if (b == a) continue;
      const float h = comp(hit, b);
      if (h < vb[b * 2] - 1e-3 || h > vb[b * 2 + 1] + 1e-3) { inside = false; break; }
    }
    if (inside) { bestT = t; best = a; }
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

// ── 2x2 multi-view layout (cell order: 0 TL=3D, 1 TR=Axial, 2 BL=Coronal, 3 BR=Sagittal) ──

TractViewport::ViewKind TractViewport::PaneKind(int i) {
  switch (i) {
    case 0: return ViewKind::ThreeD;
    case 1: return ViewKind::Axial;
    case 2: return ViewKind::Coronal;
    default: return ViewKind::Sagittal;
  }
}

int TractViewport::PaneAxis(int i) {
  switch (i) {        // world axis the ortho pane looks down; -1 for the 3D pane
    case 1: return 2;   // Axial    -> Z
    case 2: return 1;   // Coronal  -> Y
    case 3: return 0;   // Sagittal -> X
    default: return -1;
  }
}

void TractViewport::PaneFrac(int i, float& x, float& y, float& w, float& h) const {
  if (maximized_ >= 0) { x = 0.0f; y = 0.0f; w = 1.0f; h = 1.0f; return; }
  x = (i % 2) ? 0.5f : 0.0f;   // cols: 0,2 left · 1,3 right
  y = (i >= 2) ? 0.5f : 0.0f;  // rows: 0,1 top · 2,3 bottom
  w = 0.5f;
  h = 0.5f;
}

std::vector<TractViewport::OrthoPaneInfo> TractViewport::OrthoPaneLayout() const {
  std::vector<OrthoPaneInfo> out;
  // No spatial content -> no labels (keep the empty viewer clean).
  if (images_.empty() && !hasGlyphs_ && !hasPeaks_ && lineData_.empty()) return out;
  const double W = width(), H = height();  // logical pixels = the overlay's QPainter space
  for (int i = 0; i < 4; ++i) {
    const int axis = PaneAxis(i);
    if (axis < 0) continue;                            // skip the 3-D pane (free camera)
    if (maximized_ >= 0 && i != maximized_) continue;  // hidden while another pane is maximized
    float fx, fy, fw, fh;
    PaneFrac(i, fx, fy, fw, fh);
    out.push_back({QRectF(fx * W, fy * H, fw * W, fh * H), axis});
  }
  return out;
}

void TractViewport::PaneRectPx(int i, int W, int H, int& x, int& yTop, int& w, int& h) const {
  float fx, fy, fw, fh;
  PaneFrac(i, fx, fy, fw, fh);
  const int x0 = static_cast<int>(fx * W), x1 = static_cast<int>((fx + fw) * W);
  const int y0 = static_cast<int>(fy * H), y1 = static_cast<int>((fy + fh) * H);
  x = x0; yTop = y0; w = std::max(1, x1 - x0); h = std::max(1, y1 - y0);
}

int TractViewport::PaneAt(const QPoint& p) const {
  const int W = width(), H = height();
  for (int i = 0; i < 4; ++i) {
    if (maximized_ >= 0 && i != maximized_) continue;
    int x, y, w, h;
    PaneRectPx(i, W, H, x, y, w, h);
    if (p.x() >= x && p.x() < x + w && p.y() >= y && p.y() < y + h) return i;
  }
  return -1;
}

bool TractViewport::ThreeDRectLogical(int& x, int& y, int& w, int& h) const {
  if (maximized_ >= 0 && maximized_ != 0) return false;  // 3D pane not visible
  PaneRectPx(0, width(), height(), x, y, w, h);
  return true;
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
  const QShader glyphVs = LoadShader(QStringLiteral(":/shaders/glyph.vert.qsb"));
  const QShader glyphFs = LoadShader(QStringLiteral(":/shaders/glyph.frag.qsb"));
  if (!lineVs.isValid() || !lineFs.isValid() || !pointVs.isValid() ||
      !sliceVs.isValid() || !sliceFs.isValid() || !glyphVs.isValid() || !glyphFs.isValid()) {
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

  // Dynamic-offset UBO blocks. The stride is the block size rounded up to the
  // device's UBO alignment (256 on Metal). line/point hold one block per pane
  // (4); the slice UBO holds one per image×pane (kMaxBlendImages*4) so several
  // image layers blend in one pass, each bound at offset (imageRank*4 + pane).
  auto alignUp = [](quint32 v, quint32 a) { return a ? (v + a - 1) / a * a : v; };
  const quint32 ualign = rhi_->ubufAlignment();
  lineUboStride_ = alignUp(sizeof(LineUbo), ualign);
  pointUboStride_ = alignUp(sizeof(PointUbo), ualign);
  sliceUboStride_ = alignUp(sizeof(SliceUbo), ualign);
  glyphUboStride_ = alignUp(sizeof(GlyphUbo), ualign);
  lineUbo_ = newUbo("lineUbo", 4 * lineUboStride_);
  pointUbo_ = newUbo("pointUbo", 4 * pointUboStride_);
  sliceUbo_ = newUbo("sliceUbo", kMaxBlendImages * 4 * sliceUboStride_);
  glyphUbo_ = newUbo("glyphUbo", 4 * glyphUboStride_);  // one block per pane
  if (!lineUbo_ || !pointUbo_ || !sliceUbo_ || !glyphUbo_) return;

  // The 3D-texture sampler (used by the slice pipeline; created up front so the
  // SRB can reference it even before a volume is loaded).
  sampler_ = rhi_->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                              QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge,
                              QRhiSampler::ClampToEdge);
  if (!sampler_->create()) { check(nullptr, "sampler"); return; }

  // Nearest sampler for label volumes (no interpolation across label values) and
  // for the LUT lookup.
  nearestSampler_ = rhi_->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
                                     QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge,
                                     QRhiSampler::ClampToEdge);
  if (!nearestSampler_->create()) { check(nullptr, "nearestSampler"); return; }

  // 1x1 placeholder textures so the slice pipeline's template SRB is valid with
  // no image loaded. The real per-image R32F volume + RGBA32F LUT textures live
  // in each ImageSlot and are created in render(); these placeholders are bound
  // only by sliceSrb_ (used solely for pipeline creation, never for a draw).
  volTex_ = rhi_->newTexture(QRhiTexture::R32F, 1, 1, 1, 1, QRhiTexture::ThreeDimensional);
  if (!volTex_->create()) { check(nullptr, "volTex"); return; }
  lutTex_ = rhi_->newTexture(QRhiTexture::RGBA32F, QSize(1, 1), 1);
  if (!lutTex_->create()) { check(nullptr, "lutTex"); return; }

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
  lineSrb_->setBindings(
      {SRB::uniformBufferWithDynamicOffset(0, SRB::VertexStage, lineUbo_, sizeof(LineUbo))});
  if (!lineSrb_->create()) { check(nullptr, "lineSrb"); return; }
  linePs_ = rhi_->newGraphicsPipeline();
  linePs_->setFlags(QRhiGraphicsPipeline::UsesScissor);  // per-pane scissor in render()
  linePs_->setShaderStages({{QRhiShaderStage::Vertex, lineVs}, {QRhiShaderStage::Fragment, lineFs}});
  linePs_->setVertexInputLayout(colorLayout);
  linePs_->setShaderResourceBindings(lineSrb_);
  linePs_->setRenderPassDescriptor(rp);
  linePs_->setSampleCount(samples);
  linePs_->setTopology(QRhiGraphicsPipeline::Lines);
  linePs_->setDepthTest(true);
  linePs_->setDepthWrite(true);
  if (!linePs_->create()) { check(nullptr, "linePs"); delete linePs_; linePs_ = nullptr; return; }

  // Selection-highlight pipeline: the white in-box streamlines, drawn on top
  // (Lines, no depth test/write) so the selected bundle pops over everything.
  // Shares lineSrb_ (just the mvp UBO) and the line shaders.
  highlightPs_ = rhi_->newGraphicsPipeline();
  highlightPs_->setFlags(QRhiGraphicsPipeline::UsesScissor);
  highlightPs_->setShaderStages({{QRhiShaderStage::Vertex, lineVs}, {QRhiShaderStage::Fragment, lineFs}});
  highlightPs_->setVertexInputLayout(colorLayout);
  highlightPs_->setShaderResourceBindings(lineSrb_);
  highlightPs_->setRenderPassDescriptor(rp);
  highlightPs_->setSampleCount(samples);
  highlightPs_->setTopology(QRhiGraphicsPipeline::Lines);
  highlightPs_->setDepthTest(false);
  highlightPs_->setDepthWrite(false);
  if (!highlightPs_->create()) {
    check(nullptr, "highlightPs"); delete highlightPs_; highlightPs_ = nullptr; return;
  }

  // Handle-points pipeline (Points; gl_PointSize from point.vert; no depth so
  // handles are always grabbable, matching the old glDisable(GL_DEPTH_TEST)).
  pointSrb_ = rhi_->newShaderResourceBindings();
  pointSrb_->setBindings(
      {SRB::uniformBufferWithDynamicOffset(0, SRB::VertexStage, pointUbo_, sizeof(PointUbo))});
  if (!pointSrb_->create()) { check(nullptr, "pointSrb"); return; }
  pointPs_ = rhi_->newGraphicsPipeline();
  pointPs_->setFlags(QRhiGraphicsPipeline::UsesScissor);
  pointPs_->setShaderStages({{QRhiShaderStage::Vertex, pointVs}, {QRhiShaderStage::Fragment, lineFs}});
  pointPs_->setVertexInputLayout(colorLayout);
  pointPs_->setShaderResourceBindings(pointSrb_);
  pointPs_->setRenderPassDescriptor(rp);
  pointPs_->setSampleCount(samples);
  pointPs_->setTopology(QRhiGraphicsPipeline::Points);
  pointPs_->setDepthTest(false);
  pointPs_->setDepthWrite(false);
  if (!pointPs_->create()) { check(nullptr, "pointPs"); delete pointPs_; pointPs_ = nullptr; return; }

  // Slice pipeline (Triangles; 3D texture; alpha blend; depth test, no write so
  // closer lines occlude slices but slices still blend over lines behind). The
  // pipeline needs ONE layout-compatible SRB at creation; each ImageSlot builds
  // its own SRB (same layout, its own texture) for the actual per-image draws.
  sliceSrb_ = rhi_->newShaderResourceBindings();
  sliceSrb_->setBindings(
      {SRB::uniformBufferWithDynamicOffset(0, SRB::VertexStage | SRB::FragmentStage, sliceUbo_,
                                           sizeof(SliceUbo)),
       SRB::sampledTexture(1, SRB::FragmentStage, volTex_, sampler_),
       SRB::sampledTexture(2, SRB::FragmentStage, lutTex_, nearestSampler_)});
  if (!sliceSrb_->create()) { check(nullptr, "sliceSrb"); return; }
  slicePs_ = rhi_->newGraphicsPipeline();
  slicePs_->setFlags(QRhiGraphicsPipeline::UsesScissor);
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

  // ODF glyph pipeline (Triangles, opaque, depth test + write; two-sided lit in the
  // fragment shader so cull-off interior lobe faces still shade). Vertex layout is
  // interleaved [pos.xyz, normal.xyz, rgb] (9 floats), matching BuildGlyphs's mesh.
  glyphSrb_ = rhi_->newShaderResourceBindings();
  glyphSrb_->setBindings({SRB::uniformBufferWithDynamicOffset(
      0, SRB::VertexStage | SRB::FragmentStage, glyphUbo_, sizeof(GlyphUbo))});
  if (!glyphSrb_->create()) { check(nullptr, "glyphSrb"); return; }
  glyphPs_ = rhi_->newGraphicsPipeline();
  glyphPs_->setFlags(QRhiGraphicsPipeline::UsesScissor);
  glyphPs_->setShaderStages({{QRhiShaderStage::Vertex, glyphVs}, {QRhiShaderStage::Fragment, glyphFs}});
  QRhiVertexInputLayout glyphLayout;
  glyphLayout.setBindings({QRhiVertexInputBinding(9 * sizeof(float))});
  glyphLayout.setAttributes(
      {QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, 0),
       QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float3, 3 * sizeof(float)),
       QRhiVertexInputAttribute(0, 2, QRhiVertexInputAttribute::Float3, 6 * sizeof(float))});
  glyphPs_->setVertexInputLayout(glyphLayout);
  glyphPs_->setShaderResourceBindings(glyphSrb_);
  glyphPs_->setRenderPassDescriptor(rp);
  glyphPs_->setSampleCount(samples);
  glyphPs_->setTopology(QRhiGraphicsPipeline::Triangles);
  glyphPs_->setCullMode(QRhiGraphicsPipeline::None);  // two-sided lobes (winding not trusted)
  glyphPs_->setDepthTest(true);
  glyphPs_->setDepthWrite(true);
  if (!glyphPs_->create()) { check(nullptr, "glyphPs"); delete glyphPs_; glyphPs_ = nullptr; return; }

  // Force a re-upload of any geometry already staged before the device existed.
  lineDirty_ = !lineData_.empty();
  cageDirty_ = !cageData_.empty();
  boxDirty_ = hasBox_;
  highlightDirty_ = !highlightData_.empty();
  glyphDirty_ = hasGlyphs_;
  peaksDirty_ = hasPeaks_;
  for (TractOverlay& o : overlays_) o.dirty = !o.data.empty();
  for (ImageSlot& s : images_) { s.texDirty = s.lutDirty = s.quadsDirty = true; }
  focusDirty_ = !images_.empty();
}

void TractViewport::ReleaseAll() {
  // Per-image GPU resources first (each slot keeps its CPU Volume + lut for
  // re-upload). Resetting caps/counts forces the syncVbo high-water to re-grow.
  for (ImageSlot& s : images_) {
    for (QRhiResource** r : {reinterpret_cast<QRhiResource**>(&s.srb),
                             reinterpret_cast<QRhiResource**>(&s.sliceVbo),
                             reinterpret_cast<QRhiResource**>(&s.lutTex),
                             reinterpret_cast<QRhiResource**>(&s.tex)})
      if (*r) { (*r)->destroy(); delete *r; *r = nullptr; }
    s.sliceVboCap = 0;
    s.sliceVertexCount = 0;
  }
  for (TractOverlay& o : overlays_) {
    if (o.vbo) { o.vbo->destroy(); delete o.vbo; o.vbo = nullptr; }
    o.cap = 0;
    o.vertexCount = 0;
  }
  for (QRhiResource** r : {
           reinterpret_cast<QRhiResource**>(&glyphPs_),
           reinterpret_cast<QRhiResource**>(&glyphSrb_),
           reinterpret_cast<QRhiResource**>(&glyphUbo_),
           reinterpret_cast<QRhiResource**>(&glyphIbo_),
           reinterpret_cast<QRhiResource**>(&glyphVbo_),
           reinterpret_cast<QRhiResource**>(&peaksVbo_),
           reinterpret_cast<QRhiResource**>(&slicePs_),
           reinterpret_cast<QRhiResource**>(&sliceSrb_),
           reinterpret_cast<QRhiResource**>(&sampler_),
           reinterpret_cast<QRhiResource**>(&nearestSampler_),
           reinterpret_cast<QRhiResource**>(&lutTex_),
           reinterpret_cast<QRhiResource**>(&volTex_),
           reinterpret_cast<QRhiResource**>(&sliceUbo_),
           reinterpret_cast<QRhiResource**>(&pointPs_),
           reinterpret_cast<QRhiResource**>(&pointSrb_),
           reinterpret_cast<QRhiResource**>(&pointUbo_),
           reinterpret_cast<QRhiResource**>(&highlightPs_),
           reinterpret_cast<QRhiResource**>(&linePs_),
           reinterpret_cast<QRhiResource**>(&lineSrb_),
           reinterpret_cast<QRhiResource**>(&lineUbo_),
           reinterpret_cast<QRhiResource**>(&highlightVbo_),
           reinterpret_cast<QRhiResource**>(&handleVbo_),
           reinterpret_cast<QRhiResource**>(&boxVbo_),
           reinterpret_cast<QRhiResource**>(&cageVbo_),
           reinterpret_cast<QRhiResource**>(&lineVbo_),
       }) {
    if (*r) { (*r)->destroy(); delete *r; *r = nullptr; }
  }
  // Bug fix: reset the upload/capacity state so a device-loss re-init re-uploads
  // everything instead of skipping it because the old flags said "done".
  lineVboCap_ = cageVboCap_ = boxVboCap_ = handleVboCap_ = highlightVboCap_ = 0;
  glyphVboCap_ = glyphIboCap_ = peaksVboCap_ = 0;
  lineVertexCount_ = cageVertexCount_ = boxVertexCount_ = handleVertexCount_ =
      highlightVertexCount_ = 0;
  glyphVertexCount_ = glyphIndexCount_ = peaksVertexCount_ = 0;
  lineDirty_ = !lineData_.empty();
  cageDirty_ = !cageData_.empty();
  boxDirty_ = hasBox_;
  highlightDirty_ = !highlightData_.empty();
  glyphDirty_ = hasGlyphs_;
  peaksDirty_ = hasPeaks_;
  for (TractOverlay& o : overlays_) o.dirty = !o.data.empty();
  for (ImageSlot& s : images_) { s.texDirty = s.lutDirty = s.quadsDirty = true; }
  focusDirty_ = !images_.empty();
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
  syncVbo(highlightVbo_, highlightVboCap_, highlightVertexCount_, highlightData_, 6, highlightDirty_);
  for (TractOverlay& o : overlays_) syncVbo(o.vbo, o.cap, o.vertexCount, o.data, 6, o.dirty);
  syncVbo(peaksVbo_, peaksVboCap_, peaksVertexCount_, peaksData_, 6, peaksDirty_);

  // ODF glyph mesh: the [pos,normal,rgb] vertex buffer (9 floats/vertex) goes
  // through syncVbo; its parallel uint32 index buffer is uploaded inline here with
  // the same grow-on-demand pattern (syncVbo only handles float vertex buffers).
  // A copied dirty flag drives the VBO so glyphDirty_ survives for the IBO below.
  bool glyphVboDirty = glyphDirty_;
  syncVbo(glyphVbo_, glyphVboCap_, glyphVertexCount_, glyphData_, 9, glyphVboDirty);
  if (glyphDirty_) {
    const std::size_t ibytes = glyphIndices_.size() * sizeof(std::uint32_t);
    if (!glyphIbo_ || glyphIboCap_ < glyphIndices_.size()) {
      if (glyphIbo_) { glyphIbo_->destroy(); delete glyphIbo_; glyphIbo_ = nullptr; }
      glyphIboCap_ = std::max<std::size_t>(glyphIndices_.size(), 1);
      glyphIbo_ = rhi_->newBuffer(QRhiBuffer::Static, QRhiBuffer::IndexBuffer,
                                  static_cast<quint32>(glyphIboCap_ * sizeof(std::uint32_t)));
      if (!glyphIbo_->create()) {
        std::fprintf(stderr, "TractViewport: glyph index buffer create failed\n");
        delete glyphIbo_; glyphIbo_ = nullptr; glyphIboCap_ = 0;
      }
    }
    if (glyphIbo_ && ibytes > 0)
      u->uploadStaticBuffer(glyphIbo_, 0, static_cast<quint32>(ibytes), glyphIndices_.data());
    glyphIndexCount_ = glyphIbo_ ? glyphIndices_.size() : 0;
    glyphDirty_ = false;
  }

  // Background image layers: (re)create + upload each slot's 3D texture and LUT,
  // then (further down) its slice quads. The per-slot dirty flags gate every GPU
  // op, so a steady frame (or a window/opacity tweak) touches none of this.
  for (ImageSlot& s : images_) {
    if (s.texDirty) {
      const Volume& v = s.vol;
      const int nx = s.dims[0], ny = s.dims[1], nz = s.dims[2];
      if (s.tex) { s.tex->destroy(); delete s.tex; s.tex = nullptr; }
      s.tex = rhi_->newTexture(QRhiTexture::R32F, nx, ny, nz, 1, QRhiTexture::ThreeDimensional);
      if (!s.tex->create()) {
        std::fprintf(stderr, "TractViewport: 3D texture create failed\n");
        delete s.tex; s.tex = nullptr;
      } else {
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
        u->uploadTexture(s.tex, desc);
      }
      s.texDirty = false;
      s.quadsDirty = true;  // dims/affine may have changed -> rebuild quads
      s.lutDirty = true;    // (re)create the LUT + rebind the SRB against the new tex
    }
    if (s.lutDirty) {
      // Label colour table (RGBA per index); a 1x1 placeholder for grayscale
      // images so binding 2 stays valid (the shader never samples it in gray mode).
      const int w = std::max(1, s.lutWidth);
      if (s.lutTex) { s.lutTex->destroy(); delete s.lutTex; s.lutTex = nullptr; }
      s.lutTex = rhi_->newTexture(QRhiTexture::RGBA32F, QSize(w, 1), 1);
      if (s.lutTex->create() && !s.lut.empty()) {
        QRhiTextureSubresourceUploadDescription sub(
            s.lut.data(), static_cast<quint32>(s.lut.size() * sizeof(float)));
        QRhiTextureUploadEntry entry(0, 0, sub);
        QRhiTextureUploadDescription desc;
        desc.setEntries(&entry, &entry + 1);
        u->uploadTexture(s.lutTex, desc);
      }
      if (!s.srb) s.srb = rhi_->newShaderResourceBindings();
      RebuildSliceSrb(s);  // binds s.tex + s.lutTex (uVol sampler tracks label mode)
      s.lutDirty = false;
    }
  }

  // Lazily fit the ortho panes and place the shared focus. The ortho VIEW extent
  // follows the volume (so you see the whole slice), but the focus (which slice
  // is shown) centres on the streamline bundle when one is loaded — that's where
  // the density map lives, so it's visible by default — else on the volume.
  if (!focusInit_ && (lineVertexCount_ > 0 || !images_.empty() || hasGlyphs_ || hasPeaks_)) {
    const Bounds& frameB = hasVolumeBounds_      ? volumeBounds_
                           : (lineVertexCount_ > 0) ? bounds_
                           : hasGlyphs_          ? glyphBounds_
                           : hasPeaks_           ? peaksBounds_
                                                 : bounds_;             // ortho view extent
    const Bounds& focusB = (lineVertexCount_ > 0) ? bounds_ : frameB;   // slice at the bundle
    focus_ = {static_cast<float>(0.5 * (focusB.v[0] + focusB.v[1])),
              static_cast<float>(0.5 * (focusB.v[2] + focusB.v[3])),
              static_cast<float>(0.5 * (focusB.v[4] + focusB.v[5]))};
    for (int a = 0; a < 3; ++a) ortho_[a].Frame(frameB, a);
    focusInit_ = true;
    focusDirty_ = true;
  }

  // (Re)build + upload each slot's slice quads when its geometry changed or the
  // shared focus moved (a scrub/grab moves focus_ for all layers at once).
  for (ImageSlot& s : images_) {
    if (!(s.quadsDirty || focusDirty_)) continue;
    RebuildSliceQuads(s);
    bool d = true;
    syncVbo(s.sliceVbo, s.sliceVboCap, s.sliceVertexCount, s.sliceData, 3, d);
    s.quadsDirty = false;
  }
  focusDirty_ = false;

  // Fill each active pane's UBO block. OrbitCamera/OrthoSliceCamera.ViewProj are
  // ROW-MAJOR; left-multiply by clipSpaceCorrMatrix() (RHI depth/Y fix) and
  // memcpy the column-major constData() into the pane's slot at i * stride.
  const QMatrix4x4 clip = rhi_->clipSpaceCorrMatrix();
  const int W = px.width(), H = px.height();

  // Visible image layers in blend order (non-labels first), capped for the UBO.
  // Each gets a "rank" k; its UBO block for pane i lives at (k*4 + i) * stride.
  std::vector<int> vis;
  vis.reserve(drawOrder_.size());
  for (int idx : drawOrder_) {
    const ImageSlot& s = images_[idx];
    // s.tex implies its SRB was fully built (RebuildSliceSrb needs tex+lutTex); a
    // failed texture create leaves s.tex null, so skip it rather than bind a
    // never-created SRB.
    if (s.visible && s.tex && s.srb && s.sliceVbo && s.sliceVertexCount > 0)
      vis.push_back(idx);
    if (static_cast<int>(vis.size()) >= kMaxBlendImages) break;
  }

  for (int i = 0; i < 4; ++i) {
    if (maximized_ >= 0 && i != maximized_) continue;
    int rx, ry, rw, rh;
    PaneRectPx(i, W, H, rx, ry, rw, rh);
    const float pa = static_cast<float>(rw) / std::max(1, rh);
    const int axis = PaneAxis(i);
    Mat4 vpm;
    if (axis < 0) {
      vpm = camera_.ViewProj(pa);
    } else {
      // The ortho camera keeps its own in-plane view centre (pan/zoom); the slice
      // it shows is the focus-driven quad, so we don't overwrite its focus here.
      vpm = ortho_[axis].ViewProj(pa);
    }
    const QMatrix4x4 mvp = clip * QMatrix4x4(vpm.m);

    LineUbo lu{};
    std::memcpy(lu.mvp, mvp.constData(), sizeof(lu.mvp));
    u->updateDynamicBuffer(lineUbo_, i * lineUboStride_, sizeof(lu), &lu);

    PointUbo pu{};
    std::memcpy(pu.mvp, mvp.constData(), sizeof(pu.mvp));
    pu.params[0] = 13.0f;  // handle point size (px)
    u->updateDynamicBuffer(pointUbo_, i * pointUboStride_, sizeof(pu), &pu);

    if (hasGlyphs_) {
      GlyphUbo gu{};
      std::memcpy(gu.mvp, mvp.constData(), sizeof(gu.mvp));
      // Fixed world-space key light (matches the ODF prototype); shader normalizes it.
      gu.lightDir[0] = 0.4f; gu.lightDir[1] = -0.7f; gu.lightDir[2] = 0.6f;
      u->updateDynamicBuffer(glyphUbo_, i * glyphUboStride_, sizeof(gu), &gu);
    }

    if (showVolume_) {
      for (int k = 0; k < static_cast<int>(vis.size()); ++k) {
        const ImageSlot& s = images_[vis[k]];
        SliceUbo su{};
        std::memcpy(su.mvp, mvp.constData(), sizeof(su.mvp));
        const QMatrix4x4 v2w(s.voxToWorld.m);
        std::memcpy(su.voxToWorld, v2w.constData(), sizeof(su.voxToWorld));
        su.invDims[0] = s.invDims.x; su.invDims[1] = s.invDims.y; su.invDims[2] = s.invDims.z;
        su.invDims[3] = static_cast<float>(s.lutWidth);             // label LUT width
        // Stat mode reuses x/y as (threshold, cap - threshold); SetImageParams stores
        // them in valueMin/valueRange exactly like a grayscale window, so no extra field.
        su.valueParams[0] = s.valueMin; su.valueParams[1] = s.valueRange;
        su.valueParams[2] = s.opacity;
        su.valueParams[3] = s.statmap ? 3.0f                    // 0 gray,1 label,2 heatmap,3 stat,4 viridis
                          : s.viridis ? 4.0f
                          : (s.heatmap ? 2.0f : (s.isLabel ? 1.0f : 0.0f));
        const quint32 off = (static_cast<quint32>(k) * 4 + static_cast<quint32>(i)) * sliceUboStride_;
        u->updateDynamicBuffer(sliceUbo_, off, sizeof(su), &su);
      }
    }
  }

  // One pass, cleared once; each pane draws into its own viewport+scissor (the
  // four scissor rects are disjoint, so the shared depth buffer can't bleed).
  cb->beginPass(rt, QColor::fromRgbF(0.035, 0.035, 0.04), {1.0f, 0}, u);

  for (int i = 0; i < 4; ++i) {
    if (maximized_ >= 0 && i != maximized_) continue;
    int rx, ry, rw, rh;
    PaneRectPx(i, W, H, rx, ry, rw, rh);
    const int yBottom = H - (ry + rh);  // RHI viewport/scissor origin is bottom-left
    const QRhiViewport vp(rx, yBottom, rw, rh);
    const QRhiScissor sc(rx, yBottom, rw, rh);
    const int axis = PaneAxis(i);
    const QRhiCommandBuffer::DynamicOffset lineOff(0, static_cast<quint32>(i) * lineUboStride_);
    const QRhiCommandBuffer::DynamicOffset pointOff(0, static_cast<quint32>(i) * pointUboStride_);

    auto drawLines = [&](QRhiBuffer* buf, std::size_t n) {
      if (!buf || n == 0) return;
      QRhiCommandBuffer::VertexInput vin(buf, 0);
      cb->setVertexInput(0, 1, &vin);
      cb->draw(static_cast<quint32>(n));
    };

    // Streamlines render ONLY in the 3-D pane; the 2-D slice panes show the
    // track-density map instead (much cheaper than the full geometry ×3 panes,
    // and clearer for slice navigation). showTracts_ gates the active bundle;
    // overlay bundles draw whenever present. Cage = 3-D, edit-mode only.
    cb->setGraphicsPipeline(linePs_);
    cb->setViewport(vp);
    cb->setScissor(sc);
    cb->setShaderResources(lineSrb_, 1, &lineOff);
    if (axis < 0) {
      if (showTracts_) drawLines(lineVbo_, lineVertexCount_);
      for (const TractOverlay& o : overlays_) drawLines(o.vbo, o.vertexCount);
      if (showPeaks_) drawLines(peaksVbo_, peaksVertexCount_);  // DEC peak segments
      if (editMode_) drawLines(cageVbo_, cageVertexCount_);
    }

    // ODF glyphs (3-D pane only; opaque, depth test + write). Drawn after the
    // streamlines and before the alpha-blended slices so depth resolves correctly.
    if (axis < 0 && showGlyphs_ && glyphPs_ && glyphVbo_ && glyphIbo_ && glyphIndexCount_ > 0) {
      cb->setGraphicsPipeline(glyphPs_);
      cb->setViewport(vp);
      cb->setScissor(sc);
      const QRhiCommandBuffer::DynamicOffset glyphOff(0, static_cast<quint32>(i) * glyphUboStride_);
      cb->setShaderResources(glyphSrb_, 1, &glyphOff);
      const QRhiCommandBuffer::VertexInput vin(glyphVbo_, 0);
      cb->setVertexInput(0, 1, &vin, glyphIbo_, 0, QRhiCommandBuffer::IndexUInt32);
      cb->drawIndexed(static_cast<quint32>(glyphIndexCount_));
    }

    // Background image layers: blend each visible slot back-to-front (alpha
    // blend, depth no-write). 3D pane draws all three quads; an ortho pane draws
    // only its axis's quad. sliceData order is X(0..5), Y(6..11), Z(12..17).
    if (showVolume_ && slicePs_) {
      cb->setGraphicsPipeline(slicePs_);
      cb->setViewport(vp);
      cb->setScissor(sc);
      for (int k = 0; k < static_cast<int>(vis.size()); ++k) {
        const ImageSlot& s = images_[vis[k]];
        if (s.orthoOnly && axis < 0) continue;  // density map: 2-D slice panes only
        const quint32 off =
            (static_cast<quint32>(k) * 4 + static_cast<quint32>(i)) * sliceUboStride_;
        const QRhiCommandBuffer::DynamicOffset so(0, off);
        cb->setShaderResources(s.srb, 1, &so);
        QRhiCommandBuffer::VertexInput vin(s.sliceVbo, 0);
        cb->setVertexInput(0, 1, &vin);
        if (axis < 0) {
          cb->draw(static_cast<quint32>(s.sliceVertexCount));
        } else if (s.sliceVertexCount >= static_cast<std::size_t>(axis) * 6 + 6) {
          cb->draw(6, 1, static_cast<quint32>(axis) * 6, 0);
        }
      }
    }

    // White in-box highlight (3-D pane only, on top, no depth) — matches the
    // streamlines, which only render in the 3-D pane.
    if (axis < 0 && highlightPs_ && highlightVbo_ && highlightVertexCount_ > 0) {
      cb->setGraphicsPipeline(highlightPs_);
      cb->setViewport(vp);
      cb->setScissor(sc);
      cb->setShaderResources(lineSrb_, 1, &lineOff);
      drawLines(highlightVbo_, highlightVertexCount_);
    }

    // Selection box wireframe (every pane) + drag handles (3D pane) — edit mode only.
    if (editMode_ && hasBox_ && boxVbo_ && boxVertexCount_ > 0) {
      cb->setGraphicsPipeline(linePs_);
      cb->setViewport(vp);
      cb->setScissor(sc);
      cb->setShaderResources(lineSrb_, 1, &lineOff);
      drawLines(boxVbo_, boxVertexCount_);
    }
    if (editMode_ && axis < 0 && hasBox_ && handleVbo_ && handleVertexCount_ > 0) {
      cb->setGraphicsPipeline(pointPs_);
      cb->setViewport(vp);
      cb->setScissor(sc);
      cb->setShaderResources(pointSrb_, 1, &pointOff);
      QRhiCommandBuffer::VertexInput vin(handleVbo_, 0);
      cb->setVertexInput(0, 1, &vin);
      cb->draw(static_cast<quint32>(handleVertexCount_));
    }
  }

  cb->endPass();

  // Repaint the orientation labels from the (possibly new) content / pane layout.
  // update() coalesces, so this is one cheap text repaint per event-loop turn.
  if (orientOverlay_) orientOverlay_->update();
}

// ── Interaction (renderer-agnostic; unchanged from the GL viewport) ───────────

void TractViewport::mousePressEvent(QMouseEvent* event) {
  lastPos_ = event->pos();
  dragPane_ = PaneAt(event->pos());
  const int axis = dragPane_ >= 0 ? PaneAxis(dragPane_) : -1;
  if (event->button() == Qt::LeftButton) {
    if (axis < 0) {  // 3D pane: box handle (edit) > grab a slice plane > orbit
      activeHandle_ = editMode_ ? PickHandle(event->pos()) : -1;
      if (activeHandle_ >= 0) {
        // editing a box handle
      } else if (hasVolumeBounds_ && showVolume_ &&
                 (sliceDragAxis_ = PickSlicePlane(event->pos())) >= 0) {
        // grabbed a slice plane — slide it along its normal on move
      } else {
        rotating_ = true;
      }
    } else {
      panning_ = true;                           // ortho pane: left-drag pans in-plane
    }
  }
  // Middle (wheel) is mapped too, but macOS/mouse drivers often intercept it
  // before Qt sees it, so right-drag is the reliable pan.
  if (event->button() == Qt::RightButton || event->button() == Qt::MiddleButton) panning_ = true;
}

void TractViewport::mouseMoveEvent(QMouseEvent* event) {
  hoverPane_ = PaneAt(event->pos());  // arrow keys scrub the slice in the hovered pane
  const QPoint d = event->pos() - lastPos_;
  const int axis = dragPane_ >= 0 ? PaneAxis(dragPane_) : -1;
  if (sliceDragAxis_ >= 0) {                      // 3D: slide the grabbed slice plane
    const Vec3 axisDir{sliceDragAxis_ == 0 ? 1.0f : 0.0f, sliceDragAxis_ == 1 ? 1.0f : 0.0f,
                       sliceDragAxis_ == 2 ? 1.0f : 0.0f};
    const float wd = WorldDeltaAlongAxis(focus_, axisDir, QPointF(d.x(), d.y()));
    if (sliceDragAxis_ == 0) focus_.x += wd;
    else if (sliceDragAxis_ == 1) focus_.y += wd;
    else focus_.z += wd;
    focusDirty_ = true;  // every layer's slice quads follow the shared focus
    emit sliceFocusChanged();  // slice-following ODF/peaks rebuild at the new slice
    update();
  } else if (activeHandle_ >= 0) {                // 3D box edit (handles live in the 3D pane)
    DragSelectionHandle(QPointF(d.x(), d.y()));
    // Recompute the in-box highlight at ~20 Hz (matches the Python reference); the
    // full-set query is too costly to run on every mouse move, but the box itself
    // still redraws every move. mouseReleaseEvent does a final settle pass.
    if (highlightClock_.elapsed() - lastHighlightMs_ >= 50) {
      lastHighlightMs_ = highlightClock_.elapsed();
      UpdateHighlight();
    }
    update();
  } else if (rotating_ && axis < 0) {            // orbit only in the 3D pane
    camera_.Rotate(d.x(), d.y());
    update();
  } else if (panning_) {
    if (axis < 0) {
      camera_.Pan(d.x(), d.y());
    } else {                                     // ortho pane: pan its focus in-plane
      float fx, fy, fw, fh;
      PaneFrac(dragPane_, fx, fy, fw, fh);
      const int pw = std::max(1, static_cast<int>(fw * width()));
      const int ph = std::max(1, static_cast<int>(fh * height()));
      ortho_[axis].Pan(d.x(), d.y(), pw, ph);
    }
    update();
  }
  lastPos_ = event->pos();
}

void TractViewport::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    const bool wasDraggingBox = activeHandle_ >= 0;
    rotating_ = false;
    activeHandle_ = -1;
    sliceDragAxis_ = -1;
    // Settle pass: the ~20 Hz throttle can skip the final move, so recompute once
    // here so the white overlay exactly matches the box (and what d/k will edit).
    if (wasDraggingBox) UpdateHighlight();
  }
  if (event->button() == Qt::RightButton || event->button() == Qt::MiddleButton) panning_ = false;
  dragPane_ = -1;
}

void TractViewport::mouseDoubleClickEvent(QMouseEvent* event) {
  const int pane = PaneAt(event->pos());
  if (pane < 0) return;
  maximized_ = (maximized_ == pane) ? -1 : pane;  // toggle full-screen for this pane
  update();
}

void TractViewport::wheelEvent(QWheelEvent* event) {
  const QPointF pos = event->position();  // logical widget pixels
  const int pane = PaneAt(pos.toPoint());
  const int axis = pane >= 0 ? PaneAxis(pane) : -1;
  const double steps = event->angleDelta().y() / 120.0;
  if (axis < 0) {  // 3D: zoom toward the cursor (within the 3D pane's rect)
    int rx, ry, rw, rh;
    if (!ThreeDRectLogical(rx, ry, rw, rh)) { QRhiWidget::wheelEvent(event); return; }
    const float ndcx = 2.0f * static_cast<float>(pos.x() - rx) / std::max(1, rw) - 1.0f;
    const float ndcy = 1.0f - 2.0f * static_cast<float>(pos.y() - ry) / std::max(1, rh);
    camera_.ZoomToCursor(steps, ndcx, ndcy, static_cast<float>(rw) / std::max(1, rh));
  } else {         // ortho: zoom the slice view
    ortho_[axis].Zoom(steps);
  }
  update();
}

void TractViewport::keyPressEvent(QKeyEvent* event) {
  // Up/Down (PageUp/Down = ±10) scrub the slice in the hovered ortho pane: move
  // the shared focus along that pane's world axis by whole voxels.
  const int axis = hoverPane_ >= 0 ? PaneAxis(hoverPane_) : -1;
  const int key = event->key();
  if (axis >= 0 && hasVolumeBounds_ &&
      (key == Qt::Key_Up || key == Qt::Key_Down || key == Qt::Key_PageUp ||
       key == Qt::Key_PageDown)) {
    const float dir = (key == Qt::Key_Up || key == Qt::Key_PageUp) ? 1.0f : -1.0f;
    const float steps = (key == Qt::Key_PageUp || key == Qt::Key_PageDown) ? 10.0f : 1.0f;
    const float step = dir * steps * sliceScrubStep_[axis];
    if (axis == 0) focus_.x += step;
    else if (axis == 1) focus_.y += step;
    else focus_.z += step;
    focusDirty_ = true;  // every layer's slice quads follow the shared focus
    emit sliceFocusChanged();  // slice-following ODF/peaks rebuild at the new slice
    update();
    return;
  }
  if (key == Qt::Key_R) {
    ResetHoveredView();  // reset the pane under the cursor (all panes if none hovered)
  } else {
    QRhiWidget::keyPressEvent(event);
  }
}

}  // namespace tracto
