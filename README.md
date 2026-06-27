# Finch-Viewer

**Diffusion & functional MRI in one GPU scene — tractography, ODFs, peaks, and activation maps. Just Open the file.**

<p align="center">
  <img src="docs/images/odf-glyphs.png" alt="Direction-colored, lit fODF glyphs rendered on the GPU" width="480">
</p>

<p align="center">
  <a href="docs/images/ui.png"><img src="docs/images/ui.png" alt="The full app" height="150"></a>
  &emsp;
  <a href="docs/images/fmri.png"><img src="docs/images/fmri.png" alt="Signed fMRI activation overlay with diverging colorbar + orientation labels" height="150"></a>
  &emsp;
  <a href="docs/images/multimodal.png"><img src="docs/images/multimodal.png" alt="Anatomy, tracts, ODFs and peaks in one scene" height="150"></a>
  &emsp;
  <a href="docs/images/peaks.png"><img src="docs/images/peaks.png" alt="DEC peak segments" height="150"></a>
</p>
<p align="center"><sub><b>The app</b> &nbsp;·&nbsp; <b>fMRI overlay</b> &nbsp;·&nbsp; <b>One scene</b> (3-D + tri-planar) &nbsp;·&nbsp; <b>Peaks</b> &nbsp;— click to enlarge</sub></p>

---

## Highlights

| | |
|---|---|
| **GPU-rendered, real-time** | Glyphs, slices, overlays, and every colormap run on the GPU via Qt RHI → Metal — **100+ fps** on Apple Silicon. |
| **fODF glyphs** | Spherical-harmonic ODFs — direction-colored, lit, built and drawn on the GPU. |
| **fMRI activation maps** | Signed z/t/r overlays: diverging hot/cool, hard-thresholded over anatomy, with a truthful colorbar. |
| **Just Open** | Drop in any `.trk` or NIfTI; it reads the header + content and figures out the rest. |
| **One scene** | Streamlines + scalar volumes + ODFs + peaks + activation, in 3-D and tri-planar. |
| **Exact editing** | Box-select keep/delete on huge tractograms — the view is sampled, the **save is exact**. |

## Just Open

One unified **Open** auto-detects the file from its header *and content*: `.trk` vs NIfTI, and for NIfTI it tells a scalar volume from an SH-ODF from a peaks field from a **signed statistical map** apart by itself. No loaders, no modes — just open the file (or drag it in).

## Everything in one scene

3-D plus tri-planar (axial / coronal / sagittal): anatomy, streamlines, glyphs, peaks, and activation rendered together, with anatomical **L/R · A/P · S/I** labels on every pane. Glyphs follow the slice you scrub.

## fMRI activation maps

Open a signed statistical map (z / t / r) and it renders as a **diverging, hard-thresholded overlay** over grayscale anatomy — positive red→yellow, negative blue→cyan — auto-detected from the file. It comes with a **data-aware threshold**, a **truthful colorbar** (units read from the NIfTI intent code), and a hard transparency edge so the anatomy reads through. Drag the threshold/cap live.

## Colormaps

Grayscale anatomy · perceptually-uniform **viridis** for continuous metrics (ReHo/ALFF/…) · **diverging** hot/cool for signed stats · **DEC** for fiber orientation · categorical for labels. *(Jet/rainbow deliberately omitted — perceptually misleading.)*

## Peaks

Per-voxel fiber directions as DEC line segments — red = L-R, green = A-P, blue = S-I.

## Edit, exactly

Orbit the camera, scrub slices, and draw a 3-D selection box to keep or delete streamlines. The on-screen view is a fast subsample; edits act on the full set — so **saving is exact**.

## The app

Dense-dark UI: a Layers panel (Volume / Tracts / Label / ODF / Peaks / **Stat**), live contrast (intensity histogram + numeric window), a per-volume **colormap selector**, per-pane reset view, orientation labels, and an optional FPS/GPU overlay.

---

## Performance & engineering

The headline is the GPU path — and the algorithmic work behind keeping it interactive:

- **GPU all the way.** The RHI/Metal renderer draws streamlines, tri-planar slices, lit ODF glyphs, peak fields, and *every* colormap (diverging stats, viridis, DEC) on the GPU — interactive at **100+ fps**.
- **Fast where it counts.** SoA point storage with lazy FA sampling; the **whole-brain SH glyph scene builds in ~15 ms** — multithreaded, with a contiguous-coefficient gather and a cached SH basis (~2× a naive build); cache-friendly per-slice slab masking; a bounded on-screen sample with exact full-set editing.
- **Measured, not guessed.** Optimizations are kept only with a number behind them, per the project's *measure-before-optimizing* rule — and the render/rotation path is GPU-bound with no per-frame CPU work.

## Built on

- **C++ with Qt and the Qt RHI renderer.** The viewport selects its backend per platform — **Metal** on macOS, Direct3D 11 on Windows, OpenGL elsewhere; **macOS/Metal is the built-and-tested path** (portable to Vulkan / D3D12). The active editor uses **no VTK**.
- **Why RHI/Metal:** Apple froze system OpenGL at 4.1 — no compute shaders — so the renderer was migrated to Qt RHI → Metal, the modern GPU path. That migration is the project's headline move.
- **Clean boundaries:** a UI-free core library (io / data / compute), a standalone ODF library, and the Qt app on top.
- A **Python reference** implementation (VTK) is the behavior source of truth; the C++ app is the GPU-accelerated primary editor.

**Formats:** `.trk` tractograms and NIfTI (`.nii` / `.nii.gz`).

## Build & run

On macOS (Homebrew Qt):

```sh
tools/build_mac.sh                                  # configure + build -> build/Finch-Viewer.app
tools/run_qt_editor.sh --open anat.nii.gz zmap.nii.gz   # launch (Open auto-detects each file)
```

More detail in [DEV_WORKFLOW.md](DEV_WORKFLOW.md); the Python reference lives in [python/](python/).
