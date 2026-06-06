# Finch-Viewer

GPU-accelerated interactive editor for tractography streamlines. Load a
tractogram (`.trk`) over an FA background, carve away unwanted streamlines with
a 3-D box, and save the surviving set exactly.

Two implementations live here and are kept intentionally separate:

- **`python/`** — the Python reference editor (fast iteration; the behavior
  source of truth). Entry: `python/local_editor.py`, logic in the
  `python/trkedit/` package.
- **`cpp/` + `CMakeLists.txt` + `tools/`** (repo root) — the C++ port, catching
  up to the Python reference.

## Features

- Edit on the **full** streamline set (`alive` mask); the on-screen view is only
  a sampled subset, so saving is exact.
- 3-D box **delete / keep**, with the in-box streamlines **highlighted live**.
- Affine-correct FA orthogonal-slice backdrop.
- Adjustable display density, point decimation, runtime `.trk` / FA loading.
- Box selection backends (vectorized SoA scan or a uniform-grid index).
- Statistics (count, length, FA-along-tract) and a live GPU/CPU/FPS overlay.

## Quick start (Python)

```bash
# deps: pyvista vtk dipy nibabel scipy psutil  (needs an on-screen VTK build)
python python/local_editor.py \
    --fa  FA.nii.gz \
    --trk tracts.trk \
    --out edited.trk
```

`--trk` is optional (a file dialog opens if omitted; you can also load tracts /
an FA image from the on-screen buttons). Controls: drag the yellow box
(white = inside box → will be edited), then `d` delete · `k` keep · `p` preview ·
`t` stats · `+/-` density · `n` set count · `l` load · `u` undo · `r` reset ·
`s` save · `h` toggle FA · `q` quit. Rotate the view by dragging the background.

## Build & run (C++)

See [DEV_WORKFLOW.md](DEV_WORKFLOW.md) (uses the PowerShell scripts in `tools/`
with CMake + vcpkg + VTK).

## Notes

- Tractography data (`*.trk`) and volumes (`*.nii.gz`) are **not** tracked here
  (too large for GitHub); keep them locally and pass paths via the CLI.
- Project conventions and engineering rules: [CLAUDE.md](CLAUDE.md),
  [DEV_WORKFLOW.md](DEV_WORKFLOW.md), [TOOLBOX_PLAN.md](TOOLBOX_PLAN.md).
