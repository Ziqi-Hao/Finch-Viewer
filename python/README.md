# Finch-Viewer — Python editor

The **Python reference implementation** of Finch-Viewer: a GPU-accelerated,
interactive editor for tractography streamlines. This is the fast-iteration
surface and the **behavior source of truth** for the project (the C++ port under
`../cpp/` is catching up to it).

Everything Python lives in this directory:

```text
python/
  local_editor.py      # thin CLI entry point
  trkedit/             # the package
    tractogram.py      # streamlines as a Structure-of-Arrays; exact save   (no VTK)
    fa.py              # FA volume + world-point sampling                    (no VTK)
    selection.py       # box-selection backends: linear / grid              (no VTK)
    render.py          # FA slices, line layer, highlight, HUD, theme        (VTK)
    diagnostics.py     # live GPU/CPU/FPS overlay                            (VTK)
    interaction.py     # box widget, key bindings, file dialog               (VTK)
    ui.py              # on-screen menu buttons                              (VTK)
    editor.py          # holds state, wires everything together
  requirements.txt
  README.md            # this file
```

The numpy **core** (`tractogram`, `fa`, `selection`) is deliberately kept free of
VTK so it can be reused and ported (see [../CLAUDE.md](../CLAUDE.md)).

---

## Requirements

```bash
pip install -r python/requirements.txt
```

You need an **on-screen** VTK build — the PyPI `vtk` wheel, *not* a headless
EGL/OSMesa build (a headless VTK opens no interactive window and the app exits
immediately). A clean conda env works well:

```bash
conda create -n trkedit -y python=3.10
conda activate trkedit
pip install -r python/requirements.txt
```

> On WSL: use a recent WSLg + the discrete GPU. The renderer is shown in the
> top-right overlay and printed once at startup (`[GPU CHECK] rendering on: …`).

---

## Quick start

Run from the repository root (so the data paths resolve):

```bash
python python/local_editor.py \
    --fa  FA.nii.gz \
    --trk tracts.trk \
    --out edited.trk
```

`--trk` is optional — omit it and a file dialog opens at startup.

| Flag | Default | Meaning |
| --- | --- | --- |
| `--fa` | (required) | background FA NIfTI |
| `--trk` | (dialog) | input `.trk`; a file dialog opens if omitted |
| `--out` | (required) | output `.trk` for the surviving streamlines |
| `--display-n` | 12000 | max streamlines drawn at once |
| `--disp-step` | 2 | draw every k-th point (visual only; editing/saving use full res) |
| `--selector` | grid | box-selection backend (`grid` or `linear`) |
| `--seed` | 0 | display-subsample seed |

### Controls

Drag the **gold box** (its contents turn **white** = will be edited). Rotate the
**view** by dragging the empty background. On-screen buttons (top-left):
**Load tracts**, **Load image (FA)**, **Save**.

```text
d delete   k keep   p preview   t stats   +/- density   n set#   l load
u undo     r reset  s save      h toggle FA            q quit
```

---

## How it works

- **Full-set editing.** Edits act on the entire streamline set (an `alive`
  mask); the on-screen view is only a sampled subset, so **saving is exact**.
- **Structure-of-Arrays.** Every point lives in three contiguous per-axis arrays
  (`X/Y/Z`) plus a per-point streamline id, so box queries and FA sampling are
  single vectorized passes; per-streamline `(m,3)` arrays are reconstructed on
  demand.
- **Selection backends.** `linear` scans every point; `grid` (default) is a
  uniform voxel index that visits only the buckets overlapping the box —
  localized queries are ~5–13 ms (vs ~90 ms full scan).
- **Cheap edits.** A delete/keep blanks dead points to `NaN` (the GPU discards
  NaN segments) instead of re-packing geometry — ~103 ms vs ~205 ms, and the
  view doesn't reshuffle.
- **Live highlight.** The in-box streamlines are drawn as a reused white overlay
  actor (~11–13 ms/frame for typical boxes), so dragging stays smooth.
- **Display decimation** (`--disp-step`) + a per-streamline geometry cache keep
  rebuilds cheap; **FXAA** smooths the lines for ~free.

(Numbers for the reference dataset: 64,208 streamlines, ~24.5 M points.)

---

## Development journey

Built in **15 milestones**, each starting from a concrete problem:

1. **Tracts were invisible** — the FA backdrop ignored the affine's negative-x
   flip and landed ~120 mm off. Fixed by slicing in voxel space, then
   transforming each slice through the real affine.
2. **The window flashed and closed** — the env's VTK was a headless EGL build
   that can't open an interactive window. Solved with an on-screen VTK env;
   confirmed rendering on the discrete GPU (D3D12 / RTX 2060) via WSLg.
3. **Rotating the box mis-selected** — selection uses axis-aligned bounds, so a
   rotated box grabbed its larger upright box. Box rotation disabled.
4. **First `d` deleted everything** — the box defaulted to 1.25× the dataset; it
   now starts small and centered.
5. **Statistics** — `t` (counts, length, FA-along-tract) and `p` (box preview).
6. **Live GPU/CPU monitoring** — confirmed GPU use; added the renderer / FPS /
   CPU% / GPU% overlay.
7. **User-controlled density + loading** — `+`/`-`/`n` density; runtime `.trk`
   loading; `--trk` made optional.
8. **Algorithmic speedups** — SoA layout, decimated display, cached geometry,
   vectorized stats; editing moved onto the full set so saving became exact.
9. **Cheaper per-edit updates** — VTK ghost-cell hiding wasn't honored by this
   mapper, so edits use NaN-hiding (~2× faster, lines vanish in place).
10. **Real-time selection highlight** — white overlay; optimized to reuse one
    overlay actor for smooth dragging.
11. **Engineering rules** — added [../CLAUDE.md](../CLAUDE.md): stay
    elegant/readable, measure before optimizing, keep module boundaries.
12. **Separated Python & C++ + package refactor** — the monolith became this
    `trkedit/` package (VTK out of the numpy core); added the grid backend and
    removed duplicated buffers.
13. **Smooth dragging, visible menu, nicer font** — overlay reuse; on-screen
    Load/Save buttons; DejaVuSans/Mono HUD.
14. **UI redesign** — dark slate theme, teal accent, gold box, rounded labeled
    buttons, monospace readouts, FXAA, tract-framed camera.
15. **Published** — pushed to GitHub with data and build artifacts excluded.

---

## Notes

- Tractography data (`*.trk`) and volumes (`*.nii.gz`) are **not** tracked here;
  keep them locally and pass paths on the CLI.
- Project rules: [../CLAUDE.md](../CLAUDE.md). C++ build/run: [../DEV_WORKFLOW.md](../DEV_WORKFLOW.md).
