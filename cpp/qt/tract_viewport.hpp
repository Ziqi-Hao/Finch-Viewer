#pragma once

// Qt RHI (QRhiWidget, Metal on macOS) viewport: render + interaction only. It
// owns no tractogram or edit state. The MainWindow hands it a ready-made
// interleaved [pos.xyz, rgb] line buffer (built by display_geometry) and a
// bounds box; the viewport draws those lines plus a yellow bounds cage, the
// FA background slices, and the cyan selection box/handles under an orbit
// camera. Keeping data and editing out of here preserves the module boundary
// from CLAUDE.md. The renderer-agnostic camera math (WorldToScreen, handle
// pick/drag) is shared with the old GL viewport and unchanged.

#include "bounds.hpp"
#include "display_geometry.hpp"  // DisplaySpan (per-streamline vertex ranges)
#include "nifti_io.hpp"
#include "render_math.hpp"

#include <QElapsedTimer>
#include <QRhiWidget>
#include <QPoint>
#include <QPointF>
#include <QRectF>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

// Forward-declare the RHI types so the header stays free of <rhi/qrhi.h> (which
// needs Qt6::GuiPrivate). Definitions are only needed in the .cpp.
class QRhi;
class QRhiBuffer;
class QRhiTexture;
class QRhiSampler;
class QRhiShaderResourceBindings;
class QRhiGraphicsPipeline;

namespace tracto {

class OrientationOverlay;  // anatomical L/R·A/P·S/I labels (own child widget)

class TractViewport : public QRhiWidget {
  Q_OBJECT
 public:
  explicit TractViewport(QWidget* parent = nullptr);
  ~TractViewport() override;

  // Replace the line buffer (+ per-streamline spans, for the in-box highlight).
  // This is the ACTIVE (editable) tractogram — the one with the selection box and
  // highlight. Safe before the rhi exists: the upload is deferred to render().
  void SetLineGeometry(std::vector<float> interleaved, std::vector<DisplaySpan> spans,
                       const Bounds& bounds);

  // Replace the read-only overlay tractograms (the other visible-but-not-active
  // bundles), each an interleaved [pos.xyz, rgb] line buffer like SetLineGeometry.
  // Drawn alongside the active one so several tractograms can show at once.
  void SetTractOverlays(std::vector<std::vector<float>> overlays);

  // ODF glyph mesh (indexed, lit triangles). MainWindow builds it on load from a
  // SH-coefficient volume (cpp/odf) and hands over an interleaved
  // [pos.xyz, normal.xyz, rgb] vertex buffer + a uint32 triangle index list, plus
  // the glyph world (RAS) bounds for framing an ODF-only scene. Drawn opaque in the
  // 3-D pane. Pass empty vectors to clear. Safe before the rhi exists (defers upload).
  void SetGlyphGeometry(std::vector<float> interleaved, std::vector<std::uint32_t> indices,
                        const Bounds& bounds);
  void SetGlyphsVisible(bool visible);  // hide/show the glyph layer
  bool GlyphsVisible() const { return showGlyphs_; }
  bool HasGlyphs() const { return hasGlyphs_; }

  // Peaks field (per-voxel principal directions) drawn as DEC-coloured line
  // segments. MainWindow builds the segment buffer ([pos.xyz, rgb], 2 verts/segment)
  // on load; this reuses the line pipeline (one VBO, one draw — no new pipeline).
  // Pass an empty buffer to clear. Safe before the rhi exists (defers upload).
  void SetPeaksGeometry(std::vector<float> interleaved, const Bounds& bounds);
  void SetPeaksVisible(bool visible);  // hide/show the peaks layer
  bool PeaksVisible() const { return showPeaks_; }
  bool HasPeaks() const { return hasPeaks_; }

  // Provide a full-set in-box query (MainWindow binds its selection backend).
  // Drives the live white highlight of the streamlines inside the box.
  void SetSelectionQuery(std::function<std::vector<uint8_t>(const Bounds&)> query);

