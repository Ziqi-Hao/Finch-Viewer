#include "editor_app.hpp"

#include "statistics.hpp"
#include "streamline_ops.hpp"
#include "utils.hpp"

#include <vtkActor.h>
#include <vtkBoxWidget.h>
#include <vtkCallbackCommand.h>
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkCommand.h>
#include <vtkImageActor.h>
#include <vtkImageData.h>
#include <vtkImageMapper3D.h>
#include <vtkImageMapToColors.h>
#include <vtkImageProperty.h>
#include <vtkInteractorStyleTrackballCamera.h>
#include <vtkLookupTable.h>
#include <vtkMatrix4x4.h>
#include <vtkNIFTIImageReader.h>
#include <vtkObject.h>
#include <vtkOpenGLRenderWindow.h>
#include <vtkPNGWriter.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>
#include <vtkTextActor.h>
#include <vtkTextProperty.h>
#include <vtkUnsignedCharArray.h>
#include <vtkWindowToImageFilter.h>
#include <vtk_glew.h>

#ifdef _WIN32
#include <vtkWin32OpenGLRenderWindow.h>
#include <vtkWin32RenderWindowInteractor.h>
#endif

#ifdef HAVE_OPENMP
#include <omp.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace tracto {
namespace {

std::string Trim(std::string value) {
  const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
  value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
  return value;
}

bool IsDensityUpKey(const std::string& key) {
  return key == "plus" || key == "equal" || key == "KP_Add" || key == "+";
}

bool IsDensityDownKey(const std::string& key) {
  return key == "minus" || key == "KP_Subtract" || key == "-";
}

bool ParseDisplayCapInput(const std::string& raw, std::size_t fullCount, int& out) {
  std::string text = Trim(raw);
  if (text.empty()) {
    return false;
  }

  try {
    if (text.back() == '%') {
      text.pop_back();
      const double pct = std::stod(Trim(text));
      out = static_cast<int>(std::llround((pct / 100.0) * static_cast<double>(fullCount)));
    } else {
      out = static_cast<int>(std::llround(std::stod(text)));
    }
  } catch (const std::exception&) {
    return false;
  }

  return true;
}

std::string FormatFixed(double value, int precision) {
  std::ostringstream ss;
  ss << std::fixed << std::setprecision(precision) << value;
  return ss.str();
}

std::string FormatBounds(const Bounds& bounds) {
  std::ostringstream ss;
  ss << std::fixed << std::setprecision(2)
     << "x[" << bounds.v[0] << ", " << bounds.v[1] << "] "
     << "y[" << bounds.v[2] << ", " << bounds.v[3] << "] "
     << "z[" << bounds.v[4] << ", " << bounds.v[5] << "]";
  return ss.str();
}

double Clamp01(double value) {
  return std::clamp(value, 0.0, 1.0);
}

void WriteRenderWindowPng(vtkRenderWindow* window, const std::string& path) {
  vtkSmartPointer<vtkWindowToImageFilter> capture =
      vtkSmartPointer<vtkWindowToImageFilter>::New();
  capture->SetInput(window);
  capture->ReadFrontBufferOff();
  capture->Update();

  vtkSmartPointer<vtkPNGWriter> writer = vtkSmartPointer<vtkPNGWriter>::New();
  writer->SetFileName(path.c_str());
  writer->SetInputConnection(capture->GetOutputPort());
  writer->Write();
}

void PrintRenderBackendInfo(vtkRenderWindow* window, vtkRenderWindowInteractor* interactor) {
  std::cout << "  render window: " << window->GetClassName() << "\n";
  std::cout << "  interactor   : " << interactor->GetClassName() << "\n";
  if (auto* glWindow = vtkOpenGLRenderWindow::SafeDownCast(window)) {
    int major = 0;
    int minor = 0;
    glWindow->GetOpenGLVersion(major, minor);
    std::cout << "  OpenGL       : " << major << "." << minor << "\n";
    const std::string support = glWindow->GetOpenGLSupportMessage();
    if (!support.empty()) {
      std::cout << "  GL support   : " << support << "\n";
    }
    if (const char* capabilities = glWindow->ReportCapabilities()) {
      std::istringstream lines(capabilities);
      std::string line;
      int printed = 0;
      while (printed < 8 && std::getline(lines, line)) {
        if (line.empty()) {
          continue;
        }
        std::cout << "  GL caps      : " << line << "\n";
        ++printed;
      }
    }
  }
}

void ConfigureOpenGLPresent(vtkRenderWindow* window, bool frontBuffer) {
  window->SetForceMakeCurrent();
  window->SwapBuffersOn();
  if (auto* glWindow = vtkOpenGLRenderWindow::SafeDownCast(window)) {
    if (frontBuffer) {
      glWindow->SetFrameBlitModeToBlitToCurrent();
    } else {
      glWindow->SetFrameBlitModeToBlitToHardware();
    }
    glWindow->SetFramebufferFlipY(false);
  }
}

}  // namespace

