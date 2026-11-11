#include "WorkspaceTitleUI.hpp"
#include "scrollOverview.hpp"
#include "SearchInput.hpp"
#include "OverviewRender.hpp"

#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/workspace/RegularWorkspace.hpp>
#include <hyprland/src/workspace/query/Query.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <pango/pangocairo.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <unordered_set>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace {
CWorkspaceTitleUI* g_editor = nullptr;
std::unique_ptr<WorkspaceTitles::Store> g_store;
template <typename Device> struct SConsumedInput {
    WP<Device> device;
    uint32_t code;
};
std::vector<SConsumedInput<IKeyboard>> g_keys;
std::vector<SConsumedInput<IPointer>> g_buttons;
CHyprSignalListener g_keyHook, g_buttonHook, g_axisHook;

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

void notifyError(const std::string& error) {
    HyprlandAPI::addNotification(SCROLLOVERVIEW_HANDLE,
        "[scrolloverview] Workspace title: " + error,
        CHyprColor{1.0, 0.3, 0.2, 1.0}, 7000);
}

void damageOverviews() {
    for (const auto& overview : scrollOverviews())
        overview->damage();
}

// This is a direct Lua function, not generated Lua source or a shell command.
int setTitleLua(lua_State* L) {
    if (lua_gettop(L) != 2 || lua_type(L, 1) != LUA_TSTRING || lua_type(L, 2) != LUA_TSTRING)
        return luaL_error(L, "set_workspace_title: expected selector and title strings");
    bool failed = false;
    {
        size_t keySize = 0, titleSize = 0;
        const char* key = lua_tolstring(L, 1, &keySize);
        const char* title = lua_tolstring(L, 2, &titleSize);
        std::string error;
        if (!g_store || !g_store->set(std::string(key, keySize), std::string(title, titleSize), error)) {
            lua_pushfstring(L, "set_workspace_title: %s", error.c_str());
            failed = true;
        } else {
            if (g_editor)
                g_editor->cancel();
            damageOverviews();
        }
    }
    // Do not longjmp across C++ string destructors.
    return failed ? lua_error(L) : 0;
}

// Keep the insertion point visible even for long titles. Both measurements
// and CTextResource use points. The texture is bounded to the title bar, never
// the unbounded width of a 4096-byte title.
std::string editorDisplay(const WorkspaceTitles::Editor& editor, int width,
                          int fontSize, const std::string& font) {
    auto* context = pango_font_map_create_context(pango_cairo_font_map_get_default());
    auto* layout = pango_layout_new(context);
    auto* desc = pango_font_description_from_string(font.c_str());
    pango_font_description_set_size(desc, fontSize * PANGO_SCALE);
    pango_layout_set_font_description(layout, desc);
    std::vector<size_t> boundaries{0};
    for (size_t i = 1; i <= editor.cursor; ++i) {
        if (i == editor.cursor || (static_cast<unsigned char>(editor.text[i]) & 0xC0) != 0x80)
            boundaries.push_back(i);
    }
    size_t low = 0, high = boundaries.size() - 1;
    while (low < high) {
        const auto middle = (low + high) / 2;
        const auto start = boundaries[middle];
        const std::string prefix = (start ? "…" : "") + editor.text.substr(start, editor.cursor - start) + "│";
        pango_layout_set_text(layout, prefix.c_str(), prefix.size());
        int pixels = 0;
        pango_layout_get_pixel_size(layout, &pixels, nullptr);
        if (pixels <= width * 0.8)
            high = middle;
        else
            low = middle + 1;
    }
    const auto start = boundaries[low];
    pango_font_description_free(desc);
    g_object_unref(layout);
    g_object_unref(context);
    return (start ? "…" : "") + editor.text.substr(start, editor.cursor - start) + "│" + editor.text.substr(editor.cursor);
}
}

