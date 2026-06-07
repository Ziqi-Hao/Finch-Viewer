#pragma once

#include "args.hpp"
#include "bounds.hpp"
#include "tractogram_store.hpp"

#include <array>
#include <cstddef>
#include <vector>

namespace tracto {

class GlfwTractViewer {
 public:
  explicit GlfwTractViewer(Args args);

  void Load();
  void Run();

 private:
  void BuildGpuVertices();
  void ResetCamera();
  void RotateCamera(double dx, double dy);
  void ZoomCamera(double delta);
  void PanCamera(double dx, double dy);
  void DrawFrame(int width, int height);

  Args args_;
  TractogramStore tractogram_;
  std::vector<float> lineVertices_;
  std::vector<float> referenceVertices_;
  Bounds displayBounds_;

  std::array<float, 3> target_ = {0.0f, 0.0f, 0.0f};
  float radius_ = 1.0f;
  float distance_ = 3.0f;
  float yaw_ = 0.0f;
  float pitch_ = 0.22f;

  bool dragging_ = false;
  bool panning_ = false;
  double lastX_ = 0.0;
  double lastY_ = 0.0;

  unsigned int program_ = 0;
  unsigned int lineVao_ = 0;
  unsigned int lineVbo_ = 0;
  unsigned int refVao_ = 0;
  unsigned int refVbo_ = 0;
  unsigned int debugVao_ = 0;
  unsigned int debugVbo_ = 0;
  std::size_t lineVertexCount_ = 0;
  std::size_t refVertexCount_ = 0;
  std::size_t debugVertexCount_ = 0;
};

}  // namespace tracto
