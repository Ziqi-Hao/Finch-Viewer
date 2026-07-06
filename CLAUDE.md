# Tractography Editor — Project Rules

Two implementations live here, kept **separate on purpose** (Python is the fast
iteration surface; C++ is catching up to match it):

- `python/` — the Python reference editor and **behavior source of truth**.
  Entry: `python/local_editor.py`; logic in the `python/trkedit/` package
  (`tractogram`, `volume`, `selection`, `render`, `diagnostics`, `interaction`,
  `editor`, `ui`). Run from the `trkedit` conda env (Ubuntu-22.04 WSL):
  `python python/local_editor.py --fa FA.nii.gz --trk in.trk --out out.trk`
  (`scilpy_env`'s VTK is a headless EGL-only build and cannot open a window).
- `cpp/` + `CMakeLists.txt` + `tools/` — the C++ toolbox port. All C++ lives
  under `cpp/`: `cpp/core/` is the shared, UI-free io/data/compute library
  (`tracto_core`) and `cpp/odf/` is the pure-CPU ODF/glyph library (`tracto_odf`);
  the sole app is `cpp/qt/`, the Qt + RHI (Metal) editor (`local_editor_qt`),
  holding its `main.cpp` next to its own code. Build/run via the `tools/` scripts
  (`tools/build_mac.sh` on macOS; see [DEV_WORKFLOW.md](DEV_WORKFLOW.md)).

Shared `.trk`/`.nii.gz` data lives in `data/` (gitignored) so both
implementations can reference it via the CLI. Keep the two trees from leaking
into each other, and keep `cpp/core/` free of VTK and Qt.

The detailed engineering rules already live in **[DEV_WORKFLOW.md](DEV_WORKFLOW.md)**
("Code Quality Rules") and **[TOOLBOX_PLAN.md](TOOLBOX_PLAN.md)** ("Engineering
Rules"). Read them. This file states the hard requirements that override
convenience.

## Code must stay elegant and readable — this is not negotiable

Performance work must never turn the code into a tangled pile ("屎山"). A change
that makes the code faster but harder to read is **not done** until it is cleaned
up. Readability is part of the deliverable, not a follow-up.

- **Measure before optimizing.** No optimization without a number that shows it
  helps. Keep the check (benchmark / count-equivalence / smoke test) and cite it
  in the comment or commit. Do not optimize what is not hot.
- **Separate concerns.** Data / I-O / compute (selection, stats) / render /
  interaction / diagnostics stay in distinct units. Keep VTK types out of core
  data and algorithm code. If one function starts mixing loading, rendering,
  editing, stats, and interaction, split it *before* adding more behavior.
- **Explicit data flow over hidden global state.** Prefer small named functions
  that take inputs and return outputs over reaching into module globals.
- **Comment the *why*, not the *what*** — one line on the reason and the measured
  payoff for any non-obvious performance trick (SoA layout, NaN-hiding,
  decimation, lazy FA, etc.).
- **Add abstractions only at real boundaries** (e.g. a selection backend); never
  speculatively.
- **Keep Python and C++ behavior aligned** until we deliberately change semantics.

## Optimization map (where effort is — and isn't — worth it)

- **Storage**: already SoA + contiguous + lazy FA sampling. Remaining work is
  *removing redundant copies* (clarity + memory), not a new layout.
- **Interaction (selection / highlight)**: the one real algorithmic win left is a
  spatial index (uniform voxel grid) so box queries are O(box) instead of
  O(all points). Do it only as a clean, separate **selection backend**.
- **View rotation**: already GPU-bound (~8 ms/frame, no per-frame CPU work). Do
  **not** add CPU work to the render/rotation path.

## Verify every change

Editing acts on the full set (`alive_full`); display is only a sampled view.
Saving must stay exact. After any non-trivial change: run a smoke test (the
editor launches and blocks on `show()`), and for compute changes a
count-equivalence or benchmark check, before declaring it done.
