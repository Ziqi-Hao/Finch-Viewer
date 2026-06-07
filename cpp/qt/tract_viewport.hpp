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

class TractViewport : public QRhiWidget {
  Q_OBJECT
 public:
  explicit TractViewport(QWidget* parent = nullptr);
  ~TractViewport() override;

  // Replace the line buffer (+ per-streamline spans, for the in-box highlight).
  // Safe before the rhi exists: the upload is deferred to the next render().
  void SetLineGeometry(std::vector<float> interleaved, std::vector<DisplaySpan> spans,
                       const Bounds& bounds);

  // Provide a full-set in-box query (MainWindow binds its selection backend).
  // Drives the live white highlight of the streamlines inside the box.
  void SetSelectionQuery(std::function<std::vector<uint8_t>(const Bounds&)> query);

  // Set the scalar background volume (uploaded once as a 3D texture; three mid
  // slices are rendered as affine-placed quads). Safe before the rhi exists.
  // By value + move so the caller's multi-MB voxel array is not copied.
  void SetVolume(Volume volume);
  void SetVolumeVisible(bool visible);
  bool VolumeVisible() const { return showVolume_; }
  void SetTractsVisible(bool visible);  // hide/show the streamline layer
  bool TractsVisible() const { return showTracts_; }
  void SetVolumeRange(float lo, float hi);  // grayscale window (contrast) of the active volume

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

  void ResetCamera();

  // RHI device + backend name (e.g. "Apple M4 Pro · Metal") for the perf overlay;
  // empty until the rhi exists. Lives here because rhi() is protected.
  QString RendererName() const;

  // The four panes of the 2x2 layout (cell order: TL, TR, BL, BR).
  enum class ViewKind { ThreeD, Axial, Coronal, Sagittal };

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
  void CreateResources();   // build pipelines/buffers/textures once per rhi device
  void ReleaseAll();        // destroy all RHI resources (device loss / teardown)
  void RebuildCage();       // yellow bounding cage from bounds_ (CPU staging only)
  void RebuildSliceQuads(); // build the 3 slice quads at the current focus voxel
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

  // Volume slices (3D texture sampled in the fragment shader; voxel-space quads).
  Volume pendingVolume_;          // staged until the rhi exists; cleared after upload
  std::vector<float> sliceData_;  // [x,y,z] voxel-coord quad vertices (Triangles)
  bool volumeDirty_ = false;      // re-upload the 3D texture + slice quads
  bool hasVolume_ = false;
  bool showVolume_ = true;
  bool showTracts_ = true;
  Mat4 voxToWorld_;
  Mat4 worldToVox_;                 // inverse of voxToWorld_ (focus world -> voxel index)
  float voxelSpacing_[3] = {1.0f, 1.0f, 1.0f};  // world mm per voxel along each volume axis
  bool sliceQuadsDirty_ = false;    // rebuild the slice quads at the current focus
  Vec3 invDims_{1.0f, 1.0f, 1.0f};
  float volValueMin_ = 0.0f;
  float volValueRange_ = 1.0f;
  int volDims_[3] = {0, 0, 0};

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
  QRhiBuffer* sliceVbo_ = nullptr;
  QRhiBuffer* sliceUbo_ = nullptr;   // mvp + voxToWorld + invDims + valueParams
  QRhiTexture* volTex_ = nullptr;
  QRhiSampler* sampler_ = nullptr;
  QRhiShaderResourceBindings* lineSrb_ = nullptr;
  QRhiShaderResourceBindings* pointSrb_ = nullptr;
  QRhiShaderResourceBindings* sliceSrb_ = nullptr;
  QRhiGraphicsPipeline* linePs_ = nullptr;   // Lines, depth test+write
  QRhiGraphicsPipeline* highlightPs_ = nullptr;  // Lines, no depth (white selection on top)
  QRhiGraphicsPipeline* pointPs_ = nullptr;  // Points, no depth (handles on top)
  QRhiGraphicsPipeline* slicePs_ = nullptr;  // Triangles, alpha blend, depth no-write

  // Capacities currently allocated on the GPU; a grow re-creates the buffer.
  std::size_t lineVboCap_ = 0;
  std::size_t cageVboCap_ = 0;
  std::size_t boxVboCap_ = 0;
  std::size_t handleVboCap_ = 0;
  std::size_t highlightVboCap_ = 0;
  std::size_t sliceVboCap_ = 0;

  std::size_t lineVertexCount_ = 0;
  std::size_t cageVertexCount_ = 0;
  std::size_t boxVertexCount_ = 0;
  std::size_t handleVertexCount_ = 0;
  std::size_t highlightVertexCount_ = 0;
  std::size_t sliceVertexCount_ = 0;

  bool volTexUploaded_ = false;   // the 3D texture matches volTex_ dims + content

  // Per-pane dynamic-offset strides (aligned to ubufAlignment). Each UBO holds
  // one block per pane; render() binds pane i at offset i * stride.
  quint32 lineUboStride_ = 0;
  quint32 pointUboStride_ = 0;
  quint32 sliceUboStride_ = 0;
};

}  // namespace tracto
