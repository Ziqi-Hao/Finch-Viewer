cmake_minimum_required(VERSION 3.18)

project(tractography_local_editor_cpp LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Source layout:
#   cpp/core   shared, UI-free modules (io / data / compute)  -> tracto_core
#   cpp/qt     Qt + OpenGL editor                              -> local_editor_qt
#   cpp/glfw   GLFW + OpenGL viewer                            -> local_editor_glfw
#   cpp/vtk    legacy VTK editor (opt-in)                      -> local_editor_cpp
# Apps link tracto_core; the static-lib linker pulls in only the objects each
# app actually references.

find_package(ZLIB REQUIRED)   # NIfTI .nii.gz decompression (cpp/core/nifti_io)
find_package(OpenMP)

# ── core library: the VTK/Qt-free io/data/compute modules ───────────────────
add_library(tracto_core STATIC
  cpp/core/args.cpp
  cpp/core/trk_io.cpp
  cpp/core/tractogram_store.cpp
  cpp/core/streamline_ops.cpp
  cpp/core/selection_backend.cpp
  cpp/core/statistics.cpp
  cpp/core/display_geometry.cpp
  cpp/core/nifti_io.cpp
  cpp/core/utils.cpp
)

target_include_directories(tracto_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/cpp/core
)

target_link_libraries(tracto_core PUBLIC ZLIB::ZLIB)

if(OpenMP_CXX_FOUND)
  target_link_libraries(tracto_core PUBLIC OpenMP::OpenMP_CXX)
  target_compile_definitions(tracto_core PUBLIC HAVE_OPENMP=1)
endif()

# ── vendored GL loader + GLFW (for the GLFW viewer / probe) ──────────────────
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(dmri-explorer/deps/glfw-3.3.6 EXCLUDE_FROM_ALL)

add_library(tracto_glad STATIC
  dmri-explorer/deps/glad/src/glad.c
)
target_include_directories(tracto_glad PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/dmri-explorer/deps/glad/include
)

# ── GLFW/OpenGL viewer + context probe ──────────────────────────────────────
add_executable(glfw_probe tools/glfw_probe.cpp)
target_link_libraries(glfw_probe PRIVATE glfw tracto_glad)

add_executable(local_editor_glfw
  cpp/glfw/main.cpp
  cpp/glfw/glfw_tract_viewer.cpp
)
target_link_libraries(local_editor_glfw PRIVATE tracto_core glfw tracto_glad)
if(WIN32)
  target_link_libraries(local_editor_glfw PRIVATE gdi32)
endif()

# ── Qt/OpenGL editor (the primary editor; no VTK, no glfw/glad) ──────────────
# Optional: builds only when Qt6 is found, so the project still configures
# without Qt installed.
find_package(Qt6 QUIET COMPONENTS Widgets OpenGL OpenGLWidgets)
if(Qt6_FOUND)
  add_executable(local_editor_qt
    cpp/qt/main.cpp
    cpp/qt/main_window.cpp
    cpp/qt/tract_viewport.cpp
    cpp/qt/theme.cpp
    cpp/qt/viewport_hud.cpp
    cpp/qt/properties_panel.cpp
    cpp/qt/scene_panel.cpp
  )
  set_target_properties(local_editor_qt PROPERTIES AUTOMOC ON)
  target_link_libraries(local_editor_qt PRIVATE
    tracto_core
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
  )
else()
  message(STATUS "Qt6 not found: skipping local_editor_qt target")
endif()

# ── Legacy VTK editor (opt-in) ──────────────────────────────────────────────
# Off by default: Homebrew's VTK 9.6 dropped the bundled <vtk_glew.h> that
# cpp/vtk/editor_app.cpp uses, so it does not build out-of-the-box on macOS.
# Enable on Windows/vcpkg (or after porting the raw-GL include) with
# -DBUILD_VTK_EDITOR=ON.
option(BUILD_VTK_EDITOR "Build the legacy VTK editor (local_editor_cpp)" OFF)
if(BUILD_VTK_EDITOR)
  find_package(VTK REQUIRED COMPONENTS
    CommonColor CommonCore CommonDataModel CommonExecutionModel
    FiltersCore ImagingColor ImagingCore InteractionStyle InteractionWidgets
    IOImage RenderingCore RenderingFreeType RenderingOpenGL2 RenderingUI
  )
  add_executable(local_editor_cpp
    cpp/vtk/main.cpp
    cpp/vtk/editor_app.cpp
  )
  target_link_libraries(local_editor_cpp PRIVATE tracto_core ${VTK_LIBRARIES})
  if(COMMAND vtk_module_autoinit)
    vtk_module_autoinit(TARGETS local_editor_cpp MODULES ${VTK_LIBRARIES})
  endif()
endif()