EditorApp::EditorApp(Args args)
    : args_(std::move(args)),
      selectionBackend_(CreateCpuSelectionBackend()),
      displayCap_(args_.displayN) {}

void EditorApp::Load() {
  std::cout << "Loading OR   : " << args_.trkPath << "\n";
  const auto start = std::chrono::steady_clock::now();
  tractogram_.streamlines = LoadTrk(args_.trkPath, tractogram_.header);
  BuildSoA(tractogram_);
  const auto stop = std::chrono::steady_clock::now();
  std::cout << "  " << FormatCount(tractogram_.StreamlineCount()) << " streamlines, "
            << FormatCount(tractogram_.TotalPointCount()) << " points";
  if (tractogram_.header.nCount > 0 &&
      tractogram_.header.nCount != static_cast<int32_t>(tractogram_.StreamlineCount())) {
    std::cout << " (header said " << tractogram_.header.nCount << ")";
  }
  std::cout << " in "
            << std::chrono::duration_cast<std::chrono::milliseconds>(stop - start).count()
            << " ms\n";

#ifdef HAVE_OPENMP
  std::cout << "  OpenMP threads: " << omp_get_max_threads() << "\n";
#else
  std::cout << "  OpenMP: not enabled by this build\n";
#endif
  std::cout << "  selection backend: " << selectionBackend_->Name() << "\n";

  aliveFull_.assign(tractogram_.StreamlineCount(), 1);

  std::cout << "  tract RAS bounds: " << FormatBounds(TractogramBounds()) << "\n";

  RebuildDisplayPolyData(false);
}

void EditorApp::RebuildDisplayPolyData(bool render) {
  displayToFull_ = MakeDisplayIndicesFromAlive(aliveFull_, displayCap_, args_.seed);

  std::cout << "Packing streamlines into VTK PolyData ...\n";

  std::size_t totalPoints = 0;
  for (int fullIndex : displayToFull_) {
    const std::size_t full = static_cast<std::size_t>(fullIndex);
    const std::size_t npts = static_cast<std::size_t>(tractogram_.pointCounts[full]);
    if (npts >= 2) {
      const auto step = static_cast<std::size_t>(args_.dispStep);
      totalPoints += std::max<std::size_t>(2, (npts + step - 1) / step);
    }
  }

  linePoints_ = vtkSmartPointer<vtkPoints>::New();
  linePoints_->SetDataTypeToFloat();
  linePoints_->Allocate(static_cast<vtkIdType>(totalPoints));

  lineColors_ = vtkSmartPointer<vtkUnsignedCharArray>::New();
  lineColors_->SetName("RGB");
  lineColors_->SetNumberOfComponents(3);
  lineColors_->Allocate(static_cast<vtkIdType>(totalPoints * 3));

  displayCells_.clear();
  displayCells_.reserve(displayToFull_.size());

  vtkIdType cursor = 0;
  for (int fullIndex : displayToFull_) {
    const Streamline& sl = tractogram_.streamlines[static_cast<std::size_t>(fullIndex)];
    std::vector<vtkIdType> cell;

    if (sl.pointCount >= 2) {
      const auto step = static_cast<std::size_t>(args_.dispStep);
      const auto decimatedCount =
          std::max<std::size_t>(2, (static_cast<std::size_t>(sl.pointCount) + step - 1) / step);
      // Read RAS points from the SoA cloud (rasPoints was removed in favour of
      // the single contiguous SoA representation).
      const std::size_t base =
          static_cast<std::size_t>(tractogram_.offsets[static_cast<std::size_t>(fullIndex)]);
      std::vector<float> displayPoints;
      displayPoints.reserve(decimatedCount * 3);
      for (int32_t p = 0; p < sl.pointCount; p += args_.dispStep) {
        const std::size_t src = base + static_cast<std::size_t>(p);
        displayPoints.push_back(tractogram_.x[src]);
        displayPoints.push_back(tractogram_.y[src]);
        displayPoints.push_back(tractogram_.z[src]);
      }
      if (displayPoints.size() < 6) {
        displayPoints.clear();
        const std::size_t last = base + static_cast<std::size_t>(sl.pointCount - 1);
        for (std::size_t src : {base, last}) {
          displayPoints.push_back(tractogram_.x[src]);
          displayPoints.push_back(tractogram_.y[src]);
          displayPoints.push_back(tractogram_.z[src]);
        }
      }

      const std::size_t displayCount = displayPoints.size() / 3;
      cell.resize(displayCount);
      for (std::size_t p = 0; p < displayCount; ++p) {
        const float* xyz = displayPoints.data() + p * 3;
        linePoints_->InsertNextPoint(xyz[0], xyz[1], xyz[2]);
        const auto rgb = DirectionRgbFromPoints(displayPoints.data(), displayCount, p);
        lineColors_->InsertNextTypedTuple(rgb.data());
        cell[p] = cursor++;
      }
    }

    displayCells_.push_back(std::move(cell));
  }

  linePoly_ = vtkSmartPointer<vtkPolyData>::New();
  linePoly_->SetPoints(linePoints_);
  linePoly_->GetPointData()->SetScalars(lineColors_);

  std::cout << "  " << FormatCount(displayToFull_.size()) << " streamlines  ("
            << FormatCount(static_cast<std::size_t>(linePoints_->GetNumberOfPoints()))
            << " points)\n";

  RefreshLines(false);
  std::cout << "  display bounds: " << FormatBounds(PolyDataBounds()) << "\n";
  if (lineMapper_) {
    lineMapper_->SetInputData(linePoly_);
    lineMapper_->Modified();
  }
  if (render && renderWindow_) {
    PresentFrame();
  }
}

