# Standalone — NOT included by the repo root CMakeLists. Isolated RHI migration
# increment: render streamlines via Qt RHI (Metal) offscreen. Build out-of-tree:
#   qsb --glsl 430 --msl 12 -o /tmp/tract_rhi_build/line.vert.qsb cpp/qt/rhi/line.vert
#   qsb --glsl 430 --msl 12 -o /tmp/tract_rhi_build/line.frag.qsb cpp/qt/rhi/line.frag
#   cmake -S cpp/qt/rhi -B /tmp/tract_rhi_build -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qtbase
#   cmake --build /tmp/tract_rhi_build
#   /tmp/tract_rhi_build/tract_rhi_probe data/ORedited.trk /tmp/tract_rhi_build out.png
cmake_minimum_required(VERSION 3.16)
project(tract_rhi LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Gui GuiPrivate)
find_package(ZLIB REQUIRED)

set(CORE ${CMAKE_CURRENT_SOURCE_DIR}/../../core)
add_executable(tract_rhi_probe
    tract_rhi_probe.cpp
    ${CORE}/trk_io.cpp
    ${CORE}/tractogram_store.cpp
    ${CORE}/display_geometry.cpp
    ${CORE}/streamline_ops.cpp
    ${CORE}/nifti_io.cpp
    ${CORE}/utils.cpp)
target_include_directories(tract_rhi_probe PRIVATE ${CORE})
target_link_libraries(tract_rhi_probe PRIVATE Qt6::Gui Qt6::GuiPrivate ZLIB::ZLIB)
