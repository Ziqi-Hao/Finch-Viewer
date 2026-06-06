# C++ local editor

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

- `local_editor.cpp`: small executable entry point.
- `cpp/args.*`: command-line parsing.
- `cpp/trk_io.*`: TrackVis `.trk` loading/saving and RASMM conversion.
- `cpp/streamline_ops.*`: streamline geometry tests, display sampling, direction RGB.
- `cpp/editor_app.*`: VTK rendering, box widget, keyboard actions, save workflow.
- `cpp/utils.*`: small shared helpers.

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