void WorkspaceTitleInput::init() {
    g_store = std::make_unique<WorkspaceTitles::Store>(WorkspaceTitles::Store::defaultPath());
    std::string error;
    if (!g_store->load(error))
        notifyError(error);

    // Register before any overview's default/submap listeners. Always retain
    // release tracking until physical release, including after cancel/close.
    g_keyHook = Event::bus()->m_events.input.keyboard.key.listen([](IKeyboard::SKeyEvent event, Event::SCallbackInfo& info) {
        const auto device = OverviewSearchInput::keyboard();
        if (event.state == WL_KEYBOARD_KEY_STATE_RELEASED) {
            if (captured(g_keys, device, event.keycode, true))
                info.cancelled = true;
            else if (!info.cancelled && g_editor && !OverviewSearchInput::passingHistoricalRelease())
                OverviewSearchInput::suppressHistoricalReleaseBinds();
            return;
        }
        if (info.cancelled || OverviewSearchInput::passingHistoricalRelease())
            return;
        if (g_editor) {
            info.cancelled = true;
            if (OverviewSearchInput::hasPressRecord())
                return;
            if (!captured(g_keys, device, event.keycode))
                g_keys.push_back({device, event.keycode});
            g_editor->key(event.keycode);
        } else if (captured(g_keys, device, event.keycode))
            info.cancelled = true;
    });
    g_buttonHook = Event::bus()->m_events.input.mouse.button.listen([](IPointer::SButtonEvent event, Event::SCallbackInfo& info) {
        const auto device = OverviewSearchInput::pointer();
        if (event.state == WL_POINTER_BUTTON_STATE_RELEASED) {
            if (captured(g_buttons, device, event.button, true))
                info.cancelled = true;
            else if (!info.cancelled && g_editor && !OverviewSearchInput::passingHistoricalRelease())
                OverviewSearchInput::suppressHistoricalReleaseBinds();
            return;
        }
        if (info.cancelled || OverviewSearchInput::passingHistoricalRelease() || !g_editor)
            return;
        info.cancelled = true;
        if (OverviewSearchInput::hasPressRecord())
            return;
        if (!captured(g_buttons, device, event.button))
            g_buttons.push_back({device, event.button});
        // A click outside cancels without selecting/closing/dragging a window.
        if (!g_editor->mouseButton(event.button, true))
            g_editor->cancel();
    });
    g_axisHook = Event::bus()->m_events.input.mouse.axis.listen([](IPointer::SAxisEvent, Event::SCallbackInfo& info) {
        if (!info.cancelled && g_editor)
            info.cancelled = true;
    });
    if (::Config::mgr()->type() == ::Config::CONFIG_LUA)
        HyprlandAPI::addLuaFunction(SCROLLOVERVIEW_HANDLE, "scrolloverview", "set_workspace_title", setTitleLua);
}

void WorkspaceTitleInput::shutdown() {
    if (g_editor)
        g_editor->cancel();
    g_keyHook.reset();
    g_buttonHook.reset();
    g_axisHook.reset();
    g_keys.clear();
    g_buttons.clear();
    g_store.reset();
}

bool WorkspaceTitleInput::active() {
    return g_editor != nullptr;
}

std::string WorkspaceTitleInput::customTitle(const std::string& selector) {
    return g_store ? g_store->get(selector) : std::string{};
}

CWorkspaceTitleUI::CWorkspaceTitleUI(CScrollOverview& overview) : m_overview(overview) {}
CWorkspaceTitleUI::~CWorkspaceTitleUI() { cancel(); }
bool CWorkspaceTitleUI::editing() const { return g_editor == this; }

void CWorkspaceTitleUI::cancel() {
    const bool wasEditing = editing();
    if (wasEditing)
        g_editor = nullptr;
    m_selector.clear();
    m_editor.reset("");
    m_saveFailed = false;
    if (wasEditing && m_overview.pMonitor)
        m_overview.damage();
}

CBox CWorkspaceTitleUI::titleBox(size_t index) const {
    const auto monitor = m_overview.pMonitor.lock();
    if (!monitor || !ScrollOverview::Config::getValue<bool>("plugin:scrolloverview:titles:enabled", monitor))
        return {};
    auto box = m_overview.workspaceTitlePreviewBox(index);
    const double height = std::clamp(ScrollOverview::Config::getValue<int>("plugin:scrolloverview:titles:height", monitor), 16, 128) * monitor->m_scale;
    // Inset, rather than put the bar in workspace_gap: zero gap is supported.
    // Never expand to the overflowing window extents of adjacent workspaces.
    box.height = std::min(box.height, height);
    return box;
}

