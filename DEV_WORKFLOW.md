# Development Workflow

Use the scripts in `tools/` as the stable command surface. This avoids PATH
drift between PowerShell sessions and keeps Codex from repeatedly hand-writing
fragile commands.

## Environment Check

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\check_env.ps1
```

Checks:

- CMake
- vcpkg
- installed VTK package
- Visual Studio C++ compiler
- built executable smoke test if present

## Build

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\build_release.ps1
```

This script:

- refreshes PATH from Machine and User environment variables
- falls back to `C:\Program Files\CMake\bin\cmake.exe`
- configures with `C:\vcpkg\scripts\buildsystems\vcpkg.cmake`
- passes the repo's `vcpkg-overlays` directory
- builds Release
- runs `local_editor_cpp.exe --help` as a smoke test

## Run

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\run_editor.ps1
```

Optional:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\run_editor.ps1 -BuildFirst -Out edited.trk -DisplayN 16000
```

You can also pass display decimation:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\run_editor.ps1 -DisplayN 16000 -DispStep 3
```

## Run GLFW Viewer

The GLFW/OpenGL viewer uses the same data/IO modules but bypasses VTK's Win32
render window path.

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\run_glfw_editor.ps1
```

For a lighter smoke test:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\run_glfw_editor.ps1 -DisplayN 300 -DispStep 8
```

Controls:

- left drag: rotate
- right or middle drag: pan
- mouse wheel: zoom
- `r`: reset camera
- `Esc`: quit

## Codex Rule

For this repo, prefer these script entry points over raw ad-hoc commands.

When the Windows sandbox fails to spawn PowerShell, rerun only the specific
script command with escalation. If persistent approval is offered, approve the
specific script prefix rather than broad commands like `powershell.exe` alone.

## Code Quality Rules

This toolbox must stay readable as it becomes faster. Do not let performance
work turn the codebase into a tangled pile.

- Keep module boundaries clear: data, I/O, compute, render, interaction, and
  diagnostics stay separate unless there is a strong reason to cross them.
- Keep UI code out of compute kernels and keep VTK types out of core data and
  algorithm modules whenever possible.
- Prefer small named functions and explicit data flow over hidden global state.
- Add abstractions only when they make a real boundary clearer, such as
  selection backends or render adapters.
- Optimize from the data layout outward: contiguous buffers, measurable
  algorithms, then CPU/OpenMP/CUDA backends.
- Every non-trivial performance change needs a simple verification path:
  compile, smoke test, and when possible a benchmark or count equivalence check.
- Keep compatibility with the Python reference behavior until we intentionally
  decide to change semantics.
- If a function starts mixing loading, rendering, editing, statistics, and
  user interaction, split it before adding more behavior.
