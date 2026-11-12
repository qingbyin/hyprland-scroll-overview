#include "WorkspaceTitleUI.hpp"
#include "scrollOverview.hpp"
#include "SearchInput.hpp"
#include "OverviewRender.hpp"

#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
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
std::unique_ptr<WorkspaceTitleIME::Client> g_ime;
SP<CEventLoopTimer> g_imeTimer;
uint64_t g_inputGeneration = 0;
template <typename Device> struct SConsumedInput {
    WP<Device> device;
    uint32_t code;
    uint64_t generation = 0;
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

    g_ime = std::make_unique<WorkspaceTitleIME::Client>(
        [](const std::string& text) { if (g_editor) g_editor->commitText(text); },
        [](const WorkspaceTitleIME::Key& key) { if (g_editor) g_editor->unhandledKey(key); },
        [](const WorkspaceTitleIME::Panel& panel) { if (g_editor) g_editor->updateInputPanel(panel); });
    g_imeTimer = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer> timer, void*) {
        if (g_editor && g_ime)
            g_ime->poll();
        timer->updateTimeout(g_editor ? std::optional<Time::steady_dur>{std::chrono::milliseconds(8)} : std::nullopt);
    }, nullptr);
    g_pEventLoopManager->addTimer(g_imeTimer);

    // Register before any overview's default/submap listeners. Always retain
    // release tracking until physical release, including after cancel/close.
    g_keyHook = Event::bus()->m_events.input.keyboard.key.listen([](IKeyboard::SKeyEvent event, Event::SCallbackInfo& info) {
        const auto device = OverviewSearchInput::keyboard();
        if (event.state == WL_KEYBOARD_KEY_STATE_RELEASED) {
            const bool current = std::ranges::any_of(g_keys, [&](const auto& input) {
                return input.device.lock() == device && input.code == event.keycode && input.generation == g_inputGeneration;
            });
            if (captured(g_keys, device, event.keycode, true)) {
                info.cancelled = true;
                if (current && g_editor)
                    g_editor->key(event.keycode, true, event.timeMs);
            } else if (!info.cancelled && g_editor && !OverviewSearchInput::passingHistoricalRelease())
                OverviewSearchInput::suppressHistoricalReleaseBinds();
            return;
        }
        if (info.cancelled || OverviewSearchInput::passingHistoricalRelease())
            return;
        if (g_editor) {
            info.cancelled = true;
            if (OverviewSearchInput::hasPressRecord())
                return;
            if (std::ranges::any_of(g_keys, [&](const auto& input) {
                    return input.device.lock() == device && input.code == event.keycode && input.generation != g_inputGeneration;
                }))
                return;
            if (!captured(g_keys, device, event.keycode))
                g_keys.push_back({device, event.keycode, g_inputGeneration});
            g_editor->key(event.keycode, false, event.timeMs);
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
    if (g_imeTimer) {
        g_pEventLoopManager->removeTimer(g_imeTimer);
        g_imeTimer.reset();
    }
    g_ime.reset();
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
    if (wasEditing) {
        g_editor = nullptr;
        if (g_ime)
            g_ime->end();
        if (g_imeTimer)
            g_imeTimer->updateTimeout(std::nullopt);
    }
    m_inputPanel = {};
    m_panelTextures.clear();
    m_panelBox = {};
    m_candidateBoxes.clear();
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
    const double offset = std::clamp(ScrollOverview::Config::getValue<int>("plugin:scrolloverview:titles:offset", monitor), 0, 128) * monitor->m_scale;
    // Keep the workspace geometry unchanged; drawing and input share this box.
    // Never expand to the overflowing window extents of adjacent workspaces.
    box.height = std::min(box.height, height);
    box.y -= offset;
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
    m_editor.reset(g_store->get(m_selector));
    m_saveFailed = false;
    g_editor = this;
    ++g_inputGeneration;
    g_ime->begin();
    g_imeTimer->updateTimeout(std::chrono::milliseconds(8));
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
    if (editing() && m_panelBox.containsPoint(point)) {
        if (button == primary) {
            for (const auto& [box, index] : m_candidateBoxes) {
                if (box.containsPoint(point)) {
                    g_ime->select(index);
                    break;
                }
            }
        }
        return true;
    }
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

void CWorkspaceTitleUI::key(uint32_t keycode, bool release, uint32_t time) {
    const auto keyboard = OverviewSearchInput::keyboard();
    if (!keyboard || !keyboard->m_xkbState)
        return;
    auto* state = keyboard->m_xkbState;
    WorkspaceTitleIME::Key key;
    key.code = keycode + 8;
    key.sym = xkb_state_key_get_one_sym(state, key.code);
    key.time = time;
    key.release = release;
    // Fcitx uses X11 modifier bits, not arbitrary XKB keymap indices.
    const std::pair<const char*, uint32_t> modifiers[] = {
        {XKB_MOD_NAME_SHIFT, 1U << 0}, {XKB_MOD_NAME_CAPS, 1U << 1},
        {XKB_MOD_NAME_CTRL, 1U << 2}, {XKB_MOD_NAME_ALT, 1U << 3},
        {XKB_MOD_NAME_NUM, 1U << 4}, {XKB_MOD_NAME_LOGO, 1U << 6},
        {"Mod5", 1U << 7},
    };
    for (const auto& [name, mask] : modifiers)
        if (xkb_state_mod_name_is_active(state, name, XKB_STATE_MODS_EFFECTIVE) > 0)
            key.modifiers |= mask;
    // Capture fallback text now: modifiers may change before the async reply.
    if (!release && !(key.modifiers & ((1U << 2) | (1U << 3) | (1U << 6)))) {
        const int size = xkb_state_key_get_utf8(state, key.code, nullptr, 0);
        if (size > 0 && size <= 4096) {
            key.text.resize(size + 1);
            xkb_state_key_get_utf8(state, key.code, key.text.data(), key.text.size());
            key.text.resize(size);
        }
    }
    g_ime->key(std::move(key));
}

void CWorkspaceTitleUI::commitText(const std::string& text) {
    if (editing() && m_editor.insert(text)) {
        m_saveFailed = false;
        m_overview.requestInputFrame();
        m_overview.damage();
    }
}

void CWorkspaceTitleUI::updateInputPanel(const WorkspaceTitleIME::Panel& panel) {
    m_inputPanel = panel;
    m_panelTextures.clear();
    m_panelBox = {};
    m_candidateBoxes.clear();
    m_overview.requestInputFrame();
    m_overview.damage();
}

void CWorkspaceTitleUI::unhandledKey(const WorkspaceTitleIME::Key& key) {
    if (!editing() || key.release)
        return;
    switch (key.sym) {
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
        default: m_editor.insert(key.text); break;
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
    auto* fontDescription = pango_font_description_from_string(font.c_str());
    // CTextResource overrides the description's weight with its explicit field.
    const auto fontWeight = pango_font_description_get_weight(fontDescription);
    pango_font_description_free(fontDescription);
    const auto format = ScrollOverview::Config::getValue<std::string>("plugin:scrolloverview:titles:format", monitor);
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
        if (active) {
            const auto color = m_saveFailed ? CHyprColor{247.F / 255.F, 118.F / 255.F, 142.F / 255.F, alpha} :
                                             CHyprColor{22.F / 255.F, 160.F / 255.F, 133.F / 255.F, alpha};
            const double thickness = std::min(bar.height, std::max(1.0, static_cast<double>(monitor->m_scale)));
            pass.add(makeUnique<CRectPassElement>(CRectPassElement::SRectData{
                .box = CBox{bar.x, bar.y + bar.height - thickness, bar.width, thickness}, .color = color}));
        }
        const int padding = std::max(1, static_cast<int>(6 * monitor->m_scale));
        const int width = static_cast<int>(bar.width) - 2 * padding;
        const double dpi = pango_cairo_font_map_get_resolution(PANGO_CAIRO_FONT_MAP(pango_cairo_font_map_get_default()));
        const double pixelsToPoints = 72.0 / (dpi > 0 ? dpi : 96.0);
        const int fontSize = std::max(1, static_cast<int>(std::min(static_cast<double>(configuredSize) * monitor->m_scale, bar.height * 0.60) * pixelsToPoints));
        if (width <= fontSize * 2)
            continue;
        const auto nativeName = workspace->displayName();
        auto text = active ? m_editor.text : g_store->get(selector);
        // Length-delimit fields so names, templates and titles cannot collide.
        // Check the cache before expansion, prefix measurement or rasterization.
        const auto signature = std::format("{}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}", width, fontSize, font.size(), font, active,
                                           active ? m_editor.cursor : 0, nativeName.size(), nativeName, format.size(), format, text);
        auto& cached = m_textures[selector];
        if (signature != cached.signature || !cached.texture) {
            if (active) {
                auto prefix = nativeName + "    ";
                // Leave most of the title bar to the editable text, even when
                // the native workspace name alone would fill the entire bar.
                auto* pangoContext = pango_font_map_create_context(pango_cairo_font_map_get_default());
                auto* layout = pango_layout_new(pangoContext);
                auto* desc = pango_font_description_from_string(font.c_str());
                pango_font_description_set_size(desc, fontSize * PANGO_SCALE);
                pango_layout_set_font_description(layout, desc);
                pango_layout_set_text(layout, prefix.c_str(), prefix.size());
                int prefixWidth = 0;
                pango_layout_get_pixel_size(layout, &prefixWidth, nullptr);
                const int prefixBudget = static_cast<int>(width * 0.4);
                if (prefixWidth > prefixBudget) {
                    const auto measure = [&](const std::string& candidate) {
                        pango_layout_set_text(layout, candidate.c_str(), candidate.size());
                        int pixels = 0;
                        pango_layout_get_pixel_size(layout, &pixels, nullptr);
                        return pixels;
                    };
                    // Search UTF-8 boundaries, never split a multibyte character.
                    std::vector<size_t> boundaries{0};
                    for (size_t end = 1; end <= nativeName.size(); ++end) {
                        if (end == nativeName.size() || (static_cast<unsigned char>(nativeName[end]) & 0xC0) != 0x80)
                            boundaries.push_back(end);
                    }
                    prefix = "…    ";
                    if (measure(prefix) <= prefixBudget) {
                        size_t low = 0, high = boundaries.size() - 1;
                        while (low < high) {
                            const auto middle = (low + high + 1) / 2;
                            const auto candidate = nativeName.substr(0, boundaries[middle]) + "…    ";
                            if (measure(candidate) <= prefixBudget)
                                low = middle;
                            else
                                high = middle - 1;
                        }
                        prefix = nativeName.substr(0, boundaries[low]) + "…    ";
                    } else
                        prefix = "    "; // Tiny bars retain the separator, not the name.
                    prefixWidth = measure(prefix);
                }
                pango_font_description_free(desc);
                g_object_unref(layout);
                g_object_unref(pangoContext);
                text = prefix + editorDisplay(m_editor, std::max(1, width - prefixWidth), fontSize, font);
            } else
                text = WorkspaceTitles::formatTitle(format, nativeName, text);
            if (text.empty()) {
                cached.texture.reset();
                cached.signature = signature;
                continue;
            }
            Hyprgraphics::CTextResource::STextResourceData data;
            data.text = text;
            data.font = font;
            data.fontSize = fontSize;
            data.weight = fontWeight;
            data.color = Hyprgraphics::CColor{Hyprgraphics::CColor::SSRGB{167.0 / 255.0, 175.0 / 255.0, 213.0 / 255.0}};
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
                .box = CBox{bar.x + (bar.width - size.x) / 2, bar.y + (bar.height - size.y) / 2, size.x, size.y},
                .a = alpha,
                .clipBox = bar,
            }));
        }
    }
    std::erase_if(m_textures, [&live](const auto& entry) { return !live.contains(entry.first); });
    // Finish before top/overlay layers are submitted, matching preview order.
    renderInputPanel();
    OverviewRender::flushPass(context, monitor);
}

