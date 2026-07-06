cmake_minimum_required(VERSION 3.18)

project(tractography_local_editor_cpp LANGUAGES CXX C)

# Refuse in-source builds: running cmake in the repo root scatters CMakeCache.txt,
# CMakeFiles/, Makefile, etc. across the tree. Always build into ./build.
if(CMAKE_BINARY_DIR STREQUAL CMAKE_SOURCE_DIR)
  message(FATAL_ERROR "In-source builds are not allowed. Use: cmake -S . -B build")
endif()

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Source layout:
#   cpp/core   shared, UI-free modules (io / data / compute)  -> tracto_core
#   cpp/odf    pure-CPU ODF load + glyph mesh build            -> tracto_odf
#   cpp/qt     Qt + RHI (Metal) editor — the only app          -> local_editor_qt
# The app links tracto_core + tracto_odf; the static-lib linker pulls in only
# the objects it actually references.

find_package(ZLIB REQUIRED)   # NIfTI .nii.gz decompression (cpp/core/nifti_io)

# OpenMP: Apple Clang ships no OpenMP runtime and FindOpenMP can't auto-discover
# the `-Xclang -fopenmp` + Homebrew libomp combo, so it silently leaves
# selection_backend's #pragma omp paths single-threaded. Spell the flags out so
# find_package(OpenMP) validates and lights them up. (No-op off macOS / no libomp.)
if(APPLE AND NOT OpenMP_CXX_FLAGS)
  execute_process(COMMAND brew --prefix libomp
                  OUTPUT_VARIABLE _brew_libomp
                  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  # Prefer the STATIC libomp.a so the shipped Finch-Viewer.app self-contains the
  # OpenMP runtime: end users need NO libomp install and there's nothing extra to
  # bundle. Fall back to the dylib; absent entirely -> serial build (no error).
  set(_omp_lib "")
  if(_brew_libomp AND EXISTS "${_brew_libomp}/lib/libomp.a")
    set(_omp_lib "${_brew_libomp}/lib/libomp.a")
  elseif(_brew_libomp AND EXISTS "${_brew_libomp}/lib/libomp.dylib")
    set(_omp_lib "${_brew_libomp}/lib/libomp.dylib")
  endif()
  if(_omp_lib)
    set(OpenMP_C_FLAGS "-Xclang -fopenmp -I${_brew_libomp}/include")
    set(OpenMP_C_LIB_NAMES "omp")
    set(OpenMP_CXX_FLAGS "-Xclang -fopenmp -I${_brew_libomp}/include")
    set(OpenMP_CXX_LIB_NAMES "omp")
    set(OpenMP_omp_LIBRARY "${_omp_lib}")
  endif()
endif()
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

  # Manual-run perf check (no add_test — needs a .trk path argument). Documents
  # the OpenMP win on the parallelized selection scan; see the file header.
  add_executable(selection_bench cpp/core/selection_bench.cpp)
  target_link_libraries(selection_bench PRIVATE tracto_core)
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
  cpp/odf/glyph_scene.cpp
)
target_include_directories(tracto_odf PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/cpp/odf)
target_link_libraries(tracto_odf PUBLIC ZLIB::ZLIB)

if(BUILD_TESTING)
  add_executable(odf_selftest cpp/odf/odf_selftest.cpp)
  target_link_libraries(odf_selftest PRIVATE tracto_odf)
  add_test(NAME odf_selftest COMMAND odf_selftest)
endif()

# ── Qt RHI editor (the only C++ editor; pure Qt + RHI, no VTK/GL) ────────────
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
      COMMAND ${QSB_EXECUTABLE} --glsl 430 --msl 12 --hlsl 50 -o ${out} ${src}
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
  # Build as a user-facing, deployable app per platform: a Finch-Viewer.app bundle
  # on macOS (so macdeployqt can bundle Qt into it), a GUI exe (no console) on
  # Windows. The CMake TARGET name stays local_editor_qt (build scripts unchanged);
  # only the produced artifact is renamed.
  set_target_properties(local_editor_qt PROPERTIES
    AUTOMOC ON
    OUTPUT_NAME "Finch-Viewer"
    MACOSX_BUNDLE ON
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE_BUNDLE_NAME "Finch-Viewer"
    MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.ziqihao.finch-viewer"
    MACOSX_BUNDLE_BUNDLE_VERSION "0.1.0"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0")

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
