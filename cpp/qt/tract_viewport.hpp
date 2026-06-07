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
#include "nifti_io.hpp"
#include "render_math.hpp"

#include <QRhiWidget>
#include <QPoint>
#include <QPointF>

#include <array>
#include <cstddef>
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

  // Replace the line buffer and frame the camera to `bounds`. Safe to call
  // before the rhi exists: the upload is deferred to the next render().
  void SetLineGeometry(std::vector<float> interleaved, const Bounds& bounds);

  // Set the scalar background volume (uploaded once as a 3D texture; three mid
  // slices are rendered as affine-placed quads). Safe before the rhi exists.
  // By value + move so the caller's multi-MB voxel array is not copied.
  void SetVolume(Volume volume);
  void SetVolumeVisible(bool visible);
  bool VolumeVisible() const { return showVolume_; }

  // 3-D selection box (axis-aligned, world/RAS mm). MainWindow reads it to run
  // delete/keep editing; the viewport owns its geometry and the drag handles
  // (grab a face handle to resize a side, the centre handle to move the box).
  Bounds SelectionBox() const { return boxBounds_; }
  bool HasSelectionBox() const { return hasBox_; }
  void ResetSelectionBox();                 // re-place inside the current data bounds
  void SetSelectionBox(const Bounds& box);  // set the box from numeric input (panel)

  void ResetCamera();

 protected:
  // QRhiWidget lifecycle (replaces initializeGL/paintGL/resizeGL; resize implicit).
  void initialize(QRhiCommandBuffer* cb) override;
  void render(QRhiCommandBuffer* cb) override;
  void releaseResources() override;

  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;

 private:
  void CreateResources();   // build pipelines/buffers/textures once per rhi device
  void ReleaseAll();        // destroy all RHI resources (device loss / teardown)
  void RebuildCage();       // yellow bounding cage from bounds_ (CPU staging only)

  // Selection box: geometry, placement, and screen-space handle interaction.
  void RebuildSelectionGeometry();              // wireframe + 7 handle points
  void PlaceSelectionBoxInBounds(double frac);  // centred box at `frac` of bounds_
  std::array<Vec3, 7> HandlePositions() const;  // [center, X-, X+, Y-, Y+, Z-, Z+]
  QPointF WorldToScreen(const Vec3& p) const;   // logical-pixel projection
  float WorldDeltaAlongAxis(const Vec3& origin, const Vec3& axis,
                            const QPointF& mouseDelta) const;
  int PickHandle(const QPoint& pos) const;      // -1 none, 0 center, 1..6 faces
  void DragSelectionHandle(const QPointF& mouseDelta);  // move/resize active handle

  OrbitCamera camera_;
  Bounds bounds_;

  // CPU-side staging. The dirty flags defer every GPU upload to render() so the
  // setters stay safe to call before the rhi exists and never need makeCurrent.
  std::vector<float> lineData_;   // [x,y,z,r,g,b] * vertices (Lines)
  std::vector<float> cageData_;   // yellow cage, same layout (Lines)
  bool lineDirty_ = false;
  bool cageDirty_ = false;

  // Volume slices (3D texture sampled in the fragment shader; voxel-space quads).
  Volume pendingVolume_;          // staged until the rhi exists; cleared after upload
  std::vector<float> sliceData_;  // [x,y,z] voxel-coord quad vertices (Triangles)
  bool volumeDirty_ = false;      // re-upload the 3D texture + slice quads
  bool hasVolume_ = false;
  bool showVolume_ = true;
  Mat4 voxToWorld_;
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

  // ── RHI resources (owned; recreated on device loss). ──────────────────────
  QRhi* rhi_ = nullptr;

  // Line/cage/box share the line pipeline (Lines topology, [pos,rgb] layout).
  QRhiBuffer* lineVbo_ = nullptr;
  QRhiBuffer* cageVbo_ = nullptr;
  QRhiBuffer* boxVbo_ = nullptr;
  QRhiBuffer* handleVbo_ = nullptr;
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
  QRhiGraphicsPipeline* pointPs_ = nullptr;  // Points, no depth (handles on top)
  QRhiGraphicsPipeline* slicePs_ = nullptr;  // Triangles, alpha blend, depth no-write

  // Capacities currently allocated on the GPU; a grow re-creates the buffer.
  std::size_t lineVboCap_ = 0;
  std::size_t cageVboCap_ = 0;
  std::size_t boxVboCap_ = 0;
  std::size_t handleVboCap_ = 0;
  std::size_t sliceVboCap_ = 0;

  std::size_t lineVertexCount_ = 0;
  std::size_t cageVertexCount_ = 0;
  std::size_t boxVertexCount_ = 0;
  std::size_t handleVertexCount_ = 0;
  std::size_t sliceVertexCount_ = 0;

  bool volTexUploaded_ = false;   // the 3D texture matches volTex_ dims + content
};

}  // namespace tracto
