"""On-screen file menu: rounded, labeled buttons (load tracts / load FA / save).

VTK has no Qt toolbar, so each entry is a vtkButtonWidget whose two texture
states (idle / pressed) are small images we render with matplotlib -- real
labeled buttons instead of bare checkbox squares.  Pure UI wiring; the actions
are Editor methods (CLAUDE.md: interaction mutates state through explicit
commands).
"""
from __future__ import annotations

import numpy as np
import vtk
from vtk.util.numpy_support import numpy_to_vtk

# (label, Editor method name), top to bottom
_ITEMS = [("  Load tracts", "load_dialog"),
          ("  Load image (FA)", "load_fa"),
          ("  Save", "save")]

_BTN_W, _BTN_H = 210, 40
_IDLE = ((0.16, 0.18, 0.23), (0.86, 0.90, 0.96), (0.30, 0.40, 0.48))   # fill, text, border
_HOT = ((0.36, 0.82, 0.80), (0.04, 0.06, 0.09), (0.45, 0.90, 0.88))


def _button_image(label, fill, fg, border):
    """Render a rounded labeled button to an RGBA array (transparent corners)."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.patches import FancyBboxPatch
    dpi = 100
    fig = plt.figure(figsize=(_BTN_W / dpi, _BTN_H / dpi), dpi=dpi)
    fig.patch.set_alpha(0.0)
    ax = fig.add_axes([0, 0, 1, 1]); ax.set_axis_off()
    ax.set_xlim(0, _BTN_W); ax.set_ylim(0, _BTN_H)
    ax.add_patch(FancyBboxPatch((4, 4), _BTN_W - 8, _BTN_H - 8,
                 boxstyle="round,pad=0,rounding_size=9", linewidth=1.4,
                 edgecolor=border, facecolor=fill))
    ax.text(18, _BTN_H / 2, label, color=fg, fontsize=12.5, va="center",
            ha="left", family="DejaVu Sans", weight="medium")
    fig.canvas.draw()
    rgba = np.asarray(fig.canvas.buffer_rgba()).copy()
    plt.close(fig)
    return rgba


def _to_vtk_image(rgba):
    h, w, _ = rgba.shape
    img = vtk.vtkImageData(); img.SetDimensions(w, h, 1)
    flat = np.ascontiguousarray(rgba[::-1].reshape(-1, 4))   # VTK origin is bottom-left
    arr = numpy_to_vtk(flat, deep=True, array_type=vtk.VTK_UNSIGNED_CHAR)
    arr.SetNumberOfComponents(4)
    img.GetPointData().SetScalars(arr)
    return img


def _press_handler(action, rep):
    def cb(widget, event):
        rep.SetState(1)                 # show pressed
        try:
            action()
        finally:
            rep.SetState(0)             # momentary: snap back to idle
    return cb


def setup_menu(editor, x=18, top=806, gap=10):
    """Build the labeled button column near the top-left, under the title."""
    iren = editor.plotter.iren.interactor
    editor._menu_widgets = []           # keep references alive
    for i, (label, method) in enumerate(_ITEMS):
        rep = vtk.vtkTexturedButtonRepresentation2D()
        rep.SetNumberOfStates(2)
        rep.SetButtonTexture(0, _to_vtk_image(_button_image(label, *_IDLE)))
        rep.SetButtonTexture(1, _to_vtk_image(_button_image(label, *_HOT)))
        y1 = top - i * (_BTN_H + gap)
        rep.PlaceWidget([x, x + _BTN_W, y1 - _BTN_H, y1, 0, 0])
        w = vtk.vtkButtonWidget()
        w.SetInteractor(iren)
        w.SetRepresentation(rep)
        w.On()
        w.AddObserver("StateChangedEvent", _press_handler(getattr(editor, method), rep))
        editor._menu_widgets.append(w)
