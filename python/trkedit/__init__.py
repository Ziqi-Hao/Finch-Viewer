"""Tractography editor (Python reference implementation).

Module boundaries (see ../../CLAUDE.md):
    tractogram  - streamline storage as a Structure-of-Arrays   (no VTK)
    volume      - scalar background volume + point sampling       (no VTK)
    selection   - box-selection backends (linear / grid)         (no VTK)
    render      - PyVista actors: volume slices, line layer, HUD text (VTK)
    diagnostics - live GPU/CPU/FPS overlay                        (VTK)
    interaction - box widget + key bindings + file dialog         (VTK)
    editor      - holds state and wires the pieces together
"""
