"""On-screen menu: clickable buttons to load tracts / load an FA image / save.

PyVista has no native toolbar without Qt, so we use checkbox-button widgets as
momentary buttons (act on press, then snap back) with a text label beside each.
Pure UI wiring -- the actions are Editor methods (see CLAUDE.md: interaction
mutates state through explicit commands).
"""
from __future__ import annotations

from .render import FONT_FILE

# (label, editor-method-name) for each button, top to bottom
_ITEMS = [
    ("Load tracts (.trk)", "load_dialog"),
    ("Load image (FA)", "load_fa"),
    ("Save", "save"),
]


def setup_menu(editor, top=820, size=24, gap=40):
    """Add the button column near the top-left of the window."""
    plotter = editor.plotter
    y = top
    for label, method in _ITEMS:
        _add_button(plotter, (14, y), size, getattr(editor, method))
        plotter.add_text(label, position=(14 + size + 10, y + 3),
                         font_size=11, color="white", font_file=FONT_FILE)
        y -= gap


def _add_button(plotter, position, size, action):
    holder = {}

    def on_click(state):
        if not state:               # ignore the programmatic snap-back to "off"
            return
        try:
            action()
        finally:
            w = holder.get("w")
            if w is not None:
                try:
                    w.GetRepresentation().SetState(False)
                except Exception:
                    pass

    holder["w"] = plotter.add_checkbox_button_widget(
        on_click, value=False, position=position, size=size, border_size=2,
        color_on="white", color_off="dimgray", background_color="black")
