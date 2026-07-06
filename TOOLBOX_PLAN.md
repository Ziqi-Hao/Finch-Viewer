# Tractography Visualization Toolbox Plan

## Core Principle

The editor should keep one stable interaction model while allowing the compute
backend to improve over time:

- Rendering should stay on GPU through Qt RHI vertex buffers (Metal on macOS).
- Selection/statistics should use contiguous SoA data first, then switchable
  CPU/OpenMP or GPU backends.
- Editing state should always be exact on the full streamline set; display
  sampling is only a visualization policy.
- File saving should preserve source `.trk` metadata and point data whenever
  possible.
- Code quality is part of performance: clean module boundaries make it possible
  to replace CPU kernels with GPU kernels without rewriting the whole app.

## Engineering Rules

- Core data and compute modules should not depend on the renderer (Qt/RHI).
- Render modules should not own editing semantics.
- Interaction modules should mutate state through explicit commands.
- GPU/CPU backend choices should sit behind small interfaces, not leak through
  the UI.
- Avoid large all-knowing classes; split as soon as a module has multiple
  responsibilities.
- Keep the C++ implementation readable enough to compare against the Python
  reference behavior feature by feature.

## Current Python Reference

`local_editor.py` already has the behavior we want the C++ toolbox to match:

- full-set `alive_full` editing
- SoA point cloud: `ALL_X`, `ALL_Y`, `ALL_Z`, `ALL_SID`, `OFFSETS`
- deterministic display cap and point decimation
- affine-correct FA slices
- box preview, statistics, runtime `.trk` loading
- CPU/GPU/FPS overlay

## C++ Modules

> Layout note: the tree is organized as `cpp/core/` (the `tracto_core` library:
> io/data/compute), `cpp/odf/` (the `tracto_odf` ODF/glyph library), and the
> single app `cpp/qt/` (the Qt + RHI editor). The per-module file paths below
> predate that move (e.g. `cpp/trk_io.*` is now `cpp/core/trk_io.*`, and the
> render/interaction code now lives in `cpp/qt/`); the module responsibilities
> still hold.

### app

Entry point, CLI, application startup.

Current files:

- `cpp/qt/main.cpp`
- `cpp/core/args.*`

### data

Data models shared by CPU and GPU backends.

Current files:

- `cpp/core/tractogram_store.*`
- `cpp/core/bounds.hpp`

Responsibilities:

- Own raw `.trk` streamlines for lossless save.
- Own SoA RASMM buffers: `x`, `y`, `z`, `sid`, `offsets`.
- Own derived arrays: streamline lengths (points-per-line is derived from
  `offsets`, not stored), later FA-per-point.
- Expose GPU-ready buffer views without UI dependencies.

### io

File loading and saving.

Current files:

- `cpp/core/trk_io.*`
- `cpp/core/nifti_io.*`

Next:

- Move `.trk` load directly into `TractogramStore`.
- Add NIfTI volume wrapper if VTK image loading is not enough for statistics.
- Add edit-session save/load, e.g. JSON operation log.

### compute

Pure algorithms independent of VTK UI.

Current files:

- `cpp/core/streamline_ops.*`
- `cpp/core/selection_backend.*`
- `cpp/core/statistics.*`
- `cpp/core/display_geometry.*`
- `cpp/core/track_density.*`

On-disk checks: `cpp/core/core_selftest.cpp`, `cpp/odf/odf_selftest.cpp`,
`cpp/core/selection_bench.cpp` (manual perf run).

Backend plan:

- `CpuSelectionBackend`: SoA scan, OpenMP, optional per-streamline AABB prefilter.
- `CudaSelectionBackend`: one thread per point, atomic OR into streamline flags.
- `VtkmSelectionBackend`: portable GPU/CPU option if we want less CUDA lock-in.

Algorithm order:

1. SoA full point scan: fastest to implement, memory-bandwidth limited, exact.
2. Per-streamline AABB prefilter: skip most streamlines for small boxes.
3. Spatial grid/BVH: useful if preview becomes live during box drag.
4. CUDA kernel: useful when point count is very large or preview is continuous.

### render

Scene and drawable geometry.

Current location:

- `cpp/qt/tract_viewport.*` (the QRhiWidget: RHI pipelines, buffers, camera)
- `cpp/qt/main_window.*` (display-geometry build wiring)
- RHI shaders in `cpp/qt/rhi/` (`line`, `point`, `slice`, `glyph`)

GPU priorities:

- Keep streamline/glyph geometry in RHI vertex buffers; one pipeline per layer.
- Avoid rebuilding all geometry for small visibility changes where possible.
- Rebuild sampled display geometry only when alive mask or display cap changes.
- Later: per-streamline visibility on the GPU for faster hide/show.

### interaction

Editing state and commands.

Current location:

- mostly `cpp/qt/main_window.*` (history/undo, keep/delete-in-box commands)

Target split:

- `interaction/edit_session.*`
- `interaction/commands.*`
- `interaction/keymap.*`

Rules:

- `aliveFull` is authoritative.
- Undo stores masks or compressed deltas.
- Display is always derived from `aliveFull`.

### diagnostics

Performance measurement and correctness checks.

Target split:

- `diagnostics/perf_monitor.*`
- `bench/rebuild_display_bench.*`
- `tests/trk_roundtrip.*`
- `tests/selection_equivalence.*`

Metrics to show:

- RHI backend (Metal on macOS)
- FPS
- CPU utilization
- GPU utilization and memory when available
- point count, shown streamline count, full alive count

## Milestones

1. Data foundation
   - C++ `TractogramStore` with SoA buffers.
   - Full-set `aliveFull` editing.
   - Build stays green.

2. Python feature parity
   - display cap resamples surviving streamlines
   - `--disp-step`
   - `p` preview
   - `t` statistics
   - `+/-/n` density controls
   - runtime load

3. Render performance
   - decimated display cache
   - avoid full VTK actor rebuilds when only visibility changes
   - benchmark display rebuild time

4. Compute backends
   - `ISelectionBackend`
   - CPU/OpenMP backend
   - CUDA prototype backend
   - backend selection CLI flag

5. GPU-first interactive preview
   - optional live box preview
   - GPU point scan or spatial index
   - latency and throughput logging

## Near-Term Next Step

Display policy parity is implemented:

- `--disp-step`
- rebuild display from surviving full-set streamlines
- deterministic resample after every edit
- status shows `alive`, `shown`, display cap, and display step
- `+/-/n` runtime density controls
- `p` preview via `SelectionBackend`
- `t` basic statistics for surviving full-set streamlines

Next feature-parity step:

- FA-per-point sampling and FA-on-tract statistics
