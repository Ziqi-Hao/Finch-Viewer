"""Interaction wiring: box widget + key bindings + file dialog.

Translates VTK events into explicit Editor method calls -- no editing logic
lives here (see CLAUDE.md: interaction mutates state through explicit commands).
"""
from __future__ import annotations

import vtk


def ask_open_file(title="Open file", filetypes=(("All files", "*.*"),)):
    """Native file dialog (tkinter); falls back to a terminal prompt."""
    try:
        import tkinter as tk
        from tkinter import filedialog
        root = tk.Tk(); root.withdraw(); root.attributes("-topmost", True)
        path = filedialog.askopenfilename(title=title, filetypes=list(filetypes))
        root.destroy()
        return path or None
    except Exception as e:
        print("file dialog unavailable:", e)
        try:
            return input(f"{title} -> path: ").strip() or None
        except EOFError:
            return None


def setup_interaction(editor):
    """Create the selection box widget and bind keys to editor commands."""
    plotter = editor.plotter
    init = editor.default_box_bounds()
    editor.box_bounds = init
    # rotation_enabled=False keeps the box axis-aligned (selection uses axis bounds);
    # InteractionEvent fires continuously so highlight tracks the drag.
    bw = plotter.add_box_widget(callback=editor.on_box, color="yellow",
                                rotation_enabled=False, factor=1.0, bounds=init,
                                interaction_event=vtk.vtkCommand.InteractionEvent)
    bw.AddObserver(vtk.vtkCommand.EndInteractionEvent,
                   lambda *a: editor.refresh_highlight())   # final, un-throttled frame
    editor.box_widget = bw

    bindings = {
        "d": editor.delete_in_box, "k": editor.keep_only_in_box,
        "u": editor.undo, "r": editor.reset, "s": editor.save,
        "h": editor.toggle_fa, "t": editor.print_stats, "p": editor.preview_box,
        "l": editor.load_dialog, "n": editor.density_set, "q": editor.quit,
    }
    for key, fn in bindings.items():
        plotter.add_key_event(key, fn)
    for key in ("plus", "equal", "KP_Add"):       # '+' / '=' / numpad +
        plotter.add_key_event(key, editor.density_up)
    for key in ("minus", "KP_Subtract"):          # '-' / numpad -
        plotter.add_key_event(key, editor.density_down)