void CWorkspaceTitleUI::begin(size_t index) {
    if (!OverviewSearchInput::available()) {
        notifyError("Text input hooks are unavailable for this Hyprland build");
        return;
    }
    for (const auto& item : scrollOverviews()) {
        if (auto* overview = dynamic_cast<CScrollOverview*>(item.get()))
            overview->cancelWindowSearch();
    }
    const auto& workspace = m_overview.images[index]->pWorkspace;
    if (g_editor)
        g_editor->cancel();
    m_selector = Workspace::selector(*workspace);
    auto title = g_store->get(m_selector);
    if (title.empty())
        title = workspace->displayName();
    // Native names need not obey our single-line editor rules.
    if (!WorkspaceTitles::validTitle(title))
        title.clear();
    m_editor.reset(std::move(title));
    m_saveFailed = false;
    g_editor = this;
    m_overview.requestInputFrame();
    m_overview.damage();
}

bool CWorkspaceTitleUI::mouseButton(uint32_t button, bool pressed) {
    const auto monitor = m_overview.pMonitor.lock();
    if (!pressed || !monitor || m_overview.closing || !g_store ||
        m_overview.submapMouseClickPending || m_overview.dragPendingPrimary || m_overview.dragActiveWindow ||
        m_overview.resizePointerDown || m_overview.scrollingPanPointerDown ||
        scrollOverviewAt(g_pInputManager->getMouseCoordsInternal()).get() != &m_overview)
        return false;
    const auto point = (g_pInputManager->getMouseCoordsInternal() - monitor->m_position) * monitor->m_scale;
    const uint32_t primary = ScrollOverview::Config::getLeftHanded(monitor) ? BTN_RIGHT : BTN_LEFT;
    // Reverse paint order for overlapping cards during workspace animations.
    for (size_t i = m_overview.images.size(); i-- > 0;) {
        if (m_overview.workspaceOverviewAlpha(i) < 0.05F)
            continue;
        const auto box = titleBox(i);
        if (box.width <= 0 || box.height <= 0 || !box.containsPoint(point))
            continue;
        const auto device = OverviewSearchInput::pointer();
        if (!captured(g_buttons, device, button))
            g_buttons.push_back({device, button});
        if (button == primary) {
            const auto selector = Workspace::selector(*m_overview.images[i]->pWorkspace);
            // Clicking the current title preserves unsaved text.
            if (!editing() || selector != m_selector)
                begin(i);
        }
        return true;
    }
    return false;
}

void CWorkspaceTitleUI::save() {
    std::string error;
    if (!g_store->set(m_selector, m_editor.text, error)) {
        m_saveFailed = true;
        notifyError(error);
        m_overview.damage();
        return;
    }
    cancel();
    damageOverviews();
}

