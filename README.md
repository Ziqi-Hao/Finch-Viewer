# Finch-Viewer

A GPU-accelerated, interactive editor for **tractography streamlines**. Load a
tractogram (`.trk`) over an FA background, carve away unwanted streamlines with a
3-D selection box (the in-box tracts highlight live), and save the surviving set
**exactly**.

The repo holds two implementations, kept intentionally separate:

- **Python — [`python/`](python/) — the reference editor and behavior source of
  truth.** This part is complete and fully documented; start at
  **[python/README.md](python/README.md)**.
- **C++ — `cpp/` + `CMakeLists.txt` + `tools/` — a port that is still catching
  up** to the Python reference (work in progress). Build/run notes:
  [DEV_WORKFLOW.md](DEV_WORKFLOW.md).

Shared `.trk` / `.nii.gz` data lives in `data/` (not tracked — too large for
GitHub) and is passed to either editor via the CLI.

---

## Python quick start

```bash
conda create -n trkedit -y python=3.10 && conda activate trkedit
pip install -r python/requirements.txt

python python/local_editor.py --fa FA.nii.gz --trk tracts.trk --out edited.trk
```

Needs an **on-screen** VTK build (the PyPI `vtk` wheel, not a headless one). Drag
the gold box (white = selected) then `d` delete / `k` keep; on-screen buttons
load tracts / an FA image / save. **Full docs, controls, architecture, and the
15-step development history: [python/README.md](python/README.md).**

---

## Features

- Edit on the **full** streamline set (an `alive` mask); the view is only a
  sampled subset, so **saving is exact**.
- 3-D box **delete / keep** with the in-box streamlines **highlighted live**.
- Affine-correct FA orthogonal-slice backdrop.
- Adjustable display density, runtime `.trk` / FA loading, statistics, and a live
  GPU/CPU/FPS overlay.
- Pluggable box-selection backends (vectorized scan or a uniform-grid index).

---

## Project docs

- [python/README.md](python/README.md) — the comprehensive Python guide.
- [CLAUDE.md](CLAUDE.md) — engineering rules (elegant/readable, measure before
  optimizing, module boundaries).
- [DEV_WORKFLOW.md](DEV_WORKFLOW.md), [TOOLBOX_PLAN.md](TOOLBOX_PLAN.md) — C++
  build workflow and toolbox plan.
