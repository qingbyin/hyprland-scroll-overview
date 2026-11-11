#include "OverviewSearch.hpp"
#include "OverviewOpen.hpp"
#include "OverviewRender.hpp"
#include "SearchInput.hpp"
#include "scrollOverview.hpp"

#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Group.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowGroupMembership.hpp>
#include <hyprland/src/desktop/view/window/WindowMetadata.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollingAlgorithm.hpp>
#include <hyprland/src/layout/supplementary/DragController.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>
#include <pango/pangocairo.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace {
COverviewSearch* owner = nullptr;
std::vector<CHyprSignalListener> listeners;
bool inputAvailable = false;
template <typename Device> struct SConsumedInput {
    WP<Device> device;
    uint32_t code;
};
std::vector<SConsumedInput<IKeyboard>> consumedKeys;
std::vector<SConsumedInput<IPointer>> consumedButtons;

template <typename Device>
bool captured(std::vector<SConsumedInput<Device>>& inputs, const SP<Device>& device, uint32_t code, bool release = false) {
    std::erase_if(inputs, [](const auto& input) { return input.device.expired(); });
    const auto found = std::ranges::find_if(inputs, [&](const auto& input) { return input.device.lock() == device && input.code == code; });
    if (found == inputs.end())
        return false;
    if (release)
        inputs.erase(found);
    return true;
}
std::unordered_set<int32_t> consumedTouches;

void changed() {
    if (owner)
        owner->changed();
}

// Window metadata is client-provided. Pango must never see invalid UTF-8.
std::string displayText(const std::string& input) {
    auto* valid = g_utf8_make_valid(input.data(), input.size());
    std::string result;
    for (auto* p = valid; *p; p = g_utf8_next_char(p)) {
        const auto c = g_utf8_get_char(p);
        if (g_unichar_iscntrl(c))
            result += ' ';
        else
            result.append(p, g_utf8_next_char(p) - p);
    }
    g_free(valid);
    return result;
}

void text(cairo_t* cr, const std::string& value, double x, double y,
          double width, int size, bool muted = false) {
    auto* layout = pango_cairo_create_layout(cr);
    auto* font = pango_font_description_from_string("Sans");
    pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
    pango_layout_set_font_description(layout, font);
    const auto clean = displayText(value);
    pango_layout_set_text(layout, clean.data(), clean.size());
    pango_layout_set_single_paragraph_mode(layout, true);
    pango_layout_set_width(layout, std::max(1, static_cast<int>(width)) * PANGO_SCALE);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    cairo_set_source_rgb(cr, muted ? 0.64 : 0.94, muted ? 0.70 : 0.96, muted ? 0.79 : 0.99);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
    pango_font_description_free(font);
    g_object_unref(layout);
}
}

