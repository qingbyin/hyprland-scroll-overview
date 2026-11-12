#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cairo/cairo.h>
#include <xkbcommon/xkbcommon-keysyms.h>

// Snapshot of the local fuzzel Catppuccin Mocha theme. Values are RRGGBBAA,
// not Cairo's native premultiplied ARGB pixel representation.
namespace SearchUI {
inline constexpr const char* FONT = "monospace";
inline constexpr uint32_t BACKGROUND = 0x1e1e2edd;
inline constexpr uint32_t TEXT = 0xcdd6f4ff;
inline constexpr uint32_t PROMPT = 0xbac2deff;
inline constexpr uint32_t MUTED = 0x7f849cff;
inline constexpr uint32_t ACCENT = 0x89b4faff;
inline constexpr uint32_t SELECTION = 0x585b70ff;
inline constexpr double RADIUS = 10.0;
inline constexpr double BORDER_WIDTH = 1.0;

inline void color(cairo_t* cr, uint32_t rgba) {
    cairo_set_source_rgba(cr, ((rgba >> 24) & 255) / 255.0,
                          ((rgba >> 16) & 255) / 255.0,
                          ((rgba >> 8) & 255) / 255.0, (rgba & 255) / 255.0);
}

inline void roundedRectangle(cairo_t* cr, double x, double y,
                             double width, double height, double radius) {
    const double r = std::clamp(radius, 0.0, std::min(width, height) / 2);
    const double pi = std::acos(-1.0);
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + width - r, y + r, r, -pi / 2, 0);
    cairo_arc(cr, x + width - r, y + height - r, r, 0, pi / 2);
    cairo_arc(cr, x + r, y + height - r, r, pi / 2, pi);
    cairo_arc(cr, x + r, y + r, r, pi, 3 * pi / 2);
    cairo_close_path(cr);
}

// The caller uses a fresh transparent Cairo image surface. The corners stay
// transparent; painting the full surface first would leave a square panel.
inline void panel(cairo_t* cr, double width, double height) {
    roundedRectangle(cr, 0, 0, width, height, RADIUS);
    color(cr, BACKGROUND);
    cairo_fill(cr);
    const double inset = BORDER_WIDTH / 2;
    roundedRectangle(cr, inset, inset, width - BORDER_WIDTH,
                     height - BORDER_WIDTH, RADIUS - inset);
    color(cr, ACCENT);
    cairo_set_line_width(cr, BORDER_WIDTH);
    cairo_stroke(cr);
}

inline int controlSelectionDelta(uint32_t sym, bool control, bool alt, bool logo) {
    if (!control || alt || logo)
        return 0;
    switch (sym) {
        case XKB_KEY_j:
        case XKB_KEY_J:
        case XKB_KEY_n:
        case XKB_KEY_N: return 1;
        case XKB_KEY_k:
        case XKB_KEY_K:
        case XKB_KEY_p:
        case XKB_KEY_P: return -1;
        default: return 0;
    }
}
}