void EditorApp::BuildVolumeActors() {
  std::cout << "Loading volume   : " << args_.volumePath << "\n";

  vtkSmartPointer<vtkNIFTIImageReader> reader = vtkSmartPointer<vtkNIFTIImageReader>::New();
  reader->SetFileName(args_.volumePath.c_str());
  reader->Update();

  vtkImageData* image = reader->GetOutput();
  if (!image) {
    throw std::runtime_error("failed to read NIfTI image");
  }

  int extent[6] = {0, 0, 0, 0, 0, 0};
  image->GetExtent(extent);
  double range[2] = {0.0, 1.0};
  image->GetScalarRange(range);

  std::cout << "  extent=[" << extent[0] << "," << extent[1] << "] x ["
            << extent[2] << "," << extent[3] << "] x ["
            << extent[4] << "," << extent[5] << "]  range=["
            << range[0] << "," << range[1] << "]\n";

  vtkSmartPointer<vtkMatrix4x4> imageToRas = vtkSmartPointer<vtkMatrix4x4>::New();
  imageToRas->Identity();
  vtkMatrix4x4* niftiMatrix = reader->GetSFormMatrix();
  if (!niftiMatrix) {
    niftiMatrix = reader->GetQFormMatrix();
  }
  if (niftiMatrix) {
    imageToRas->DeepCopy(niftiMatrix);
  } else {
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        imageToRas->SetElement(
            r, c, tractogram_.header.voxToRas[static_cast<std::size_t>(r) * 4 + c]);
      }
    }
  }

  // Use voxel-index coordinates locally; the affine places slices in RAS mm.
  image->SetOrigin(0.0, 0.0, 0.0);
  image->SetSpacing(1.0, 1.0, 1.0);

  vtkSmartPointer<vtkLookupTable> lut = vtkSmartPointer<vtkLookupTable>::New();
  lut->SetNumberOfTableValues(256);
  lut->SetRange(range[0], range[1]);
  lut->Build();
  for (int i = 0; i < 256; ++i) {
    const double t = static_cast<double>(i) / 255.0;
    const double alpha = t < 0.025 ? 0.0 : Clamp01(0.12 + 0.58 * t);
    lut->SetTableValue(i, t, t, t, alpha);
  }

  vtkSmartPointer<vtkImageMapToColors> colorMap =
      vtkSmartPointer<vtkImageMapToColors>::New();
  colorMap->SetInputData(image);
  colorMap->SetLookupTable(lut);
  colorMap->SetOutputFormatToRGBA();
  colorMap->Update();

  const int midX = (extent[0] + extent[1]) / 2;
  const int midY = (extent[2] + extent[3]) / 2;
  const int midZ = (extent[4] + extent[5]) / 2;

  auto addSlice = [&](int x0, int x1, int y0, int y1, int z0, int z1) {
    vtkSmartPointer<vtkImageActor> actor = vtkSmartPointer<vtkImageActor>::New();
    actor->GetMapper()->SetInputConnection(colorMap->GetOutputPort());
    actor->SetDisplayExtent(x0, x1, y0, y1, z0, z1);
    actor->SetUserMatrix(imageToRas);
    actor->GetProperty()->SetOpacity(1.0);
    actor->GetProperty()->SetInterpolationTypeToNearest();
    renderer_->AddActor(actor);
    volumeActors_.push_back(actor);
  };

  addSlice(midX, midX, extent[2], extent[3], extent[4], extent[5]);
  addSlice(extent[0], extent[1], midY, midY, extent[4], extent[5]);
  addSlice(extent[0], extent[1], extent[2], extent[3], midZ, midZ);
}