namespace OverviewSearch {
bool active() {
    return owner != nullptr;
}

bool validArgument(std::string_view argument) {
    if (argument.empty() || argument == "open" || argument == "toggle" || argument == "close" || argument == "query")
        return true;
    return argument.starts_with("query ") && WindowSearch::validQuery(argument.substr(6));
}

bool eligible(const PHLWINDOW& window) {
    if (!Desktop::View::validMapped(window) || !window->m_workspace ||
        window->m_workspace->type() == Workspace::eWorkspaceType::SPECIAL ||
        (window->m_state & Desktop::View::WINDOW_STATE_PINNED))
        return false;
    if (window->m_ruleApplicator->noFocus().valueOrDefault())
        return false;
    const auto traits = window->backend().traits();
    if ((window->backend().isX11() && traits.overrideRedirect && !traits.wantsFocus) ||
        (!window->backend().isX11() && traits.hasModalChild &&
         ScrollOverview::Config::getValue<Hyprlang::INT>("general:modal_parent_blocking")))
        return false;
    const auto monitor = window->m_workspace->m_monitor.lock();
    if (!monitor || !monitor->m_enabled || window->m_monitor != monitor)
        return false;
    // Hidden group tabs are not represented by independent overview cards.
    if (const auto group = window->grouping().group(); group && group->current() != window)
        return false;
    // Non-scrolling fullscreen hides other tiled cards. Non-layout-managed
    // fullscreen also redirects their focus, even on a scrolling workspace.
    const auto space = window->m_workspace->space();
    const bool scrolling = space && space->algorithm() &&
        dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>(space->algorithm()->tiledAlgo().get());
    const auto fullscreen = Fullscreen::controller()->getFullscreenWindow(window->m_workspace);
    if (fullscreen && fullscreen != window && !window->isFloating() &&
        (!scrolling || !Fullscreen::controller()->layoutManagedFS(fullscreen)))
        return false;
    // Test the prospective overview transform, not native desktop geometry:
    // scaling around the monitor center can expose off-desktop floating cards.
    if (window->isFloating()) {
        auto box = window->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        box.translate(-monitor->m_position);
        const auto center = monitor->m_size / 2.0;
        box.translate(-center).scale(ScrollOverview::Config::getScale(monitor)).translate(center);
        if (box.intersection(CBox{Vector2D{}, monitor->m_size}).empty())
            return false;
    }
    return true;
}

SDispatchResult dispatch(std::string argument) {
    if (OverviewSearchInput::passingHistoricalRelease())
        return {};
    if (!validArgument(argument))
        return {.success = false, .error = "expected open|toggle|close|query <UTF-8 text>; query limit is 4096 bytes, no controls"};
    if (!inputAvailable && argument != "close")
        return {.success = false, .error = "search input hooks unavailable for this Hyprland build"};
    // Close/toggle address the modal owner, not whichever monitor gained focus.
    if (owner)
        return owner->action(argument);
    if (argument == "close")
        return {};
    const auto monitor = Desktop::focusState()->monitor();
    auto overview = monitor ? scrollOverviewForMonitor(monitor) : activeScrollOverview();
    if (!overview)
        overview = openOverview(monitor).overview;
    auto* concrete = overview ? dynamic_cast<CScrollOverview*>(overview.get()) : nullptr;
    if (!concrete)
        return {.success = false, .error = "could not open overview on the focused monitor"};
    return concrete->windowSearchAction(argument);
}

void initInput() {
    if (!listeners.empty())
        return;
    inputAvailable = OverviewSearchInput::initialize();
    auto& events = Event::bus()->m_events;
    listeners.emplace_back(events.input.keyboard.key.listen([](IKeyboard::SKeyEvent event, Event::SCallbackInfo& info) {
        if (info.cancelled)
            return;
        const auto device = OverviewSearchInput::keyboard();
        if (event.state == WL_KEYBOARD_KEY_STATE_RELEASED) {
            if (captured(consumedKeys, device, event.keycode, true))
                info.cancelled = true;
            else if (owner)
                OverviewSearchInput::suppressHistoricalReleaseBinds();
            return;
        }
        if (!owner) {
            if (captured(consumedKeys, device, event.keycode))
                info.cancelled = true;
            return;
        }
        info.cancelled = true;
        // A repeated press from the shortcut which opened search still belongs
        // to Hyprland. Its eventual release must take the historical path.
        if (OverviewSearchInput::hasPressRecord())
            return;
        if (!captured(consumedKeys, device, event.keycode))
            consumedKeys.push_back({device, event.keycode});
        owner->key(event);
    }));
    listeners.emplace_back(events.input.mouse.button.listen([](IPointer::SButtonEvent event, Event::SCallbackInfo& info) {
        if (info.cancelled)
            return;
        const auto device = OverviewSearchInput::pointer();
        if (event.state == WL_POINTER_BUTTON_STATE_RELEASED) {
            if (captured(consumedButtons, device, event.button, true))
                info.cancelled = true;
            else if (owner)
                OverviewSearchInput::suppressHistoricalReleaseBinds();
            return;
        }
        if (!owner)
            return;
        info.cancelled = true;
        if (OverviewSearchInput::hasPressRecord())
            return;
        if (!captured(consumedButtons, device, event.button))
            consumedButtons.push_back({device, event.button});
        owner->pointerButton(event.button);
    }));
    listeners.emplace_back(events.input.mouse.move.listen([](Vector2D, Event::SCallbackInfo& info) {
        if (!info.cancelled && owner)
            info.cancelled = true;
    }));
    listeners.emplace_back(events.input.mouse.axis.listen([](IPointer::SAxisEvent event, Event::SCallbackInfo& info) {
        if (info.cancelled || !owner)
            return;
        info.cancelled = true;
        if (event.delta != 0)
            owner->moveSelection(event.delta > 0 ? 1 : -1);
    }));
    listeners.emplace_back(events.input.touch.down.listen([](ITouch::SDownEvent event, Event::SCallbackInfo& info) {
        if (!info.cancelled && owner) {
            consumedTouches.insert(event.touchID);
            info.cancelled = true;
        }
    }));
    listeners.emplace_back(events.input.touch.motion.listen([](ITouch::SMotionEvent event, Event::SCallbackInfo& info) {
        if (!info.cancelled && (owner || consumedTouches.contains(event.touchID)))
            info.cancelled = true;
    }));
    listeners.emplace_back(events.input.touch.up.listen([](ITouch::SUpEvent event, Event::SCallbackInfo& info) {
        if (info.cancelled)
            return;
        const bool captured = consumedTouches.erase(event.touchID) != 0;
        if (owner || captured)
            info.cancelled = true;
    }));
    listeners.emplace_back(events.input.touch.cancel.listen([](ITouch::SCancelEvent event, Event::SCallbackInfo& info) {
        if (info.cancelled)
            return;
        const bool captured = consumedTouches.erase(event.touchID) != 0;
        if (owner || captured)
            info.cancelled = true;
    }));
    // Rebuild from live metadata at render and before every action. These hooks
    // also wake an otherwise idle overview on title/class/workspace changes.
    const auto windowChanged = [] { changed(); };
    listeners.emplace_back(events.window.open.listen(windowChanged));
    listeners.emplace_back(events.window.close.listen(windowChanged));
    listeners.emplace_back(events.window.title.listen(windowChanged));
    listeners.emplace_back(events.window.class_.listen(windowChanged));
    listeners.emplace_back(events.window.pin.listen(windowChanged));
    listeners.emplace_back(events.window.fullscreen.listen(windowChanged));
    listeners.emplace_back(events.window.floating.listen(windowChanged));
    listeners.emplace_back(events.window.updateState.listen(windowChanged));
    listeners.emplace_back(events.window.minimize.listen(windowChanged));
    listeners.emplace_back(events.window.active.listen(windowChanged));
    listeners.emplace_back(events.window.moveToWorkspace.listen(windowChanged));
    listeners.emplace_back(events.workspace.renamed.listen(windowChanged));
    listeners.emplace_back(events.workspace.moveToMonitor.listen(windowChanged));
    listeners.emplace_back(events.monitor.layoutChanged.listen([] { changed(); }));
    listeners.emplace_back(events.monitor.removed.listen(windowChanged));
    listeners.emplace_back(events.monitor.destroyMon.listen(windowChanged));
}

void shutdownInput() {
    if (owner)
        owner->cancel();
    listeners.clear();
    consumedKeys.clear();
    consumedButtons.clear();
    consumedTouches.clear();
    OverviewSearchInput::shutdown();
    inputAvailable = false;
}
}

