cmake_minimum_required(VERSION 3.18)

project(tractography_local_editor_cpp LANGUAGES CXX C)
include(CTest)

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
  cpp/core/track_density.cpp
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

if(BUILD_TESTING)
  add_executable(core_selftest cpp/core/core_selftest.cpp)
  target_link_libraries(core_selftest PRIVATE tracto_core)
  add_test(NAME core_selftest COMMAND core_selftest)
endif()

# ── ODF foundation: pure-CPU SH-coefficient ODF load + glyph mesh build ──────
# Deliberately standalone (its own NIfTI parse + private math types; zlib + C++17
# stdlib only — no VTK, no Qt, no GL) so it stays unit-testable and reusable. Kept
# a separate lib from tracto_core to preserve that boundary; the Qt editor links
# both. See cpp/odf/odf_types.hpp for why the types are namespaced tracto::odf.
add_library(tracto_odf STATIC
  cpp/odf/odf_volume.cpp
  cpp/odf/sh_basis.cpp
  cpp/odf/icosphere.cpp
  cpp/odf/glyph_builder.cpp
  cpp/odf/discrete_sphere.cpp
)
target_include_directories(tracto_odf PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/cpp/odf)
target_link_libraries(tracto_odf PUBLIC ZLIB::ZLIB)

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

# ── Qt RHI editor (the primary editor; no VTK, no glfw/glad) ─────────────────
# The viewport is a QRhiWidget (Metal on macOS), so we need Qt6::GuiPrivate for
# <rhi/qrhi.h>; OpenGL/OpenGLWidgets are no longer used. Optional: builds only
# when Qt6 is found, so the project still configures without Qt installed.
find_package(Qt6 QUIET COMPONENTS Gui Widgets GuiPrivate)
if(Qt6_FOUND)
  # ── Step 0: compile the RHI shaders (.vert/.frag) to .qsb, then embed them in
  # a generated .qrc so the viewport can load ":/shaders/<name>.qsb". qsb is
  # located via find_program (don't rely on a manual qsb step).
  find_program(QSB_EXECUTABLE NAMES qsb
    HINTS /opt/homebrew/opt/qtshadertools/bin
          ${Qt6_DIR}/../../../bin
          ${Qt6_DIR}/../../../libexec)
  if(NOT QSB_EXECUTABLE)
    message(FATAL_ERROR "qsb not found (needed to build local_editor_qt shaders). "
                        "Install qtshadertools or set QSB_EXECUTABLE.")
  endif()

  set(QT_RHI_SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/cpp/qt/rhi)
  set(QT_RHI_SHADERS line.vert line.frag point.vert slice.vert slice.frag
                     glyph.vert glyph.frag)
  set(QT_RHI_QSB_DIR ${CMAKE_CURRENT_BINARY_DIR}/qt_shaders)
  file(MAKE_DIRECTORY ${QT_RHI_QSB_DIR})

  set(QT_RHI_QSB_FILES "")
  foreach(shader ${QT_RHI_SHADERS})
    set(src ${QT_RHI_SHADER_DIR}/${shader})
    set(out ${QT_RHI_QSB_DIR}/${shader}.qsb)
    add_custom_command(
      OUTPUT ${out}
      COMMAND ${QSB_EXECUTABLE} --glsl 430 --msl 12 -o ${out} ${src}
      DEPENDS ${src}
      COMMENT "qsb ${shader} -> ${shader}.qsb"
      VERBATIM)
    list(APPEND QT_RHI_QSB_FILES ${out})
  endforeach()

  # The .qsb are build-time outputs (the custom commands above); mark them so the
  # resource step knows they are generated, not missing.
  set_source_files_properties(${QT_RHI_QSB_FILES} PROPERTIES GENERATED TRUE)

  add_executable(local_editor_qt
    cpp/qt/main.cpp
    cpp/qt/main_window.cpp
    cpp/qt/tract_viewport.cpp
    cpp/qt/theme.cpp
    cpp/qt/viewport_hud.cpp
    cpp/qt/perf_overlay.cpp
    cpp/qt/properties_panel.cpp
    cpp/qt/layers_panel.cpp
    cpp/qt/histogram_widget.cpp
    cpp/qt/stat_colorbar.cpp
    cpp/qt/orientation_overlay.cpp
  )
  set_target_properties(local_editor_qt PROPERTIES AUTOMOC ON)

  # Embed the compiled .qsb as Qt resources, loaded at runtime as
  # ":/shaders/<name>.qsb" (BASE strips the binary dir, PREFIX adds /shaders).
  # Use qt6_add_resources rather than a hand-written .qrc + AUTORCC: this makes rcc
  # depend on the .qsb CONTENT, so it re-embeds whenever a shader recompiles and
  # only after the qsb custom commands run. A hand-rolled .qrc + AUTORCC silently
  # kept a STALE shader — AUTORCC re-runs on .qrc *text* changes (never, here, since
  # the file list is fixed), not on the referenced files, and ran before the qsb
  # rebuilt — so every edit to a .vert/.frag was a no-op until a clean rebuild.
  qt6_add_resources(local_editor_qt "rhi_shaders"
    PREFIX "/shaders"
    BASE ${QT_RHI_QSB_DIR}
    FILES ${QT_RHI_QSB_FILES})
  target_link_libraries(local_editor_qt PRIVATE
    tracto_core
    tracto_odf
    Qt6::Gui
    Qt6::Widgets
    Qt6::GuiPrivate
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