void EditorApp::Run() {
  renderer_ = vtkSmartPointer<vtkRenderer>::New();
  renderer_->SetBackground(0.035, 0.035, 0.04);

#ifdef _WIN32
  renderWindow_ = vtkSmartPointer<vtkWin32OpenGLRenderWindow>::New();
#else
  renderWindow_ = vtkSmartPointer<vtkRenderWindow>::New();
#endif
  renderWindow_->SetSize(1280, 900);
  renderWindow_->SetWindowName("SUBG08 OR editor (C++/VTK)");
  renderWindow_->SetMultiSamples(0);
  renderWindow_->SetUseSRGBColorSpace(false);
  ConfigureOpenGLPresent(renderWindow_, args_.frontBuffer);
  if (!args_.screenshotPath.empty()) {
    renderWindow_->OffScreenRenderingOn();
  }
  renderWindow_->AddRenderer(renderer_);

#ifdef _WIN32
  interactor_ = vtkSmartPointer<vtkWin32RenderWindowInteractor>::New();
#else
  interactor_ = vtkSmartPointer<vtkRenderWindowInteractor>::New();
#endif
  interactor_->SetRenderWindow(renderWindow_);

  vtkSmartPointer<vtkInteractorStyleTrackballCamera> style =
      vtkSmartPointer<vtkInteractorStyleTrackballCamera>::New();
  interactor_->SetInteractorStyle(style);

  if (args_.noVolume) {
    showVolume_ = false;
    std::cout << "Skipping volume slices (--no-fa)\n";
  } else {
    BuildVolumeActors();
  }

  lineMapper_ = vtkSmartPointer<vtkPolyDataMapper>::New();
  lineMapper_->SetInputData(linePoly_);
  lineMapper_->ScalarVisibilityOn();
  lineMapper_->SetScalarModeToUsePointData();
  lineMapper_->SetColorModeToDirectScalars();
  lineMapper_->SelectColorArray("RGB");

  lineActor_ = vtkSmartPointer<vtkActor>::New();
  lineActor_->SetMapper(lineMapper_);
  lineActor_->GetProperty()->SetLineWidth(1.6);
  lineActor_->GetProperty()->LightingOff();
  lineActor_->GetProperty()->SetAmbient(1.0);
  lineActor_->GetProperty()->SetDiffuse(0.0);
  lineActor_->GetProperty()->SetOpacity(1.0);
  renderer_->AddActor(lineActor_);

  AddReferenceGeometry();

  statusActor_ = vtkSmartPointer<vtkTextActor>::New();
  statusActor_->SetDisplayPosition(10, 10);
  statusActor_->GetTextProperty()->SetFontSize(15);
  statusActor_->GetTextProperty()->SetColor(1.0, 1.0, 1.0);
  renderer_->AddActor2D(statusActor_);
  UpdateStatus();

  boxWidget_ = vtkSmartPointer<vtkBoxWidget>::New();
  boxWidget_->SetInteractor(interactor_);
  boxWidget_->SetPlaceFactor(1.0);
  Bounds dataBounds = PolyDataBounds();
  boxWidget_->PlaceWidget(dataBounds.v);
  boxBounds_ = dataBounds;
  hasBox_ = true;
  boxWidget_->SetOutlineCursorWires(true);
  boxWidget_->GetOutlineProperty()->SetColor(1.0, 1.0, 0.0);

  vtkSmartPointer<vtkCallbackCommand> boxCallback =
      vtkSmartPointer<vtkCallbackCommand>::New();
  boxCallback->SetClientData(this);
  boxCallback->SetCallback([](vtkObject* caller, unsigned long, void* clientData, void*) {
    auto* app = static_cast<EditorApp*>(clientData);
    auto* widget = static_cast<vtkBoxWidget*>(caller);
    app->OnBoxChanged(widget);
  });
  boxWidget_->AddObserver(vtkCommand::InteractionEvent, boxCallback);
  boxWidget_->AddObserver(vtkCommand::EndInteractionEvent, boxCallback);
  boxWidget_->On();

  vtkSmartPointer<vtkCallbackCommand> keyCallback =
      vtkSmartPointer<vtkCallbackCommand>::New();
  keyCallback->SetClientData(this);
  keyCallback->SetCallback([](vtkObject* caller, unsigned long, void* clientData, void*) {
    auto* app = static_cast<EditorApp*>(clientData);
    auto* interactor = static_cast<vtkRenderWindowInteractor*>(caller);
    app->OnKey(interactor->GetKeySym());
  });
  interactor_->AddObserver(vtkCommand::KeyPressEvent, keyCallback);

  SetCameraToDataBounds();

  std::cout << "\nLaunching viewer ...\n";
  std::cout << "Hint: drag the yellow box handles to position it, then press 'd' or 'k'.\n";
  std::cout << "If the view looks empty, press 'c' or Home to recenter on streamlines.\n";
  if (args_.frontBuffer) {
    std::cout << "Display fallback: drawing directly to GL_FRONT (--front-buffer).\n";
  }

  interactor_->Initialize();
  interactor_->Enable();
  PresentFrame();
  PrintRenderBackendInfo(renderWindow_, interactor_);
  if (!args_.screenshotPath.empty()) {
    WriteRenderWindowPng(renderWindow_, args_.screenshotPath);
    std::cout << "screenshot -> " << args_.screenshotPath << "\n";
    return;
  }

  vtkSmartPointer<vtkCallbackCommand> startupRender =
      vtkSmartPointer<vtkCallbackCommand>::New();
  startupRender->SetClientData(this);
  startupRender->SetCallback([](vtkObject*, unsigned long, void* clientData, void*) {
    auto* app = static_cast<EditorApp*>(clientData);
    if (app->startupFramesRemaining_ > 0) {
      --app->startupFramesRemaining_;
      app->PresentFrame();
      return;
    }
    if (app->interactor_ && app->startupTimerId_ > 0) {
      app->interactor_->DestroyTimer(app->startupTimerId_);
      app->startupTimerId_ = 0;
    }
  });
  interactor_->AddObserver(vtkCommand::TimerEvent, startupRender);
  startupFramesRemaining_ = 40;
  startupTimerId_ = interactor_->CreateRepeatingTimer(50);
  interactor_->Start();
}