COverviewSearch::COverviewSearch(CScrollOverview& overview) : m_overview(overview) {}
COverviewSearch::~COverviewSearch() {
    if (owner == this)
        owner = nullptr;
}

bool COverviewSearch::active() const {
    return owner == this;
}

void COverviewSearch::repaint() {
    m_dirty = true;
    const auto monitor = m_overview.pMonitor.lock();
    if (!monitor || !monitor->m_enabled)
        return;
    m_overview.requestInputFrame();
    m_overview.damage();
}

void COverviewSearch::changed() {
    if (!active())
        return;
    const auto monitor = m_overview.pMonitor.lock();
    if (!monitor || !monitor->m_enabled) {
        cancel();
        return;
    }
    repaint();
}

void COverviewSearch::cancel() {
    if (!active())
        return;
    owner = nullptr;
    m_windows.clear();
    m_candidates.clear();
    m_matches.clear();
    // Keep the GPU texture until the next render or overview destruction,
    // where the renderer's EGL context is current.
    repaint();
}

SDispatchResult COverviewSearch::action(const std::string& argument) {
    if (!OverviewSearch::validArgument(argument))
        return {.success = false, .error = "invalid search argument"};
    if (argument == "close" || (argument == "toggle" && active())) {
        cancel();
        return {};
    }
    if (m_overview.isClosing())
        m_overview.reopen();
    if (!active()) {
        if (owner)
            owner->cancel();
        m_editor.set("");
        m_selected = m_first = 0;
        owner = this;
    }
    if (argument == "query" || argument.starts_with("query ")) {
        m_editor.set(argument == "query" ? std::string_view{} : std::string_view{argument}.substr(6));
        m_selected = m_first = 0;
    }
    m_message.clear();
    refresh();
    repaint();
    return {};
}

