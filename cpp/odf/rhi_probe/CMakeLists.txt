# Standalone — NOT included by the repo root CMakeLists. Configure/build out-of-tree:
#   # compute shaders need >= ESSL 3.10 / GLSL 4.30, so NOT --qt6 (it targets 100es/120/150):
#   qsb --glsl 310es,430 --msl 12 -o /tmp/odf_rhi_build/deform_probe.comp.qsb cpp/odf/rhi_probe/deform_probe.comp
#   cmake -S cpp/odf/rhi_probe -B /tmp/odf_rhi_build -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qtbase
#   cmake --build /tmp/odf_rhi_build
#   /tmp/odf_rhi_build/rhi_probe /tmp/odf_rhi_build/deform_probe.comp.qsb
cmake_minimum_required(VERSION 3.16)
project(rhi_probe LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# QRhi lives in QtGui but its headers are "private" (shipped, not source-compatible
# guaranteed). On this brew qtbase the Qt6::GuiPrivate target is defined in a separate
# Qt6GuiPrivate package, so it must be requested as an explicit component.
find_package(Qt6 REQUIRED COMPONENTS Gui GuiPrivate Widgets)

add_executable(rhi_probe rhi_probe.cpp)
target_link_libraries(rhi_probe PRIVATE Qt6::Gui Qt6::GuiPrivate)

# GPU-vs-oracle check for the real ODF deform kernel. Compiles the verified CPU
# foundation (.cpp in the parent dir) + the RHI harness; needs zlib for the .nii.gz.
find_package(ZLIB REQUIRED)
add_executable(odf_deform_check
    odf_deform_check.cpp
    ../sh_basis.cpp
    ../icosphere.cpp
    ../odf_volume.cpp)
target_include_directories(odf_deform_check PRIVATE ..)
target_link_libraries(odf_deform_check PRIVATE Qt6::Gui Qt6::GuiPrivate ZLIB::ZLIB)

# First visible ODF render: CPU oracle glyph mesh -> Metal offscreen -> PNG.
add_executable(odf_render
    odf_render.cpp
    ../sh_basis.cpp
    ../icosphere.cpp
    ../odf_volume.cpp
    ../glyph_builder.cpp)
target_include_directories(odf_render PRIVATE ..)
target_link_libraries(odf_render PRIVATE Qt6::Gui Qt6::GuiPrivate ZLIB::ZLIB)

# Fully-GPU ODF render: compute deform (SSBO) + programmable-vertex-pulling instanced
# draw. Verifies GPU positions vs the CPU oracle and renders a PNG.
add_executable(odf_render_gpu
    odf_render_gpu.cpp
    ../sh_basis.cpp
    ../icosphere.cpp
    ../odf_volume.cpp
    ../glyph_builder.cpp)
target_include_directories(odf_render_gpu PRIVATE ..)
target_link_libraries(odf_render_gpu PRIVATE Qt6::Gui Qt6::GuiPrivate ZLIB::ZLIB)

# Interactive ODF viewer: QRhiWidget (Metal) + orbit camera running the fused pipeline.
add_executable(odf_viewer
    odf_viewer.cpp
    ../sh_basis.cpp
    ../icosphere.cpp
    ../odf_volume.cpp)
target_include_directories(odf_viewer PRIVATE ..)
target_link_libraries(odf_viewer PRIVATE Qt6::Gui Qt6::GuiPrivate Qt6::Widgets ZLIB::ZLIB)
