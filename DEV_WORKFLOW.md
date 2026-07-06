# Development Workflow

Use the scripts in `tools/` as the stable command surface rather than
hand-writing build/run commands.

## macOS (Homebrew) — Qt RHI editor

The C++ editor is `local_editor_qt`, a pure **Qt + RHI** app (Metal on macOS;
no VTK, no OpenGL). One-time toolchain install:

```bash
brew install cmake ninja qt libomp   # Qt6 (qtbase + qtshadertools `qsb`) + OpenMP runtime
```

`libomp` is a **build-time** dependency only: it makes the parallel compute paths
(e.g. the selection backend) multi-threaded. Apple Clang ships no OpenMP runtime,
so without libomp `find_package(OpenMP)` fails and those `#pragma omp` loops run
serially (build still succeeds — it just degrades). We link the **static**
`libomp.a`, so the shipped `Finch-Viewer.app` self-contains the OpenMP runtime —
**end users need nothing installed**. The `selection_bench` target measures the
win (~5–7× on a 24M-point tractogram).

Build and run via the mac scripts:

```bash
./tools/build_mac.sh                  # configure (Ninja) + build
./tools/build_mac.sh local_editor_qt  # build just the Qt editor
./tools/run_qt_editor.sh --trk in.trk --volume FA.nii.gz   # every flag optional; File menu also loads
```

The scalar/volume `.nii.gz` must be in the **same space** as the tractogram, or
its slices land off-screen; the editor warns when their bounds don't overlap.
`--screenshot out.png` renders one frame headless and exits (used for verifying).

Controls: left-drag rotate · right/middle-drag pan · wheel zoom · `R` reset ·
`H` toggle volume slices.

> **Windows:** the Qt editor is cross-platform, but only the macOS build is
> scripted today. The former VTK + vcpkg Windows workflow (and its
> `vcpkg-overlays/` and PowerShell scripts) was retired together with the legacy
> VTK editor.

## Packaging for distribution (macOS)

`run_qt_editor.sh` runs straight from `build/` against your Homebrew Qt — no
packaging needed for dev. To ship the app to a Mac **without** Homebrew Qt:

```bash
./tools/package_mac.sh          # -> build/Finch-Viewer.app (self-contained)
./tools/package_mac.sh --dmg    # also -> build/Finch-Viewer.dmg
```

This builds Release, bundles Qt (frameworks + plugins) into the `.app` via
`macdeployqt`, and ad-hoc codesigns it. The result is fully self-contained — Qt
is bundled and libomp is statically linked, so **the end user installs nothing**
(verified: `otool -L` shows no `/opt/homebrew`, and dyld loads every framework
from inside the bundle). The deploy runs in a `$TMPDIR` staging dir because an
iCloud-synced `~/Documents` re-adds `com.apple.FinderInfo` mid-sign and breaks
codesign; `$TMPDIR` isn't synced, so signing there is deterministic.

Gatekeeper: the bundle is only **ad-hoc** signed (no Apple Developer ID), so a
recipient must right-click → **Open** once, or run
`xattr -dr com.apple.quarantine Finch-Viewer.app`. Notarization is out of scope.

## Code Quality Rules

This toolbox must stay readable as it becomes faster. Do not let performance
work turn the codebase into a tangled pile.

- Keep module boundaries clear: data, I/O, compute, render, interaction, and
  diagnostics stay separate unless there is a strong reason to cross them.
- Keep UI code out of compute kernels and keep rendering types out of core data
  and algorithm modules whenever possible.
- Prefer small named functions and explicit data flow over hidden global state.
- Add abstractions only when they make a real boundary clearer, such as
  selection backends or render adapters.
- Optimize from the data layout outward: contiguous buffers, measurable
  algorithms, then CPU/OpenMP/GPU backends.
- Every non-trivial performance change needs a simple verification path:
  compile, smoke test, and when possible a benchmark or count equivalence check.
- Keep compatibility with the Python reference behavior until we intentionally
  decide to change semantics.
- If a function starts mixing loading, rendering, editing, statistics, and
  user interaction, split it before adding more behavior.