Bounds EditorApp::PolyDataBounds() const {
  Bounds b;
  if (linePoly_ && linePoly_->GetNumberOfPoints() > 0) {
    linePoly_->GetBounds(b.v);
  } else {
    b.v[0] = b.v[2] = b.v[4] = -1.0;
    b.v[1] = b.v[3] = b.v[5] = 1.0;
  }
  return b;
}

Bounds EditorApp::TractogramBounds() const {
  Bounds b;
  if (tractogram_.x.empty()) {
    b.v[0] = b.v[2] = b.v[4] = -1.0;
    b.v[1] = b.v[3] = b.v[5] = 1.0;
    return b;
  }

  const auto xMinMax = std::minmax_element(tractogram_.x.begin(), tractogram_.x.end());
  const auto yMinMax = std::minmax_element(tractogram_.y.begin(), tractogram_.y.end());
  const auto zMinMax = std::minmax_element(tractogram_.z.begin(), tractogram_.z.end());
  b.v[0] = *xMinMax.first;
  b.v[1] = *xMinMax.second;
  b.v[2] = *yMinMax.first;
  b.v[3] = *yMinMax.second;
  b.v[4] = *zMinMax.first;
  b.v[5] = *zMinMax.second;
  return b;
}

void EditorApp::AddReferenceGeometry() {
  if (!renderer_) {
    return;
  }

  const Bounds b = PolyDataBounds();
  const double cx = 0.5 * (b.v[0] + b.v[1]);
  const double cy = 0.5 * (b.v[2] + b.v[3]);
  const double cz = 0.5 * (b.v[4] + b.v[5]);
  const double dx = b.v[1] - b.v[0];
  const double dy = b.v[3] - b.v[2];
  const double dz = b.v[5] - b.v[4];
  const double axisLen = std::max({dx, dy, dz, 10.0});

  vtkSmartPointer<vtkPoints> points = vtkSmartPointer<vtkPoints>::New();
  points->SetDataTypeToFloat();

  auto addPoint = [&](double x, double y, double z) {
    return points->InsertNextPoint(x, y, z);
  };

  const vtkIdType corners[8] = {
      addPoint(b.v[0], b.v[2], b.v[4]), addPoint(b.v[1], b.v[2], b.v[4]),
      addPoint(b.v[1], b.v[3], b.v[4]), addPoint(b.v[0], b.v[3], b.v[4]),
      addPoint(b.v[0], b.v[2], b.v[5]), addPoint(b.v[1], b.v[2], b.v[5]),
      addPoint(b.v[1], b.v[3], b.v[5]), addPoint(b.v[0], b.v[3], b.v[5]),
  };
  const vtkIdType x0 = addPoint(cx, cy, cz);
  const vtkIdType x1 = addPoint(cx + axisLen, cy, cz);
  const vtkIdType y0 = addPoint(cx, cy, cz);
  const vtkIdType y1 = addPoint(cx, cy + axisLen, cz);
  const vtkIdType z0 = addPoint(cx, cy, cz);
  const vtkIdType z1 = addPoint(cx, cy, cz + axisLen);

  vtkSmartPointer<vtkCellArray> lines = vtkSmartPointer<vtkCellArray>::New();
  auto addLine = [&](vtkIdType a, vtkIdType c) {
    const vtkIdType ids[2] = {a, c};
    lines->InsertNextCell(2, ids);
  };

  addLine(corners[0], corners[1]);
  addLine(corners[1], corners[2]);
  addLine(corners[2], corners[3]);
  addLine(corners[3], corners[0]);
  addLine(corners[4], corners[5]);
  addLine(corners[5], corners[6]);
  addLine(corners[6], corners[7]);
  addLine(corners[7], corners[4]);
  addLine(corners[0], corners[4]);
  addLine(corners[1], corners[5]);
  addLine(corners[2], corners[6]);
  addLine(corners[3], corners[7]);
  addLine(x0, x1);
  addLine(y0, y1);
  addLine(z0, z1);

  vtkSmartPointer<vtkUnsignedCharArray> colors = vtkSmartPointer<vtkUnsignedCharArray>::New();
  colors->SetName("RGB");
  colors->SetNumberOfComponents(3);
  for (vtkIdType i = 0; i < points->GetNumberOfPoints(); ++i) {
    unsigned char rgb[3] = {255, 230, 0};
    if (i == x0 || i == x1) {
      rgb[0] = 255;
      rgb[1] = 40;
      rgb[2] = 40;
    } else if (i == y0 || i == y1) {
      rgb[0] = 40;
      rgb[1] = 255;
      rgb[2] = 40;
    } else if (i == z0 || i == z1) {
      rgb[0] = 80;
      rgb[1] = 160;
      rgb[2] = 255;
    }
    colors->InsertNextTypedTuple(rgb);
  }

  vtkSmartPointer<vtkPolyData> poly = vtkSmartPointer<vtkPolyData>::New();
  poly->SetPoints(points);
  poly->SetLines(lines);
  poly->GetPointData()->SetScalars(colors);

  vtkSmartPointer<vtkPolyDataMapper> mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
  mapper->SetInputData(poly);
  mapper->ScalarVisibilityOn();
  mapper->SetScalarModeToUsePointData();
  mapper->SetColorModeToDirectScalars();
  mapper->SelectColorArray("RGB");

  vtkSmartPointer<vtkActor> actor = vtkSmartPointer<vtkActor>::New();
  actor->SetMapper(mapper);
  actor->GetProperty()->SetLineWidth(3.0);
  actor->GetProperty()->LightingOff();
  actor->GetProperty()->SetAmbient(1.0);
  actor->GetProperty()->SetDiffuse(0.0);
  renderer_->AddActor(actor);
}

