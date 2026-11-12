#include "SearchUI.hpp"
#include "WindowSearch.hpp"

#include <pango/pangocairo.h>
#include <xkbcommon/xkbcommon.h>
#include <linux/input-event-codes.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#define CHECK(condition) do { if (!(condition)) \
    throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #condition); } while (false)

struct IKeyboard {
    struct SKeyEvent { uint32_t keycode; };
    xkb_state* m_xkbState = nullptr;
};
IKeyboard device;
namespace OverviewSearchInput {
IKeyboard* keyboard() { return &device; }
}
class COverviewSearch {
  public:
    WindowSearch::Editor m_editor;
    std::vector<size_t> m_matches{0, 1, 2};
    size_t m_selected = 0, m_rows = 8;
    std::string m_message;
    int paints = 0, confirms = 0, cancels = 0;
    void refresh() {}
    void repaint() { ++paints; }
    void confirm() { ++confirms; }
    void cancel() { ++cancels; }
    void key(const IKeyboard::SKeyEvent& event);
    void moveSelection(int delta);
};

// The script extracts these two production methods without changing their body.
#include "search-input-methods.inc"

void navigation() {
    auto* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    CHECK(context);
    const xkb_rule_names names{.rules = "evdev", .model = "pc105", .layout = "us",
                               .variant = nullptr, .options = nullptr};
    auto* keymap = xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    CHECK(keymap);
    device.m_xkbState = xkb_state_new(keymap);
    CHECK(device.m_xkbState);
    auto held = [](uint32_t key, bool down) {
        xkb_state_update_key(device.m_xkbState, key + 8, down ? XKB_KEY_DOWN : XKB_KEY_UP);
    };
    COverviewSearch search;
    search.m_editor.set("query");
    const auto cursor = search.m_editor.cursor();
    held(KEY_LEFTCTRL, true);
    for (const auto key : {KEY_J, KEY_N}) {
        search.m_selected = 2;
        search.key({static_cast<uint32_t>(key)});
        CHECK(search.m_selected == 0); // Wrap forward.
        CHECK(search.m_editor.text() == "query" && search.m_editor.cursor() == cursor);
    }
    for (const auto key : {KEY_K, KEY_P}) {
        search.m_selected = 0;
        search.key({static_cast<uint32_t>(key)});
        CHECK(search.m_selected == 2); // Wrap backward.
    }
    held(KEY_LEFTSHIFT, true);
    search.key({KEY_N});
    CHECK(search.m_selected == 0); // Shift/Caps uppercase keysyms work too.
    held(KEY_LEFTSHIFT, false);
    for (const auto modifier : {KEY_LEFTALT, KEY_LEFTMETA}) {
        held(modifier, true);
        search.key({KEY_J});
        CHECK(search.m_selected == 0 && search.m_editor.text() == "query");
        held(modifier, false);
    }
    search.m_matches.clear();
    search.key({KEY_K});
    CHECK(search.m_selected == 0 && search.m_editor.text() == "query");
    search.m_matches = {0};
    search.key({KEY_J});
    CHECK(search.m_selected == 0);
    search.key({KEY_U});
    CHECK(search.m_editor.text().empty());
    held(KEY_LEFTCTRL, false);
    for (const auto key : {KEY_J, KEY_K, KEY_N, KEY_P})
        search.key({static_cast<uint32_t>(key)});
    CHECK(search.m_editor.text() == "jknp");
    search.m_matches = {0, 1, 2};
    search.key({KEY_DOWN});
    CHECK(search.m_selected == 1);
    search.key({KEY_UP});
    CHECK(search.m_selected == 0);
    search.key({KEY_TAB});
    CHECK(search.m_selected == 1);
    search.key({KEY_ENTER});
    search.key({KEY_ESC});
    CHECK(search.confirms == 1 && search.cancels == 1);
    xkb_state_unref(device.m_xkbState);
    device.m_xkbState = nullptr;
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
}

void appearance() {
    CHECK(std::string(SearchUI::FONT) == "monospace");
    CHECK(SearchUI::BACKGROUND == 0x1e1e2edd && SearchUI::TEXT == 0xcdd6f4ff);
    CHECK(SearchUI::PROMPT == 0xbac2deff && SearchUI::MUTED == 0x7f849cff);
    CHECK(SearchUI::SELECTION == 0x585b70ff && SearchUI::ACCENT == 0x89b4faff);
    for (const double scale : {1.0, 1.25, 2.0}) {
        const int width = static_cast<int>(200 * scale), height = static_cast<int>(100 * scale);
        auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
        auto* cr = cairo_create(surface);
        cairo_scale(cr, scale, scale);
        SearchUI::panel(cr, 200, 100);
        CHECK(cairo_status(cr) == CAIRO_STATUS_SUCCESS);
        cairo_surface_flush(surface);
        const auto* data = cairo_image_surface_get_data(surface);
        const int stride = cairo_image_surface_get_stride(surface);
        auto pixel = [&](int x, int y) {
            return reinterpret_cast<const uint32_t*>(data + y * stride)[x];
        };
        CHECK(pixel(0, 0) == 0 && pixel(width - 1, 0) == 0);
        CHECK(pixel(0, height - 1) == 0 && pixel(width - 1, height - 1) == 0);
        const uint32_t center = pixel(width / 2, height / 2);
        CHECK((center >> 24) == 0xdd); // Preserve fuzzel's background transparency.
        CHECK(((center >> 16) & 255) == 26 && ((center >> 8) & 255) == 26);
        CHECK((center & 255) == 40); // Cairo premultiplies 30, 30, 46 by 221/255.
        CHECK((pixel(width / 2, 0) >> 24) > 0xdd); // Blue border, not background.
        SearchUI::color(cr, SearchUI::TEXT);
        double r, g, b, a;
        CHECK(cairo_pattern_get_rgba(cairo_get_source(cr), &r, &g, &b, &a) == CAIRO_STATUS_SUCCESS);
        CHECK(r == 205 / 255.0 && g == 214 / 255.0 && b == 244 / 255.0 && a == 1);
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
    }
}

int main() {
    try {
        navigation();
        std::cout << "PASS production search key handling and selection wrapping\n";
        appearance();
        std::cout << "PASS fuzzel palette and rounded Cairo panel at 1x/1.25x/2x\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
