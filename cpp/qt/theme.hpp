#pragma once

// Centralized dark theme for the Qt editor. One place owns the color tokens so
// the QSS stylesheet and any GL-side colors (viewport clear, overlays) read the
// same values instead of drifting. Apply once at startup via ApplyTheme().
//
// Direction: "professional dense dark" — the reference class is pro 3D / neuro
// tools (Blender, 3D Slicer, DSI Studio): dark-first, low chrome, a single
// accent, layered surfaces separated by hairlines rather than heavy borders.

#include <QString>

class QApplication;

namespace tracto {
namespace theme {

// Design tokens (hex). Grouped by role; kept as constants so non-QSS code can
// read the same palette.
inline constexpr const char* kBg0 = "#0F1115";       // viewport / deepest surface
inline constexpr const char* kBg1 = "#15181E";       // bars, panels
inline constexpr const char* kBg2 = "#1C2027";       // cards, elevated controls
inline constexpr const char* kHairline = "#262B33";  // 1px dividers / borders
inline constexpr const char* kText = "#E6E9EF";      // primary text
inline constexpr const char* kTextDim = "#A6ADBB";   // secondary text
inline constexpr const char* kTextMuted = "#6B7280"; // disabled / hints
inline constexpr const char* kAccent = "#4C8DFF";    // single accent (neutral azure)
inline constexpr const char* kOnAccent = "#0B0E14";  // text/icon on an accent fill
inline constexpr const char* kDanger = "#FF5C5C";    // destructive (delete)

}  // namespace theme

// The full QSS stylesheet, built from the tokens above.
QString StyleSheet();

// Apply the Fusion base style, a dark palette (so dialogs follow), and the QSS.
void ApplyTheme(QApplication& app);

}  // namespace tracto
