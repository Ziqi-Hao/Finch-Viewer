cmake_minimum_required(VERSION 3.18)

project(tractography_local_editor_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(VTK REQUIRED COMPONENTS
  CommonColor
  CommonCore
  CommonDataModel
  CommonExecutionModel
  FiltersCore
  ImagingColor
  ImagingCore
  InteractionStyle
  InteractionWidgets
  IOImage
  RenderingCore
  RenderingFreeType
  RenderingOpenGL2
)

find_package(OpenMP)

add_executable(local_editor_cpp
  local_editor.cpp
  cpp/args.cpp
  cpp/editor_app.cpp
  cpp/selection_backend.cpp
  cpp/statistics.cpp
  cpp/streamline_ops.cpp
  cpp/tractogram_store.cpp
  cpp/trk_io.cpp
  cpp/utils.cpp
)

target_include_directories(local_editor_cpp PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/cpp
)

target_link_libraries(local_editor_cpp PRIVATE ${VTK_LIBRARIES})

if(OpenMP_CXX_FOUND)
  target_link_libraries(local_editor_cpp PRIVATE OpenMP::OpenMP_CXX)
  target_compile_definitions(local_editor_cpp PRIVATE HAVE_OPENMP=1)
endif()

if(COMMAND vtk_module_autoinit)
  vtk_module_autoinit(
    TARGETS local_editor_cpp
    MODULES ${VTK_LIBRARIES}
  )
endif()