  // Multi-image background. Each loaded volume/label is an independent slice
  // layer (its own 3D texture, window/opacity, and grayscale-or-label mode),
  // keyed by `id`; the slice panes blend every visible layer back-to-front
  // (non-label volumes underneath, labels on top). Keying by id keeps cheap
  // window/opacity tweaks off the texture-upload path. Safe before the rhi
  // exists (uploads defer to render). `lut` is RGBA per label index (label mode).
  void SetImage(int id, Volume vol, bool isLabel, std::vector<float> lut, int lutWidth);
  void SetImageParams(int id, float winLo, float winHi, float opacity);  // cheap: UBO only
  void SetImageVisible(int id, bool visible);
  void SetImageHeatmap(int id, bool on);    // render via the "hot" density colour ramp
  void SetImageStatmap(int id, bool on);    // signed stat overlay (diverging hot/cool + threshold)
  void SetImageOrthoOnly(int id, bool on);  // draw only in the 2-D slice panes (not the 3-D pane)
  void RemoveImage(int id);

  void SetVolumeVisible(bool visible);  // global master switch for all slice layers
  bool VolumeVisible() const { return showVolume_; }
  void SetTractsVisible(bool visible);  // hide/show the streamline layer
  bool TractsVisible() const { return showTracts_; }

  // Edit mode is opt-in: in view mode (default) no cage / selection box / handles
  // are drawn and left-drag always orbits. The selection box + box editing only
  // appear once edit mode is on. Core experience is viewing, not editing.
  void SetEditMode(bool on);
  bool EditMode() const { return editMode_; }

  // 3-D selection box (axis-aligned, world/RAS mm). MainWindow reads it to run
  // delete/keep editing; the viewport owns its geometry and the drag handles
  // (grab a face handle to resize a side, the centre handle to move the box).
  Bounds SelectionBox() const { return boxBounds_; }
  bool HasSelectionBox() const { return hasBox_; }
  void ResetSelectionBox();                 // re-place inside the current data bounds
  void SetSelectionBox(const Bounds& box);  // set the box from numeric input (panel)

  void ResetCamera();            // re-frame ALL four panes + recentre the slice focus
  void ResetView(int pane);      // re-frame just one pane (pane<0 -> all, = ResetCamera)
  void ResetHoveredView();       // re-frame the pane under the cursor (R key); all if none

  // Shared slice-crosshair position (world RAS mm). MainWindow reads it to rebuild
  // slice-following ODF/peaks glyphs when the user scrubs.
  Vec3 SliceFocus() const { return focus_; }
  // Jump the slice focus to a world Z and lock it (used for headless verification of
  // slice-following; equivalent to scrubbing the axial pane there).
  void SetSliceFocusZ(float z);

 signals:
  void sliceFocusChanged();  // the slice crosshair moved (arrow scrub / slice-plane drag)

 public:

  // RHI device + backend name (e.g. "Apple M4 Pro · Metal") for the perf overlay;
  // empty until the rhi exists. Lives here because rhi() is protected.
  QString RendererName() const;

  // The four panes of the 2x2 layout (cell order: TL, TR, BL, BR).
  enum class ViewKind { ThreeD, Axial, Coronal, Sagittal };

  // One visible 2-D ortho pane: its logical-pixel rect + world axis (0=X,1=Y,2=Z).
  // Read by OrientationOverlay to place the L/R·A/P·S/I edge labels. Empty when
  // nothing is loaded or while a non-ortho pane is maximized.
  struct OrthoPaneInfo { QRectF rect; int axis; };
  std::vector<OrthoPaneInfo> OrthoPaneLayout() const;

