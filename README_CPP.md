# C++ local editor

> **macOS / Qt:** the active C++ editor is now `local_editor_qt`, a pure
> **Qt + OpenGL** app (no VTK). See the *macOS (Homebrew)* section of
> [DEV_WORKFLOW.md](DEV_WORKFLOW.md). The VTK editor below is legacy and
> off by default (`-DBUILD_VTK_EDITOR=ON` to build it).

This is a C++/VTK port of `local_editor.py`.

It keeps the same interactive workflow:

- mouse rotate/pan/zoom
- box widget selection
- `d` delete streamlines inside the box
- `k` keep only streamlines inside the box
- `u` undo
- `r` reset
- `s` save
- `h` toggle FA slices
- `q` quit

Differences from the Python implementation:

- Streamlines are stored in contiguous C++ data structures.
- Box tests and full-set save filtering use OpenMP when the compiler supports it.
- Rendering uses VTK's OpenGL backend directly.
- Save filtering records every box operation and replays it exactly on the full `.trk` set.
- FA slices are placed with the real NIfTI/TrackVis affine, including negative axes.

## Layout

All C++ lives under `cpp/`, split into a shared library plus one folder per app:

- `cpp/core/` — the UI-free io/data/compute library (`tracto_core`): `args`,
  `trk_io` (TrackVis `.trk` load/save + RASMM), `tractogram_store`,
  `streamline_ops`, `selection_backend`, `statistics`, `display_geometry`,
  `nifti_io` (VTK-free NIfTI reader), `render_math`, `utils`.
- `cpp/qt/` — the primary Qt + OpenGL editor (`main.cpp`, `main_window`, `tract_viewport`).
- `cpp/glfw/` — the GLFW + OpenGL viewer (`main.cpp`, `glfw_tract_viewer`).
- `cpp/vtk/` — the legacy VTK editor described below (`main.cpp`, `editor_app`), opt-in.

Each app links `tracto_core` and adds only its own renderer/UI.

## Build

Install CMake, a C++17 compiler, and VTK development libraries.

On Windows, one practical route is Visual Studio 2022 plus VTK from vcpkg:

```powershell
winget install Kitware.CMake
git clone https://github.com/microsoft/vcpkg C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
C:\vcpkg\vcpkg.exe install vtk:x64-windows
```

Then build:

```powershell
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
```

If `libaec` fails to download from GitLab while installing VTK, this repo includes
`vcpkg-overlays/libaec`, which uses the same `libaec` version from the upstream
GitHub mirror:

```powershell
C:\vcpkg\vcpkg.exe install vtk:x64-windows --overlay-ports=.\vcpkg-overlays --clean-after-build
```

## Run

```powershell
.\build\Release\local_editor_cpp.exe `
  --fa SUBG08_tissue_FA_aggressive.nii.gz `
  --trk SUBG08_OR_full.trk `
  --out SUBG08_OR_edited.trk
```

Optional arguments:

```text
--display-n 12000
--seed 0
```
