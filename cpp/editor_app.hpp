#pragma once

#include "args.hpp"
#include "bounds.hpp"
#include "selection_backend.hpp"
#include "tractogram_store.hpp"
#include "trk_io.hpp"

#include <vtkSmartPointer.h>
#include <vtkType.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class vtkActor;
class vtkBoxWidget;
class vtkImageActor;
class vtkPoints;
class vtkPolyData;
class vtkPolyDataMapper;
class vtkRenderWindow;
class vtkRenderWindowInteractor;
class vtkRenderer;
class vtkTextActor;
class vtkUnsignedCharArray;

namespace tracto {

enum class OpKind {
  DeleteInBox,
  KeepOnlyInBox,
};

struct EditOp {
  OpKind kind;
  Bounds bounds;
};

struct Snapshot {
  std::vector<uint8_t> aliveFull;
  std::vector<EditOp> ops;
  bool dirty = false;
};

class EditorApp {
 public:
  explicit EditorApp(Args args);

  void Load();
  void Run();

  void OnBoxChanged(vtkBoxWidget* widget);
  void OnKey(const std::string& key);

 private:
  void RebuildDisplayPolyData(bool render = false);
  void BuildFaActors();
  void AddReferenceGeometry();
  void RefreshLines(bool render = true);
  void UpdateStatus();
  void DeleteInBox();
  void KeepOnlyInBox();
  void Undo();
  void Reset();
  void ToggleFa();
  void PreviewBox();
  void PrintStats();
  void RecenterCamera();
  void DensityUp();
  void DensityDown();
  void DensitySet();
  void Save();
  void Quit();
  void PushSnapshot();
  void SetDisplayCap(int cap);
  std::vector<uint8_t> FullInBox(const Bounds& bounds) const;
  std::size_t CountAliveFull() const;
  std::size_t CountShownDisplay() const;
  Bounds PolyDataBounds() const;
  Bounds TractogramBounds() const;

  Args args_;
  TractogramStore tractogram_;
  std::unique_ptr<SelectionBackend> selectionBackend_;
  std::vector<int> displayToFull_;
  std::vector<uint8_t> aliveFull_;
  std::vector<EditOp> ops_;
  std::vector<Snapshot> history_;

  bool dirty_ = false;
  bool showFa_ = true;
  bool hasBox_ = false;
  int displayCap_ = 12000;
  Bounds boxBounds_;

  std::vector<std::vector<vtkIdType>> displayCells_;

  vtkSmartPointer<vtkPolyData> linePoly_;
  vtkSmartPointer<vtkPoints> linePoints_;
  vtkSmartPointer<vtkUnsignedCharArray> lineColors_;
  vtkSmartPointer<vtkPolyDataMapper> lineMapper_;
  vtkSmartPointer<vtkActor> lineActor_;

  vtkSmartPointer<vtkRenderer> renderer_;
  vtkSmartPointer<vtkRenderWindow> renderWindow_;
  vtkSmartPointer<vtkRenderWindowInteractor> interactor_;
  vtkSmartPointer<vtkTextActor> statusActor_;
  vtkSmartPointer<vtkBoxWidget> boxWidget_;
  std::vector<vtkSmartPointer<vtkImageActor>> faActors_;
};

}  // namespace tracto