void CWorkspaceTitleUI::renderInputPanel() {
    m_panelBox = {};
    m_candidateBoxes.clear();
    const auto monitor = m_overview.pMonitor.lock();
    if (!editing() || !monitor)
        return;
    const auto image = std::ranges::find_if(m_overview.images, [this](const auto& item) {
        return item && item->pWorkspace && Workspace::selector(*item->pWorkspace) == m_selector;
    });
    if (image == m_overview.images.end())
        return;
    const auto index = std::distance(m_overview.images.begin(), image);
    const auto bar = titleBox(index);
    const CBox screen{0, 0, monitor->m_transformedSize.x, monitor->m_transformedSize.y};
    if (bar.width <= 0 || bar.height <= 0 || !bar.overlaps(screen))
        return;

    // This compositor-owned editor has no client wl_surface. Fcitx's
    // ClientSideInputPanel API supplies composition and candidates instead
    // of trying to attach a native Wayland popup to another application's surface.
    std::vector<std::pair<std::string, int>> rows;
    if (!m_inputPanel.preedit.empty()) {
        auto preedit = m_inputPanel.preedit;
        const auto cursor = m_inputPanel.cursor;
        if (cursor >= 0 && static_cast<size_t>(cursor) <= preedit.size() &&
            (static_cast<size_t>(cursor) == preedit.size() || (static_cast<unsigned char>(preedit[cursor]) & 0xC0) != 0x80))
            preedit.insert(cursor, "│");
        rows.emplace_back(std::move(preedit), -1);
    }
    if (!m_inputPanel.auxiliary.empty())
        rows.emplace_back(m_inputPanel.auxiliary, -1);
    for (size_t i = 0; i < m_inputPanel.candidates.size(); ++i) {
        const auto& candidate = m_inputPanel.candidates[i];
        rows.emplace_back(candidate.label + "  " + candidate.text, i);
    }
    if (m_inputPanel.hasPrevious || m_inputPanel.hasNext)
        rows.emplace_back(std::string(m_inputPanel.hasPrevious ? "‹  " : "") + (m_inputPanel.hasNext ? "›" : ""), -1);
    if (rows.empty())
        return;

    const double scale = monitor->m_scale;
    const double padding = 10 * scale;
    const int logicalFont = std::clamp(ScrollOverview::Config::getValue<int>("plugin:scrolloverview:titles:font_size", monitor), 6, 64);
    const double rowHeight = (logicalFont + 12) * scale;
    const size_t maxRows = static_cast<size_t>(std::max(0.0, std::floor((screen.height - 2 * padding) / rowHeight)));
    rows.resize(std::min(rows.size(), maxRows));
    if (rows.empty())
        return;
    const double width = std::min(560 * scale, screen.width);
    const double height = rows.size() * rowHeight + 2 * padding;
    const double x = std::clamp(bar.x + (bar.width - width) / 2, 0.0, screen.width - width);
    double y = bar.y + bar.height + 4 * scale;
    if (y + height > screen.height)
        y = bar.y - height - 4 * scale;
    y = std::clamp(y, 0.0, screen.height - height);
    m_panelBox = CBox{x, y, width, height};
    const float alpha = m_overview.workspaceOverviewAlpha(index);
    auto& pass = Render::IHyprRenderer::currentPass(g_pHyprRenderer->context());
    pass.add(makeUnique<CRectPassElement>(CRectPassElement::SRectData{
        .box = m_panelBox, .color = CHyprColor{0.10F, 0.13F, 0.18F, 0.98F * alpha}}));
    const auto font = ScrollOverview::Config::getValue<std::string>("plugin:scrolloverview:titles:font", monitor);
    auto* fontDescription = pango_font_description_from_string(font.c_str());
    const auto fontWeight = pango_font_description_get_weight(fontDescription);
    pango_font_description_free(fontDescription);
    const double dpi = pango_cairo_font_map_get_resolution(PANGO_CAIRO_FONT_MAP(pango_cairo_font_map_get_default()));
    const int fontSize = std::max(1, static_cast<int>(logicalFont * scale * 72.0 / (dpi > 0 ? dpi : 96.0)));
    const double textWidth = width - 2 * padding;
    if (textWidth <= 0)
        return;
    m_panelTextures.resize(rows.size());
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& [text, candidate] = rows[i];
        const CBox row{x + padding, y + padding + i * rowHeight, textWidth, rowHeight};
        if (candidate >= 0) {
            m_candidateBoxes.emplace_back(row, candidate);
            if (candidate == m_inputPanel.selected)
                pass.add(makeUnique<CRectPassElement>(CRectPassElement::SRectData{
                    .box = row, .color = CHyprColor{0.18F, 0.37F, 0.55F, alpha}}));
        }
        auto& cached = m_panelTextures[i];
        const auto signature = std::format("{}:{}:{}:{}:{}", textWidth, fontSize, font.size(), font, text);
        if (!cached.texture || cached.signature != signature) {
            Hyprgraphics::CTextResource::STextResourceData data;
            data.text = text;
            data.font = font;
            data.fontSize = fontSize;
            data.weight = fontWeight;
            data.maxSize = Vector2D{textWidth, rowHeight};
            data.wrap = false;
            data.ellipsize = true;
            cached.texture = g_pHyprRenderer->renderText(std::move(data));
            cached.signature = signature;
        }
        if (cached.texture) {
            const auto size = cached.texture->m_size;
            pass.add(makeUnique<CTexPassElement>(CTexPassElement::SRenderData{
                .tex = cached.texture,
                .box = CBox{row.x, row.y + (row.height - size.y) / 2, size.x, size.y},
                .a = alpha, .clipBox = row}));
        }
    }
}