void EditorApp::OnBoxChanged(vtkBoxWidget* widget) {
  vtkSmartPointer<vtkPolyData> box = vtkSmartPointer<vtkPolyData>::New();
  widget->GetPolyData(box);
  box->GetBounds(boxBounds_.v);
  hasBox_ = true;
}

void EditorApp::OnKey(const std::string& key) {
  try {
    if (key == "d") {
      DeleteInBox();
    } else if (key == "k") {
      KeepOnlyInBox();
    } else if (key == "u") {
      Undo();
    } else if (key == "r") {
      Reset();
    } else if (key == "s") {
      Save();
    } else if (key == "h") {
      ToggleVolume();
    } else if (key == "p") {
      PreviewBox();
    } else if (key == "t") {
      PrintStats();
    } else if (key == "c" || key == "Home") {
      RecenterCamera();
    } else if (IsDensityUpKey(key)) {
      DensityUp();
    } else if (IsDensityDownKey(key)) {
      DensityDown();
    } else if (key == "n") {
      DensitySet();
    } else if (key == "q") {
      Quit();
    }
  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
  }
}

void EditorApp::PushSnapshot() {
  Snapshot s;
  s.aliveFull = aliveFull_;
  s.ops = ops_;
  s.dirty = dirty_;
  history_.push_back(std::move(s));
}

std::size_t EditorApp::CountAliveFull() const {
  return static_cast<std::size_t>(
      std::count(aliveFull_.begin(), aliveFull_.end(), static_cast<uint8_t>(1)));
}

std::size_t EditorApp::CountShownDisplay() const {
  std::size_t count = 0;
  for (int fullIndex : displayToFull_) {
    if (aliveFull_[static_cast<std::size_t>(fullIndex)]) {
      ++count;
    }
  }
  return count;
}

std::vector<uint8_t> EditorApp::FullInBox(const Bounds& bounds) const {
  return selectionBackend_->SelectInBox(tractogram_, bounds);
}

void EditorApp::DeleteInBox() {
  if (!hasBox_) {
    std::cout << "Move the box first.\n";
    return;
  }

  const std::vector<uint8_t> inBox = FullInBox(boxBounds_);
  std::size_t nKill = 0;
  for (std::size_t i = 0; i < inBox.size(); ++i) {
    if (inBox[i] && aliveFull_[i]) {
      ++nKill;
    }
  }

  if (nKill == 0) {
    std::cout << "box contains 0 alive streamlines\n";
    return;
  }

  PushSnapshot();
  for (std::size_t i = 0; i < inBox.size(); ++i) {
    if (inBox[i]) {
      aliveFull_[i] = 0;
    }
  }
  ops_.push_back({OpKind::DeleteInBox, boxBounds_});
  dirty_ = true;

  std::cout << "deleted " << nKill << "  (alive "
            << FormatCount(CountAliveFull()) << "/"
            << FormatCount(tractogram_.StreamlineCount()) << ")\n";
  RebuildDisplayPolyData(true);
}

void EditorApp::KeepOnlyInBox() {
  if (!hasBox_) {
    std::cout << "Move the box first.\n";
    return;
  }

  const std::vector<uint8_t> inBox = FullInBox(boxBounds_);
  std::size_t nKeep = 0;
  for (std::size_t i = 0; i < inBox.size(); ++i) {
    if (inBox[i] && aliveFull_[i]) {
      ++nKeep;
    }
  }

  if (nKeep == 0) {
    std::cout << "box contains 0 alive streamlines\n";
    return;
  }

  PushSnapshot();
  for (std::size_t i = 0; i < inBox.size(); ++i) {
    aliveFull_[i] = static_cast<uint8_t>(aliveFull_[i] && inBox[i]);
  }
  ops_.push_back({OpKind::KeepOnlyInBox, boxBounds_});
  dirty_ = true;

  std::cout << "kept " << nKeep << "  (alive "
            << FormatCount(CountAliveFull()) << "/"
            << FormatCount(tractogram_.StreamlineCount()) << ")\n";
  RebuildDisplayPolyData(true);
}

