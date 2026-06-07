#pragma once

// Qt OpenGL viewport: render + interaction only. It owns no tractogram or edit
// state. The MainWindow hands it a ready-made interleaved [pos.xyz, rgb]
// GL_LINES buffer (built by display_geometry) and a bounds box; the viewport
// draws those lines plus a bounds cage under an orbit camera. Keeping data and
// editing out of here preserves the module boundary from CLAUDE.md.

#include "bounds.hpp"
#include "nifti_io.hpp"
#include "render_math.hpp"

#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLWidget>
#include <QPoint>
#include <QPointF>

#include <array>
#include <cstddef>
#include <vector>

namespace tracto {

class TractViewport : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core {
  Q_OBJECT
 public:
  explicit TractViewport(QWidget* parent = nullptr);
  ~TractViewport() override;

  // Replace the line buffer and frame the camera to `bounds`. Safe to call
  // before the GL context exists: the upload is deferred to the next paint.
  void SetLineGeometry(std::vector<float> interleaved, const Bounds& bounds);

  // Set the scalar background volume (uploaded once as a 3D texture; three mid
  // slices are rendered as affine-placed quads). Safe before the GL context.
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
  void initializeGL() override;
  void paintGL() override;
  void resizeGL(int w, int h) override;

  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;

 private:
  void UploadGeometry();      // (re)fill the line VBO from lineData_
  void RebuildCage();         // yellow bounding cage from bounds_
  void UploadVolume();        // 3D texture + slice quads from pendingVolume_

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

  std::vector<float> lineData_;   // [x,y,z,r,g,b] * vertices
  std::vector<float> cageData_;   // [x,y,z,r,g,b] * vertices (GL_LINES)
  bool lineDirty_ = false;

  // Line/cage program (pos + rgb).
  unsigned int program_ = 0;
  int mvpLoc_ = -1;
  unsigned int lineVao_ = 0;
  unsigned int lineVbo_ = 0;
  unsigned int cageVao_ = 0;
  unsigned int cageVbo_ = 0;
  std::size_t lineVertexCount_ = 0;
  std::size_t cageVertexCount_ = 0;

  // volume slice program (samples a 3D texture; voxel-space quads).
  Volume pendingVolume_;          // staged until the GL context exists
  bool volumeDirty_ = false;
  bool hasVolume_ = false;
  bool showVolume_ = true;
  unsigned int sliceProgram_ = 0;
  int sliceMvpLoc_ = -1;
  int sliceVoxToWorldLoc_ = -1;
  int sliceInvDimsLoc_ = -1;
  int sliceValueMinLoc_ = -1;
  int sliceValueRangeLoc_ = -1;
  unsigned int volTexture_ = 0;
  unsigned int sliceVao_ = 0;
  unsigned int sliceVbo_ = 0;
  std::size_t sliceVertexCount_ = 0;
  Mat4 voxToWorld_;
  Vec3 invDims_{1.0f, 1.0f, 1.0f};
  float volValueMin_ = 0.0f;
  float volValueRange_ = 1.0f;

  // Selection box (pos+rgb wireframe + GL_POINTS handles, drawn with program_).
  Bounds boxBounds_;
  bool hasBox_ = false;
  std::vector<float> boxData_;     // staged wireframe vertices
  std::vector<float> handleData_;  // staged handle-point vertices
  bool boxGeomDirty_ = false;      // upload deferred to paintGL (no per-drag makeCurrent)
  unsigned int boxVao_ = 0;
  unsigned int boxVbo_ = 0;
  unsigned int handleVao_ = 0;
  unsigned int handleVbo_ = 0;
  std::size_t boxVertexCount_ = 0;
  std::size_t handleVertexCount_ = 0;
  int pointSizeLoc_ = -1;
  int activeHandle_ = -1;  // -1 none; 0 center; 1..6 face handles (axis*2+side+1)

  QPoint lastPos_;
  bool rotating_ = false;
  bool panning_ = false;
};

}  // namespace tracto