void COverviewSearch::refresh() {
    const auto selectedWindow = m_selected < m_matches.size() ? m_windows[m_matches[m_selected]].lock() : PHLWINDOW{};
    m_candidates.clear();
    m_windows.clear();
    for (const auto& window : Desktop::windowState()->windows()) {
        if (!OverviewSearch::eligible(window))
            continue;
        const auto workspace = window->m_workspace;
        WindowSearch::Candidate candidate;
        candidate.identity = window->metadata().stableID();
        candidate.title = window->metadata().title();
        candidate.appID = window->metadata().appID();
        candidate.workspaceName = workspace->displayName();
        if (workspace->addressableName() != candidate.workspaceName)
            candidate.workspaceName += " " + workspace->addressableName();
        if (const auto number = workspace->numberedID())
            candidate.workspaceNumber = *number;
        m_candidates.emplace_back(std::move(candidate));
        m_windows.emplace_back(window);
    }
    m_matches = WindowSearch::match(m_candidates, m_editor.text());
    m_selected = std::min(m_selected, m_matches.empty() ? size_t{0} : m_matches.size() - 1);
    if (selectedWindow) {
        for (size_t i = 0; i < m_matches.size(); ++i) {
            if (m_windows[m_matches[i]].lock() == selectedWindow) {
                m_selected = i;
                break;
            }
        }
    }
    if (m_selected < m_first)
        m_first = m_selected;
    if (m_selected >= m_first + m_rows)
        m_first = m_selected - m_rows + 1;
}

void COverviewSearch::moveSelection(int delta) {
    refresh();
    if (!m_matches.empty()) {
        const auto count = static_cast<int64_t>(m_matches.size());
        m_selected = (static_cast<int64_t>(m_selected) + delta % count + count) % count;
    }
    m_message.clear();
    repaint();
}

void COverviewSearch::confirm() {
    // Do not silently activate a different row if the selected client vanished
    // or ceased to match since the last frame.
    const auto selected = m_selected < m_matches.size() ? m_windows[m_matches[m_selected]].lock() : PHLWINDOW{};
    refresh();
    if (!selected || !OverviewSearch::eligible(selected) ||
        std::ranges::none_of(m_matches, [&](size_t i) { return m_windows[i].lock() == selected; })) {
        m_message = "Window changed or closed; select a result again";
        repaint();
        return;
    }
    const auto monitor = selected->m_workspace->m_monitor.lock();
    auto destination = openOverview(monitor).overview;
    auto* overview = destination ? dynamic_cast<CScrollOverview*>(destination.get()) : nullptr;
    if (!overview || !overview->revealSearchWindow(selected)) {
        m_message = "Window is no longer available in the overview";
        repaint();
        return;
    }
    cancel(); // Never close the source or destination overview.
}