 protected:
  // QRhiWidget lifecycle (replaces initializeGL/paintGL/resizeGL; resize implicit).
  void initialize(QRhiCommandBuffer* cb) override;
  void render(QRhiCommandBuffer* cb) override;
  void releaseResources() override;

  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;  // maximize/restore a pane
  void wheelEvent(QWheelEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;

 private:
  struct ImageSlot;         // one background image layer (defined below)

  void CreateResources();   // build pipelines/buffers/textures once per rhi device
  void ReleaseAll();        // destroy all RHI resources (device loss / teardown)
  void RebuildCage();       // yellow bounding cage from bounds_ (CPU staging only)
  void RebuildSliceQuads(ImageSlot& s);  // build s's 3 slice quads at the current focus
  void RebuildSliceSrb(ImageSlot& s);    // (re)bind s's SRB: UBO(dyn) + s.tex + s.lutTex
  ImageSlot* FindImage(int id);          // slot with this id, or nullptr
  void RecomputeDrawOrder();             // refill drawOrder_ (non-labels first, labels last)
  void RecomputeVolumeBounds();          // volumeBounds_ + scrub step from the primary image
  void UpdateHighlight();   // gather in-box displayed streamlines into white overlay

  // Selection box: geometry, placement, and screen-space handle interaction.
  void RebuildSelectionGeometry();              // wireframe + 7 handle points
  void PlaceSelectionBoxInBounds(double frac);  // centred box at `frac` of bounds_
  std::array<Vec3, 7> HandlePositions() const;  // [center, X-, X+, Y-, Y+, Z-, Z+]
  QPointF WorldToScreen(const Vec3& p) const;   // logical-pixel projection
  float WorldDeltaAlongAxis(const Vec3& origin, const Vec3& axis,
                            const QPointF& mouseDelta) const;
  int PickHandle(const QPoint& pos) const;      // -1 none, 0 center, 1..6 faces
  void DragSelectionHandle(const QPointF& mouseDelta);  // move/resize active handle
  // In the 3D pane, ray-pick the nearest slice plane under the cursor (-1 none,
  // else world axis 0/1/2). Used to grab + slide a plane along its normal.
  int PickSlicePlane(const QPoint& pos) const;

  // The bounds the cameras frame on, in priority order: volume > streamlines >
  // glyphs > peaks. Shared by ResetCamera / ResetView so they agree.
  const Bounds& FramingBounds() const;

  // ── 2x2 multi-view layout (one QRhi, four sub-viewports). ─────────────────
  static ViewKind PaneKind(int i);     // pane index 0..3 -> kind (cell order)
  static int PaneAxis(int i);          // -1 for the 3D pane, else 0=X 1=Y 2=Z
  // Pane i's rect as fractions of the widget (top-left origin); honours maximize.
  void PaneFrac(int i, float& x, float& y, float& w, float& h) const;
  // Pane i's pixel rect (top-left origin) inside a W x H surface; exact tiling.
  void PaneRectPx(int i, int W, int H, int& x, int& yTop, int& w, int& h) const;
  int PaneAt(const QPoint& posLogical) const;  // which pane holds a logical-px point (-1)
  // The 3D pane's logical-pixel rect (where box editing happens). false if hidden.
  bool ThreeDRectLogical(int& x, int& y, int& w, int& h) const;

  OrbitCamera camera_;
  OrthoSliceCamera ortho_[3];   // [axis] 0=X(sagittal) 1=Y(coronal) 2=Z(axial)
  Vec3 focus_{};                // shared crosshair / slice position (world mm)
  bool focusInit_ = false;
  int maximized_ = -1;          // -1 = 2x2; else a single pane (0..3) fills the view
  int dragPane_ = -1;           // pane index currently being dragged (-1 none)
  int hoverPane_ = -1;          // pane under the cursor (for arrow-key slice scrub)
  int sliceDragAxis_ = -1;      // 3D pane: slice plane being dragged (-1 none, else axis)
  Bounds bounds_;               // data (line/cage) bounds
  Bounds volumeBounds_;         // volume world bounds — preferred for framing
  bool hasVolumeBounds_ = false;

  // CPU-side staging. The dirty flags defer every GPU upload to render() so the
  // setters stay safe to call before the rhi exists and never need makeCurrent.
  std::vector<float> lineData_;   // [x,y,z,r,g,b] * vertices (Lines)
  std::vector<float> cageData_;   // yellow cage, same layout (Lines)
  bool lineDirty_ = false;
  bool cageDirty_ = false;

  // Live white highlight of the in-box streamlines: gathered from lineData_ via
  // displaySpans_ when the box moves, drawn on top (no depth) so it pops.
  std::vector<DisplaySpan> displaySpans_;  // per displayed streamline, in lineData_ order
  std::vector<float> highlightData_;       // [x,y,z, 1,1,1] * vertices (Lines)
  bool highlightDirty_ = false;
  std::function<std::vector<uint8_t>(const Bounds&)> selectionQuery_;  // full-set in-box mask
  QElapsedTimer highlightClock_;           // throttles the recompute during a drag
  qint64 lastHighlightMs_ = -1000;

  // ── ODF glyph layer (indexed lit triangles; mesh built CPU-side by MainWindow
  // from the SH-coefficient volume, then handed over whole like the line buffer). ─
  std::vector<float> glyphData_;             // [pos.xyz, normal.xyz, rgb] * vertices
  std::vector<std::uint32_t> glyphIndices_;  // triangle list into glyphData_
  Bounds glyphBounds_;                       // world (RAS) bounds of the glyph mesh
  bool hasGlyphs_ = false;
  bool showGlyphs_ = true;
  bool glyphDirty_ = false;                  // re-upload the VBO + IBO in render()

  // ── Peaks layer (DEC-coloured line segments; reuses the line pipeline). ──────
  std::vector<float> peaksData_;   // [pos.xyz, rgb] * vertices (Lines, 2 verts/segment)
  Bounds peaksBounds_;             // world (RAS) bounds of the peaks segments
  bool hasPeaks_ = false;
  bool showPeaks_ = true;
  bool peaksDirty_ = false;        // re-upload the peaks VBO in render()

  // Read-only overlay tractograms (visible bundles other than the active one).
  // Each is a static [pos.xyz, rgb] line buffer drawn with the line pipeline; no
  // spans/highlight (only the active bundle is editable). Replaced wholesale.
  struct TractOverlay {
    std::vector<float> data;        // [x,y,z,r,g,b] * vertices (Lines)
    QRhiBuffer* vbo = nullptr;
    std::size_t cap = 0, vertexCount = 0;
    bool dirty = false;             // re-upload in the next render()
  };
  std::vector<TractOverlay> overlays_;

  // ── Background image layers (multi-image blend). ──────────────────────────
  // Each loaded volume/label is one ImageSlot: its own 3D texture, slice quads,
  // window/opacity, and grayscale-or-label mode. The slice panes blend every
  // visible slot back-to-front (non-label volumes first, labels on top), so
  // several segmentations can overlay one anatomy. The CPU-side Volume + lut are
  // retained so RHI device loss can re-upload the textures.
  struct ImageSlot {
    int id = -1;
    bool visible = true;
    // Display params (cheap to change; pushed straight into the per-frame UBO).
    float valueMin = 0.0f, valueRange = 1.0f, opacity = 1.0f;
    bool isLabel = false;
    bool heatmap = false;    // density "hot" colour ramp (slice shader mode 2)
    bool statmap = false;    // signed stat overlay: diverging + threshold (slice shader mode 3)
    bool orthoOnly = false;  // draw only in the 2-D slice panes, not the 3-D pane
    int lutWidth = 1;
    // Source data + derived placement (slice quads live in this image's voxel space).
    Volume vol;
    std::vector<float> lut;        // label mode: RGBA per label index
    Mat4 voxToWorld, worldToVox;
    Vec3 invDims{1.0f, 1.0f, 1.0f};
    float voxelSpacing[3] = {1.0f, 1.0f, 1.0f};
    int dims[3] = {0, 0, 0};
    std::vector<float> sliceData;  // [x,y,z] voxel-coord quad vertices (Triangles)
    // GPU resources (owned; recreated on device loss). Each slot binds its own
    // texture into a layout-compatible SRB (the slice pipeline's template SRB).
    QRhiTexture* tex = nullptr;
    QRhiTexture* lutTex = nullptr;
    QRhiBuffer* sliceVbo = nullptr;
    QRhiShaderResourceBindings* srb = nullptr;
    std::size_t sliceVboCap = 0, sliceVertexCount = 0;
    bool texDirty = false;    // (re)create + upload tex, then rebuild srb + lut
    bool lutDirty = false;    // (re)create + upload lutTex, rebuild srb
    bool quadsDirty = false;  // rebuild + upload slice quads at the current focus
  };
  static constexpr int kMaxBlendImages = 8;  // simultaneously-blended layers (UBO sizing)
  std::vector<ImageSlot> images_;
  std::vector<int> drawOrder_;   // indices into images_, non-labels first then labels
  bool focusDirty_ = false;      // focus moved -> rebuild every slot's slice quads
  bool showVolume_ = true;       // global master switch for all slice layers
  bool showTracts_ = true;
  float sliceScrubStep_[3] = {1.0f, 1.0f, 1.0f};  // world mm/voxel of the primary image

  // Selection box (cyan wireframe + handle points).
  Bounds boxBounds_;
  bool hasBox_ = false;
  std::vector<float> boxData_;     // staged wireframe vertices (Lines)
  std::vector<float> handleData_;  // staged handle-point vertices (Points)
  bool boxDirty_ = false;          // re-upload box + handle vertex buffers
  int activeHandle_ = -1;          // -1 none; 0 center; 1..6 face handles (axis*2+side+1)

  QPoint lastPos_;
  bool rotating_ = false;
  bool panning_ = false;
  bool editMode_ = false;  // opt-in; gates cage/box/handles + box interaction

  // Anatomical orientation labels (L/R·A/P·S/I), a transparent child overlay that
  // repaints from OrthoPaneLayout(); owned by Qt's parent-child (this).
  OrientationOverlay* orientOverlay_ = nullptr;

  // ── RHI resources (owned; recreated on device loss). ──────────────────────
  QRhi* rhi_ = nullptr;

  // Line/cage/box share the line pipeline (Lines topology, [pos,rgb] layout).
  QRhiBuffer* lineVbo_ = nullptr;
  QRhiBuffer* cageVbo_ = nullptr;
  QRhiBuffer* boxVbo_ = nullptr;
  QRhiBuffer* handleVbo_ = nullptr;
  QRhiBuffer* highlightVbo_ = nullptr;
  QRhiBuffer* lineUbo_ = nullptr;    // mat4 mvp
  QRhiBuffer* pointUbo_ = nullptr;   // mat4 mvp + vec4 params (point size)
  // Slice UBO holds kMaxBlendImages*4 blocks (one per image×pane), bound per draw
  // via a dynamic offset. volTex_/lutTex_/sliceSrb_ are 1x1 placeholders that
  // exist only so the slice pipeline has a layout-compatible SRB at creation; the
  // real per-image textures live in each ImageSlot.
  QRhiBuffer* sliceUbo_ = nullptr;   // mvp + voxToWorld + invDims + valueParams (per image×pane)
  QRhiTexture* volTex_ = nullptr;        // 1x1 placeholder (template SRB only)
  QRhiTexture* lutTex_ = nullptr;        // 1x1 placeholder (template SRB only)
  QRhiSampler* sampler_ = nullptr;       // linear (grayscale volumes)
  QRhiSampler* nearestSampler_ = nullptr;  // nearest (labels + LUT)
  QRhiShaderResourceBindings* lineSrb_ = nullptr;
  QRhiShaderResourceBindings* pointSrb_ = nullptr;
  QRhiShaderResourceBindings* sliceSrb_ = nullptr;  // template (layout) for slicePs_
  QRhiGraphicsPipeline* linePs_ = nullptr;   // Lines, depth test+write
  QRhiGraphicsPipeline* highlightPs_ = nullptr;  // Lines, no depth (white selection on top)
  QRhiGraphicsPipeline* pointPs_ = nullptr;  // Points, no depth (handles on top)
  QRhiGraphicsPipeline* slicePs_ = nullptr;  // Triangles, alpha blend, depth no-write

  // ODF glyph layer: indexed lit triangles ([pos,normal,rgb], own UBO mvp+light).
  QRhiBuffer* glyphVbo_ = nullptr;
  QRhiBuffer* glyphIbo_ = nullptr;
  QRhiBuffer* glyphUbo_ = nullptr;   // mat4 mvp + vec4 lightDir, one block per pane
  QRhiShaderResourceBindings* glyphSrb_ = nullptr;
  QRhiGraphicsPipeline* glyphPs_ = nullptr;  // Triangles, depth test+write, two-sided lit
  std::size_t glyphVboCap_ = 0, glyphIboCap_ = 0;
  std::size_t glyphVertexCount_ = 0, glyphIndexCount_ = 0;
  quint32 glyphUboStride_ = 0;

  // Peaks layer: line segments drawn with linePs_/lineSrb_/lineUbo_ (own VBO only).
  QRhiBuffer* peaksVbo_ = nullptr;
  std::size_t peaksVboCap_ = 0, peaksVertexCount_ = 0;

  // Capacities currently allocated on the GPU; a grow re-creates the buffer.
  std::size_t lineVboCap_ = 0;
  std::size_t cageVboCap_ = 0;
  std::size_t boxVboCap_ = 0;
  std::size_t handleVboCap_ = 0;
  std::size_t highlightVboCap_ = 0;

  std::size_t lineVertexCount_ = 0;
  std::size_t cageVertexCount_ = 0;
  std::size_t boxVertexCount_ = 0;
  std::size_t handleVertexCount_ = 0;
  std::size_t highlightVertexCount_ = 0;

  // Per-pane dynamic-offset strides (aligned to ubufAlignment). The line/point
  // UBOs hold one block per pane; the slice UBO holds one per image×pane.
  quint32 lineUboStride_ = 0;
  quint32 pointUboStride_ = 0;
  quint32 sliceUboStride_ = 0;
};

}  // namespace tracto