void EditorApp::Undo() {
  if (history_.empty()) {
    std::cout << "nothing to undo\n";
    return;
  }

  Snapshot s = std::move(history_.back());
  history_.pop_back();
  aliveFull_ = std::move(s.aliveFull);
  ops_ = std::move(s.ops);
  dirty_ = s.dirty;

  std::cout << "undo -> alive " << FormatCount(CountAliveFull()) << "/"
            << FormatCount(tractogram_.StreamlineCount()) << "\n";
  RebuildDisplayPolyData(true);
}

void EditorApp::Reset() {
  PushSnapshot();
  std::fill(aliveFull_.begin(), aliveFull_.end(), static_cast<uint8_t>(1));
  ops_.clear();
  dirty_ = false;
  std::cout << "reset\n";
  RebuildDisplayPolyData(true);
}

void EditorApp::ToggleVolume() {
  if (volumeActors_.empty()) {
    std::cout << "no volume actors loaded\n";
    return;
  }
  showVolume_ = !showVolume_;
  for (auto& actor : volumeActors_) {
    actor->SetVisibility(showVolume_ ? 1 : 0);
  }
  PresentFrame();
}

void EditorApp::PreviewBox() {
  if (!hasBox_) {
    std::cout << "Move the box first.\n";
    return;
  }

  const std::vector<uint8_t> inBox = FullInBox(boxBounds_);
  std::size_t n = 0;
  for (std::size_t i = 0; i < inBox.size(); ++i) {
    if (inBox[i] && aliveFull_[i]) {
      ++n;
    }
  }

  const std::size_t alive = std::max<std::size_t>(1, CountAliveFull());
  const double pct = 100.0 * static_cast<double>(n) / static_cast<double>(alive);
  std::cout << "box holds " << FormatCount(n) << " alive streamlines ("
            << FormatFixed(pct, 1)
            << "% of alive) -> 'd' deletes them, 'k' keeps only them\n";
}

void EditorApp::PrintStats() {
  const BasicStats stats = ComputeBasicStats(tractogram_, aliveFull_);
  if (stats.aliveCount == 0) {
    std::cout << "no alive streamlines\n";
    return;
  }

  std::size_t boxAlive = 0;
  if (hasBox_) {
    const std::vector<uint8_t> inBox = FullInBox(boxBounds_);
    for (std::size_t i = 0; i < inBox.size(); ++i) {
      if (inBox[i] && aliveFull_[i]) {
        ++boxAlive;
      }
    }
  }

  std::ostringstream ss;
  ss << "\n-------- STATISTICS (surviving full set) --------\n"
     << "  streamlines : alive " << FormatCount(stats.aliveCount) << "/"
     << FormatCount(stats.fullCount) << "   deleted " << FormatCount(stats.deadCount)
     << " (" << FormatFixed(stats.deletedPercent, 1) << "%)\n"
     << "  length (mm) : mean " << FormatFixed(stats.lengthMm.mean, 1)
     << "  median " << FormatFixed(stats.lengthMm.median, 1)
     << "  sd " << FormatFixed(stats.lengthMm.stddev, 1)
     << "  min " << FormatFixed(stats.lengthMm.min, 1)
     << "  max " << FormatFixed(stats.lengthMm.max, 1) << "\n"
     << "  points/line : mean " << FormatFixed(stats.pointsPerLine.mean, 1)
     << "  min " << static_cast<int64_t>(std::llround(stats.pointsPerLine.min))
     << "  max " << static_cast<int64_t>(std::llround(stats.pointsPerLine.max)) << "\n";

  if (hasBox_) {
    ss << "  in curr box : " << FormatCount(boxAlive) << " alive streamlines\n";
  }

  ss << "  display cap : " << FormatCount(static_cast<std::size_t>(displayCap_))
     << "  (shown now: " << FormatCount(CountShownDisplay()) << ")\n"
     << "--------------------------------------------------\n";

  std::cout << ss.str();
}

void EditorApp::RecenterCamera() {
  SetCameraToDataBounds();
  PresentFrame();
}

void EditorApp::SetCameraToDataBounds() {
  if (!renderer_) {
    return;
  }

  const Bounds bounds = PolyDataBounds();
  const double cx = 0.5 * (bounds.v[0] + bounds.v[1]);
  const double cy = 0.5 * (bounds.v[2] + bounds.v[3]);
  const double cz = 0.5 * (bounds.v[4] + bounds.v[5]);
  const double dx = bounds.v[1] - bounds.v[0];
  const double dy = bounds.v[3] - bounds.v[2];
  const double dz = bounds.v[5] - bounds.v[4];
  const double radius = std::max(1.0, 0.5 * std::sqrt(dx * dx + dy * dy + dz * dz));

  vtkCamera* camera = renderer_->GetActiveCamera();
  camera->SetFocalPoint(cx, cy, cz);
  camera->SetPosition(cx, cy - 3.2 * radius, cz + 0.45 * radius);
  camera->SetViewUp(0.0, 0.0, 1.0);
  renderer_->ResetCamera(bounds.v);
  renderer_->ResetCameraClippingRange(bounds.v);
}