void COverviewSearch::key(const IKeyboard::SKeyEvent& event) {
    const auto keyboard = OverviewSearchInput::keyboard();
    auto* state = keyboard ? keyboard->m_xkbState : nullptr;
    if (!state)
        return;
    const auto code = event.keycode + 8;
    const auto sym = xkb_state_key_get_one_sym(state, code);
    const bool control = xkb_state_mod_name_is_active(state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0;
    const bool alt = xkb_state_mod_name_is_active(state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0;
    const bool logo = xkb_state_mod_name_is_active(state, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0;
    switch (sym) {
        case XKB_KEY_Escape: cancel(); return;
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter: confirm(); return;
        case XKB_KEY_Up:
        case XKB_KEY_KP_Up: moveSelection(-1); return;
        case XKB_KEY_Down:
        case XKB_KEY_KP_Down: moveSelection(1); return;
        case XKB_KEY_Tab: moveSelection(1); return;
        case XKB_KEY_ISO_Left_Tab: moveSelection(-1); return;
        case XKB_KEY_Page_Up: moveSelection(-static_cast<int>(m_rows)); return;
        case XKB_KEY_Page_Down: moveSelection(static_cast<int>(m_rows)); return;
        case XKB_KEY_Left:
        case XKB_KEY_KP_Left: m_editor.left(); break;
        case XKB_KEY_Right:
        case XKB_KEY_KP_Right: m_editor.right(); break;
        case XKB_KEY_Home: m_editor.home(); break;
        case XKB_KEY_End: m_editor.end(); break;
        case XKB_KEY_BackSpace: m_editor.backspace(); break;
        case XKB_KEY_Delete:
        case XKB_KEY_KP_Delete: m_editor.eraseForward(); break;
        default: {
            if (control && (sym == XKB_KEY_u || sym == XKB_KEY_U)) {
                m_editor.set("");
                break;
            }
            if (control || alt || logo)
                return;
            const int size = xkb_state_key_get_utf8(state, code, nullptr, 0);
            if (size <= 0 || size > static_cast<int>(WindowSearch::MAX_QUERY_BYTES))
                return;
            std::string input(size + 1, '\0');
            xkb_state_key_get_utf8(state, code, input.data(), input.size());
            input.resize(size);
            if (!WindowSearch::validQuery(input) || m_editor.text().size() + input.size() > WindowSearch::MAX_QUERY_BYTES)
                return;
            m_editor.insert(input);
        }
    }
    m_message.clear();
    refresh();
    repaint();
}

void COverviewSearch::updateGeometry() {
    const auto monitor = m_overview.pMonitor.lock();
    if (!monitor)
        return;
    m_pixelScale = monitor->m_scale;
    const auto size = monitor->m_transformedSize / m_pixelScale;
    const double width = std::max(80.0, std::min(760.0, size.x - 32.0));
    m_rows = std::clamp(static_cast<int>((size.y - 160.0) / 58.0), 1, 8);
    const double height = 112.0 + 58.0 * m_rows;
    m_box = CBox{(size.x - width) / 2.0, std::max(8.0, (size.y - height) * 0.22), width, height};
    m_box.scale(m_pixelScale);
}

void COverviewSearch::pointerButton(uint32_t button) {
    if (button != BTN_LEFT)
        return;
    const auto monitor = m_overview.pMonitor.lock();
    if (!monitor)
        return;
    updateGeometry();
    const auto point = (g_pInputManager->getMouseCoordsInternal() - monitor->m_position) * monitor->m_scale;
    if (!m_box.containsPoint(point))
        return;
    const double y = (point.y - m_box.y) / m_pixelScale;
    if (y < 76.0 || y >= 76.0 + 58.0 * m_rows)
        return;
    const auto row = m_first + static_cast<size_t>((y - 76.0) / 58.0);
    if (row >= m_matches.size())
        return;
    m_selected = row;
    // Mouse chooses the row; Enter confirms, avoiding accidental jumps while
    // results are changing beneath the pointer.
    refresh();
    m_message.clear();
    repaint();
}

void COverviewSearch::buildTexture() {
    const int width = std::max(1, static_cast<int>(std::ceil(m_box.w)));
    const int height = std::max(1, static_cast<int>(std::ceil(m_box.h)));
    auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    auto* cr = cairo_create(surface);
    cairo_scale(cr, m_pixelScale, m_pixelScale);
    const double w = width / m_pixelScale;
    cairo_set_source_rgba(cr, 0.035, 0.05, 0.08, 0.98);
    cairo_paint(cr);
    cairo_set_source_rgb(cr, 0.24, 0.39, 0.57);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, 0.5, 0.5, w - 1, height / m_pixelScale - 1);
    cairo_stroke(cr);
    text(cr, "WINDOW SEARCH  /  ALL WORKSPACES", 18, 10, w - 36, 11, true);

    // Query scrolls horizontally to keep the codepoint cursor visible.
    auto* query = pango_cairo_create_layout(cr);
    auto* font = pango_font_description_from_string("Sans");
    pango_font_description_set_absolute_size(font, 20 * PANGO_SCALE);
    pango_layout_set_font_description(query, font);
    pango_layout_set_text(query, m_editor.text().c_str(), m_editor.text().size());
    pango_layout_set_single_paragraph_mode(query, true);
    PangoRectangle cursor;
    pango_layout_get_cursor_pos(query, m_editor.cursor(), &cursor, nullptr);
    const double caret = static_cast<double>(cursor.x) / PANGO_SCALE;
    const double shift = std::max(0.0, caret - (w - 44));
    cairo_save(cr);
    cairo_rectangle(cr, 18, 31, w - 36, 34);
    cairo_clip(cr);
    cairo_set_source_rgb(cr, 0.95, 0.97, 1.0);
    cairo_move_to(cr, 18 - shift, 34);
    pango_cairo_show_layout(cr, query);
    cairo_set_source_rgb(cr, 0.4, 0.75, 1.0);
    cairo_rectangle(cr, 18 + caret - shift, 34, 2, 25);
    cairo_fill(cr);
    cairo_restore(cr);
    pango_font_description_free(font);
    g_object_unref(query);

    if (m_matches.empty())
        text(cr, "No matching windows", 18, 91, w - 36, 18, true);
    for (size_t row = 0; row < m_rows && m_first + row < m_matches.size(); ++row) {
        const size_t index = m_first + row;
        const auto& candidate = m_candidates[m_matches[index]];
        const double y = 76 + row * 58;
        if (index == m_selected) {
            cairo_set_source_rgb(cr, 0.12, 0.23, 0.36);
            cairo_rectangle(cr, 8, y, w - 16, 55);
            cairo_fill(cr);
        }
        text(cr, candidate.title.empty() ? "(untitled window)" : candidate.title, 18, y + 5, w - 36, 17);
        const auto window = m_windows[m_matches[index]].lock();
        const auto monitor = window ? window->m_monitor.lock() : PHLMONITOR{};
        const auto workspace = window ? window->m_workspace : PHLWORKSPACE{};
        std::string detail = candidate.appID + "  ·  " + (workspace ? workspace->displayName() : candidate.workspaceName);
        if (candidate.workspaceNumber)
            detail += " [" + std::to_string(*candidate.workspaceNumber) + "]";
        if (monitor)
            detail += "  ·  " + monitor->m_name;
        text(cr, detail, 18, y + 30, w - 36, 12, true);
    }
    std::string footer = m_message;
    if (footer.empty())
        footer = std::to_string(m_matches.empty() ? 0 : m_selected + 1) + "/" + std::to_string(m_matches.size()) +
            "   ↑↓ / Tab: select   Enter: show in overview   Esc: cancel";
    text(cr, footer, 18, 83 + m_rows * 58, w - 36, 12, true);
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    m_texture = g_pHyprRenderer->createTexture(surface);
    cairo_surface_destroy(surface);
    m_dirty = false;
}

void COverviewSearch::render() {
    if (!active() || m_overview.isClosing())
        return;
    updateGeometry();
    refresh();
    // Cache rasterization while still checking current global metadata each
    // frame. Names and weak references never serve as object identities.
    std::string key = m_editor.text() + '\0' + std::to_string(m_editor.cursor()) + ":" +
        std::to_string(m_selected) + ":" + std::to_string(m_first) + ":" +
        std::to_string(m_box.w) + ":" + std::to_string(m_box.h) + ":" + std::to_string(m_pixelScale) + m_message;
    for (const auto index : m_matches) {
        const auto& c = m_candidates[index];
        key += '\0' + std::to_string(c.identity) + '\0' + c.title + '\0' + c.appID + '\0' + c.workspaceName;
        if (c.workspaceNumber)
            key += std::to_string(*c.workspaceNumber);
        const auto window = m_windows[index].lock();
        if (window && window->m_monitor)
            key += '\0' + window->m_monitor->m_name;
    }
    if (m_dirty || key != m_textureKey || !m_texture) {
        m_textureKey = std::move(key);
        buildTexture();
    }
    if (!m_texture)
        return;
    auto& context = g_pHyprRenderer->context();
    Render::IHyprRenderer::currentPass(context).add(makeUnique<CTexPassElement>(CTexPassElement::SRenderData{
        .tex = m_texture, .box = m_box,
    }));
    OverviewRender::flushPass(context, m_overview.pMonitor.lock());
}

SDispatchResult CScrollOverview::windowSearchAction(const std::string& argument) {
    if (!inputAvailable && argument != "close")
        return {.success = false, .error = "search input hooks unavailable for this Hyprland build"};
    if (!OverviewSearch::validArgument(argument))
        return {.success = false, .error = "invalid search argument"};
    if (argument != "close") {
        // Never steal the release of an in-progress drag, resize or gesture.
        for (const auto& item : scrollOverviews()) {
            const auto* overview = dynamic_cast<CScrollOverview*>(item.get());
            if (overview && (overview->m_isSwiping || overview->dragPendingPrimary || overview->dragActiveWindow ||
                             overview->resizePointerDown || overview->scrollingPanPointerDown))
                return {.success = false, .error = "finish the overview gesture or pointer operation before searching"};
        }
        if (g_layoutManager && g_layoutManager->dragController()->target())
            return {.success = false, .error = "finish the native window drag before searching"};
    }
    if (!windowSearch)
        windowSearch = makeUnique<COverviewSearch>(*this);
    return windowSearch->action(argument);
}

void CScrollOverview::cancelWindowSearch() {
    if (windowSearch)
        windowSearch->cancel();
}

bool CScrollOverview::isWindowSearchActive() const {
    return windowSearch && windowSearch->active();
}