void CWorkspaceTitleUI::key(uint32_t keycode) {
    const auto keyboard = OverviewSearchInput::keyboard();
    if (!keyboard || !keyboard->m_xkbState)
        return;
    auto* state = keyboard->m_xkbState; // actual layout/modifiers, NOT bind-symbol state
    const auto code = keycode + 8;
    const auto sym = xkb_state_key_get_one_sym(state, code);
    switch (sym) {
        case XKB_KEY_Escape: cancel(); return;
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter: save(); return;
        case XKB_KEY_BackSpace: m_editor.backspace(); break;
        case XKB_KEY_Delete:
        case XKB_KEY_KP_Delete: m_editor.deleteForward(); break;
        case XKB_KEY_Left:
        case XKB_KEY_KP_Left: m_editor.left(); break;
        case XKB_KEY_Right:
        case XKB_KEY_KP_Right: m_editor.right(); break;
        case XKB_KEY_Home:
        case XKB_KEY_KP_Home: m_editor.home(); break;
        case XKB_KEY_End:
        case XKB_KEY_KP_End: m_editor.end(); break;
        default: {
            // Suppress shortcuts; allow Shift and AltGr as text modifiers.
            if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0 ||
                xkb_state_mod_name_is_active(state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0 ||
                xkb_state_mod_name_is_active(state, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0)
                break;
            const int size = xkb_state_key_get_utf8(state, code, nullptr, 0);
            if (size <= 0 || size > 4096)
                break;
            std::string text(size + 1, '\0');
            xkb_state_key_get_utf8(state, code, text.data(), text.size());
            text.resize(size);
            m_editor.insert(text);
            break;
        }
    }
    m_overview.requestInputFrame();
    m_overview.damage();
}

void CWorkspaceTitleUI::render() {
    const auto monitor = m_overview.pMonitor.lock();
    if (!monitor || !g_store)
        return;
    const bool enabled = ScrollOverview::Config::getValue<bool>("plugin:scrolloverview:titles:enabled", monitor);
    if (editing() && (!enabled || std::ranges::none_of(m_overview.images, [this](const auto& image) {
            return image && image->pWorkspace && Workspace::selector(*image->pWorkspace) == m_selector;
        })))
        cancel();
    if (!enabled || m_overview.closing)
        return;
    const auto font = ScrollOverview::Config::getValue<std::string>("plugin:scrolloverview:titles:font", monitor);
    const int configuredSize = std::clamp(ScrollOverview::Config::getValue<int>("plugin:scrolloverview:titles:font_size", monitor), 6, 64);
    std::unordered_set<std::string> live;
    auto& context = g_pHyprRenderer->context();
    auto& pass = Render::IHyprRenderer::currentPass(context);
    // draw(..., {}) is immediate and supplies empty damage, so it paints
    // nothing. Queue titles and let the pass calculate their draw regions.
    for (size_t i = 0; i < m_overview.images.size(); ++i) {
        const auto& workspace = m_overview.images[i]->pWorkspace;
        const auto selector = Workspace::selector(*workspace);
        live.insert(selector);
        const auto bar = titleBox(i);
        const CBox screen{0, 0, monitor->m_transformedSize.x, monitor->m_transformedSize.y};
        if (bar.width <= 0 || bar.height <= 0 || !bar.overlaps(screen))
            continue;
        const float alpha = m_overview.workspaceOverviewAlpha(i);
        const bool active = editing() && selector == m_selector;
        const auto color = active ? (m_saveFailed ? CHyprColor{0.55F, 0.08F, 0.05F, 0.98F * alpha} : CHyprColor{0.08F, 0.24F, 0.36F, 0.98F * alpha})
                                  : CHyprColor{0.06F, 0.07F, 0.09F, 0.94F * alpha};
        pass.add(makeUnique<CRectPassElement>(CRectPassElement::SRectData{.box = bar, .color = color}));
        const int padding = std::max(1, static_cast<int>(6 * monitor->m_scale));
        const int width = static_cast<int>(bar.width) - 2 * padding;
        const double dpi = pango_cairo_font_map_get_resolution(PANGO_CAIRO_FONT_MAP(pango_cairo_font_map_get_default()));
        const double pixelsToPoints = 72.0 / (dpi > 0 ? dpi : 96.0);
        const int fontSize = std::max(1, static_cast<int>(std::min(static_cast<double>(configuredSize) * monitor->m_scale, bar.height * 0.60) * pixelsToPoints));
        if (width <= fontSize * 2)
            continue;
        auto text = active ? m_editor.text : g_store->get(selector);
        if (!active && text.empty())
            text = workspace->displayName();
        // The cache signature precedes expensive Pango/text rasterization.
        const auto signature = std::format("{}:{}:{}:{}:{}:{}:{}", width, fontSize, font.size(), font, active, active ? m_editor.cursor : 0, text);
        auto& cached = m_textures[selector];
        if (signature != cached.signature || !cached.texture) {
            if (active)
                text = editorDisplay(m_editor, width, fontSize, font);
            Hyprgraphics::CTextResource::STextResourceData data;
            data.text = text;
            data.font = font;
            data.fontSize = fontSize;
            data.maxSize = Vector2D{static_cast<double>(width), bar.height};
            data.wrap = false;
            data.ellipsize = true;
            cached.texture = g_pHyprRenderer->renderText(std::move(data));
            cached.signature = signature;
        }
        if (cached.texture) {
            const auto size = cached.texture->m_size;
            pass.add(makeUnique<CTexPassElement>(CTexPassElement::SRenderData{
                .tex = cached.texture,
                .box = CBox{bar.x + padding, bar.y + (bar.height - size.y) / 2, size.x, size.y},
                .a = alpha,
                .clipBox = bar,
            }));
        }
    }
    std::erase_if(m_textures, [&live](const auto& entry) { return !live.contains(entry.first); });
    // Finish before top/overlay layers are submitted, matching preview order.
    OverviewRender::flushPass(context, monitor);
}