void EditorApp::PresentFrame() {
  if (!renderWindow_) {
    return;
  }

  ConfigureOpenGLPresent(renderWindow_, args_.frontBuffer);
  if (args_.frontBuffer) {
    if (auto* glWindow = vtkOpenGLRenderWindow::SafeDownCast(renderWindow_)) {
      glWindow->MakeCurrent();
      glDrawBuffer(GL_FRONT);
    }
  }
  renderWindow_->Render();
  if (args_.frontBuffer) {
    if (auto* glWindow = vtkOpenGLRenderWindow::SafeDownCast(renderWindow_)) {
      glWindow->MakeCurrent();
      glDrawBuffer(GL_FRONT);
    }
    renderWindow_->Frame();
    glFlush();
    glFinish();
  } else if (renderWindow_->GetMapped()) {
    renderWindow_->Frame();
  }
}

void EditorApp::SetDisplayCap(int cap) {
  const int fullCount = static_cast<int>(tractogram_.StreamlineCount());
  displayCap_ = std::clamp(cap, 1, std::max(1, fullCount));
  std::cout << "display cap -> " << FormatCount(static_cast<std::size_t>(displayCap_)) << "\n";
  RebuildDisplayPolyData(true);
}

void EditorApp::DensityUp() {
  const int next = static_cast<int>(std::floor(static_cast<double>(displayCap_) * 1.5)) + 1;
  SetDisplayCap(next);
}

void EditorApp::DensityDown() {
  const int next =
      std::max(200, static_cast<int>(std::floor(static_cast<double>(displayCap_) / 1.5)));
  SetDisplayCap(next);
}

void EditorApp::DensitySet() {
  std::cout << "display how many streamlines (number or %, e.g. 8000 or 25%): ";
  std::string raw;
  if (!std::getline(std::cin, raw)) {
    std::cout << "\ninput unavailable\n";
    return;
  }

  int cap = 0;
  if (!ParseDisplayCapInput(raw, tractogram_.StreamlineCount(), cap)) {
    std::cout << "could not parse: " << raw << "\n";
    return;
  }

  SetDisplayCap(cap);
}

void EditorApp::Save() {
  const auto start = std::chrono::steady_clock::now();
  std::cout << "saving current full-set alive mask for "
            << FormatCount(tractogram_.StreamlineCount()) << " streamlines ...\n";

  std::vector<uint8_t> keep = aliveFull_;
  const std::size_t nOut = static_cast<std::size_t>(
      std::count(keep.begin(), keep.end(), static_cast<uint8_t>(1)));

  std::cout << "  full-set survivors: " << FormatCount(nOut) << "/"
            << FormatCount(tractogram_.StreamlineCount()) << "\n";

  if (!WriteTrkSubset(args_.outPath, tractogram_.header, tractogram_.streamlines, keep)) {
    throw std::runtime_error("failed while writing output .trk");
  }

  const auto stop = std::chrono::steady_clock::now();
  dirty_ = false;
  std::cout << "saved -> " << args_.outPath << "  ("
            << std::chrono::duration_cast<std::chrono::milliseconds>(stop - start).count()
            << " ms)\n";
  UpdateStatus();
}

void EditorApp::Quit() {
  if (dirty_) {
    std::cout << "WARNING: unsaved edits. Press s first, or close the terminal to abandon them.\n";
  }
  interactor_->TerminateApp();
}

void EditorApp::RefreshLines(bool render) {
  vtkSmartPointer<vtkCellArray> lines = vtkSmartPointer<vtkCellArray>::New();
  for (std::size_t i = 0; i < displayCells_.size(); ++i) {
    const std::size_t fullIndex = static_cast<std::size_t>(displayToFull_[i]);
    if (!aliveFull_[fullIndex] || displayCells_[i].size() < 2) {
      continue;
    }
    lines->InsertNextCell(static_cast<vtkIdType>(displayCells_[i].size()),
                          displayCells_[i].data());
  }
  linePoly_->SetLines(lines);
  linePoly_->Modified();
  if (lineMapper_) {
    lineMapper_->Modified();
  }
  UpdateStatus();
  if (render && renderWindow_) {
    PresentFrame();
  }
}

void EditorApp::UpdateStatus() {
  if (!statusActor_) {
    return;
  }
  std::ostringstream ss;
  ss << "alive: " << FormatCount(CountAliveFull()) << "/"
     << FormatCount(tractogram_.StreamlineCount())
     << "   shown: " << FormatCount(CountShownDisplay())
     << "   cap: " << FormatCount(static_cast<std::size_t>(displayCap_))
     << "   step: " << args_.dispStep << "\n"
     << "d=delete in box  k=keep only in box  u=undo  r=reset  "
     << "s=save  h=toggle volume  p=preview  t=stats  c=recenter  +/-=density  n=set density  q=quit";
  statusActor_->SetInput(ss.str().c_str());
}

}  // namespace tracto
