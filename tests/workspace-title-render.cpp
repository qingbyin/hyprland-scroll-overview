// Offline integration regression for actual title/panel render and cancellation.
// Run: bash scripts/test-workspace-title-render.sh [extra compiler flags]
// The script extracts unchanged production definitions plus the real flushPass.
// No copied render algorithm, Hyprland header tree, GL context or compositor.
// Fakes model submission, damage, rectangular coverage and painter's order only.
// The real Pango parser checks font families/weights; DPI/layout are deterministic.
// Shaping, glyph pixels, real layout geometry,
// input, persistence, renderer state and Hyprland occlusion are NOT tested here.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <dlfcn.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include "../WorkspaceTitleIME.hpp"
#include <ranges>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#define CHECK(condition) do { \
    if (!(condition)) \
        throw std::runtime_error(std::format("line {}: {}", __LINE__, #condition)); \
} while (false)

template <typename T> using SP = std::shared_ptr<T>;
template <typename T, typename... Args>
auto makeUnique(Args&&... args) { return std::make_unique<T>(std::forward<Args>(args)...); }

struct Vector2D { double x = 0, y = 0; };
struct CBox {
    double x = 0, y = 0, width = 0, height = 0;
    CBox() = default;
    CBox(double x_, double y_, double w, double h) : x(x_), y(y_), width(w), height(h) {}
    CBox(Vector2D pos, Vector2D size) : CBox(pos.x, pos.y, size.x, size.y) {}
    bool empty() const { return width <= 0 || height <= 0; }
    bool overlaps(const CBox& b) const {
        return !empty() && !b.empty() && x < b.x + b.width && b.x < x + width &&
            y < b.y + b.height && b.y < y + height;
    }
    bool contains(double px, double py) const {
        return !empty() && px >= x && px < x + width && py >= y && py < y + height;
    }
};
CBox intersect(const CBox& a, const CBox& b) {
    if (!a.overlaps(b))
        return {};
    const double x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    return {x, y, std::min(a.x + a.width, b.x + b.width) - x,
            std::min(a.y + a.height, b.y + b.height) - y};
}
bool sameBox(const CBox& a, const CBox& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}
struct CRegion {
    CBox box;
    CRegion() = default;
    explicit CRegion(CBox b) : box(b) {}
    bool empty() const { return box.empty(); }
};
struct CHyprColor { float r = 0, g = 0, b = 0, a = 1; };

struct Settings {
    bool enabled = true;
    int height = 40, fontSize = 20, offset = 12;
    std::string font = "DejaVuSansM Nerd Font,FontAwesome 7 Free Semibold";
};
struct Monitor {
    double m_scale = 1;
    Vector2D m_transformedSize{800, 600};
    Settings settings;
};
using PHLMONITOR = SP<Monitor>;
struct MonitorRef : std::weak_ptr<Monitor> {
    using std::weak_ptr<Monitor>::operator=;
    explicit operator bool() const { return !expired(); }
};
namespace ScrollOverview::Config {
    template <typename T> T getValue(const std::string& key, PHLMONITOR monitor) {
        const auto& s = monitor->settings;
        if constexpr (std::is_same_v<T, bool>) {
            CHECK(key == "plugin:scrolloverview:titles:enabled");
            return s.enabled;
        } else if constexpr (std::is_same_v<T, int>) {
            if (key == "plugin:scrolloverview:titles:height")
                return s.height;
            if (key == "plugin:scrolloverview:titles:offset")
                return s.offset;
            CHECK(key == "plugin:scrolloverview:titles:font_size");
            return s.fontSize;
        } else {
            static_assert(std::is_same_v<T, std::string>);
            CHECK(key == "plugin:scrolloverview:titles:font");
            return s.font;
        }
    }
}
namespace Workspace {
    struct Item {
        std::string key, name;
        std::string displayName() const { return name; }
    };
    std::string selector(const Item& item) { return item.key; }
}
struct Image { SP<Workspace::Item> pWorkspace; };
struct CScrollOverview {
    MonitorRef pMonitor;
    int damages = 0;
    void damage() { ++damages; }
    bool closing = false;
    std::vector<SP<Image>> images;
    std::vector<CBox> previews;
    std::vector<float> alphas;
    CBox workspaceTitlePreviewBox(size_t i) const { return previews.at(i); }
    float workspaceOverviewAlpha(size_t i) const { return alphas.at(i); }
};
namespace WorkspaceTitles {
    struct Store {
        std::unordered_map<std::string, std::string> values;
        std::string get(const std::string& key) const {
            const auto it = values.find(key);
            return it == values.end() ? std::string{} : it->second;
        }
    };
    struct Editor {
        std::string text;
        size_t cursor = 0;
        void reset(std::string value) { text = std::move(value); cursor = text.size(); }
    };
}
std::unique_ptr<WorkspaceTitles::Store> g_store;
// Editor display and Pango are external dependencies, not extracted methods.
int editorWidth = 0, prefixMeasurements = 0;
std::string editorDisplay(const WorkspaceTitles::Editor& editor, int width, int, const std::string&) {
    editorWidth = width;
    return editor.text.substr(0, editor.cursor) + "|" + editor.text.substr(editor.cursor);
}
double fakeDpi = 96;
void* pango_cairo_font_map_get_default() { return nullptr; }
void* PANGO_CAIRO_FONT_MAP(void* map) { return map; }
double pango_cairo_font_map_get_resolution(void*) { return fakeDpi; }
// Deterministic proportional advances verify the render method reserves the
// measured prefix, not a byte-count estimate. These do not test Pango shaping.
constexpr int PANGO_SCALE = 1024;
struct FakePangoContext {};
// Load only the description API, keeping deterministic layout fakes and the
// standalone test command (no Hyprland headers or Pango development flags).
struct PangoParser {
    void* library = dlopen("libpango-1.0.so.0", RTLD_NOW | RTLD_LOCAL);
    template <typename T> T symbol(const char* name) {
        CHECK(library);
        const auto result = reinterpret_cast<T>(dlsym(library, name));
        CHECK(result);
        return result;
    }
    void* (*parse)(const char*) = symbol<void* (*)(const char*)>("pango_font_description_from_string");
    const char* (*family)(const void*) = symbol<const char* (*)(const void*)>("pango_font_description_get_family");
    int (*weight)(const void*) = symbol<int (*)(const void*)>("pango_font_description_get_weight");
    void (*free)(void*) = symbol<void (*)(void*)>("pango_font_description_free");
    ~PangoParser() { dlclose(library); }
};
struct FakeFont { std::string family; int size = 0, weight = 400; };
struct FakeLayout { std::string text; FakeFont font; };
FakePangoContext* pango_font_map_create_context(void*) { return new FakePangoContext; }
FakeLayout* pango_layout_new(FakePangoContext*) { return new FakeLayout; }
FakeFont* pango_font_description_from_string(const char* font) {
    static PangoParser pango;
    auto* description = pango.parse(font);
    CHECK(description);
    auto* result = new FakeFont{pango.family(description), 0, pango.weight(description)};
    pango.free(description);
    return result;
}
int pango_font_description_get_weight(FakeFont* font) { return font->weight; }
void pango_font_description_set_size(FakeFont* font, int size) { font->size = size; }
void pango_layout_set_font_description(FakeLayout* layout, FakeFont* font) { layout->font = *font; }
void pango_layout_set_text(FakeLayout* layout, const char* text, size_t size) { layout->text.assign(text, size); }
int measuredWidth(const std::string& text, int fontSize) {
    double width = 0;
    for (const unsigned char c : text) {
        if ((c & 0xC0) != 0x80)
            width += c == ' ' ? 0.25 : c == 'W' ? 1 : 0.5;
    }
    return static_cast<int>(std::ceil(width * fontSize * (fakeDpi > 0 ? fakeDpi : 96) / 72));
}
void pango_layout_get_pixel_size(FakeLayout* layout, int* width, int*) {
    ++prefixMeasurements;
    *width = measuredWidth(layout->text, layout->font.size / PANGO_SCALE);
}
void pango_font_description_free(FakeFont* font) { delete font; }
template <typename T> void g_object_unref(T* object) { delete object; }

namespace Hyprgraphics {
    struct CColor {
        struct SSRGB { double r = 0, g = 0, b = 0; };
        SSRGB rgb;
        explicit CColor(SSRGB value) : rgb(value) {}
        SSRGB asRgb() const { return rgb; }
    };
    struct CTextResource {
        struct STextResourceData {
            std::string text, font;
            int fontSize = 0, weight = 400;
            CColor color{CColor::SSRGB{1, 1, 1}};
            Vector2D maxSize;
            bool wrap = false, ellipsize = false;
        };
    };
}
namespace Render {
    struct ITexture { Vector2D m_size; std::string text; };
}
struct IPassElement {
    virtual ~IPassElement() = default;
    virtual CBox box() const = 0;
    virtual CBox clip() const { return {}; }
    virtual std::string label() const = 0;
    virtual float alpha() const = 0;
};
struct CRectPassElement : IPassElement {
    struct SRectData { CBox box; CHyprColor color; };
    SRectData m_data;
    explicit CRectPassElement(const SRectData& data) : m_data(data) {}
    CBox box() const override { return m_data.box; }
    std::string label() const override { return "rect"; }
    float alpha() const override { return m_data.color.a; }
};
struct CTexPassElement : IPassElement {
    struct SRenderData {
        SP<Render::ITexture> tex;
        CBox box;
        float a = 1;
        CBox clipBox;
    };
    SRenderData m_data;
    explicit CTexPassElement(const SRenderData& data) : m_data(data) {}
    CBox box() const override { return m_data.box; }
    CBox clip() const override { return m_data.clipBox; }
    std::string label() const override { return "text:" + m_data.tex->text; }
    float alpha() const override { return m_data.a; }
};
struct Paint { std::string label; CBox box; };
struct Trace {
    std::vector<std::string> events, submitted;
    std::vector<Paint> painted;
    std::vector<CRegion> damage;
    std::vector<Hyprgraphics::CTextResource::STextResourceData> raster;
    std::vector<CTexPassElement::SRenderData> textures;
    std::vector<CRectPassElement::SRectData> rectangles;
    int immediate = 0, emptyImmediate = 0, flushes = 0, clears = 0;
    bool failRaster = false, oversizedTexture = false;
} trace;
void paint(const IPassElement& element, const CRegion& damage) {
    // Empty damage must remain a no-op, matching the confirmed upstream bug.
    if (damage.empty() || element.alpha() <= 0)
        return;
    auto box = intersect(element.box(), damage.box);
    if (!element.clip().empty())
        box = intersect(box, element.clip());
    if (!box.empty())
        trace.painted.push_back({element.label(), box});
}
namespace Render {
    struct CRenderContext;
    struct CRenderPass {
        std::vector<std::unique_ptr<IPassElement>> elements;
        bool empty() const { return elements.empty(); }
        void add(std::unique_ptr<IPassElement> element) {
            trace.events.push_back("add:" + element->label());
            trace.submitted.push_back(element->label());
            if (const auto* tex = dynamic_cast<CTexPassElement*>(element.get()))
                trace.textures.push_back(tex->m_data);
            if (const auto* rect = dynamic_cast<CRectPassElement*>(element.get()))
                trace.rectangles.push_back(rect->m_data);
            elements.push_back(std::move(element));
        }
        void render(CRenderContext&, const CRegion& damage) {
            trace.events.push_back("flush");
            ++trace.flushes;
            trace.damage.push_back(damage);
            for (const auto& element : elements)
                paint(*element, damage);
        }
        void clear() { elements.clear(); ++trace.clears; trace.events.push_back("clear"); }
    };
    struct CRenderContext { CRenderPass pass; };
    struct IHyprRenderer {
        static CRenderPass& currentPass(CRenderContext& ctx) { return ctx.pass; }
    };
}
struct FakeRenderer {
    Render::CRenderContext ctx;
    Render::CRenderContext& context() { return ctx; }
    template <typename Data>
    void draw(Render::CRenderContext&, const Data& data, const CRegion& damage = {}) {
        ++trace.immediate;
        trace.emptyImmediate += damage.empty();
        trace.events.push_back("immediate");
        if constexpr (std::is_same_v<Data, CRectPassElement::SRectData>)
            paint(CRectPassElement{data}, damage);
        else
            paint(CTexPassElement{data}, damage);
    }
    SP<Render::ITexture> renderText(Hyprgraphics::CTextResource::STextResourceData&& data) {
        trace.raster.push_back(data);
        if (std::exchange(trace.failRaster, false))
            return {};
        const double dpi = fakeDpi > 0 ? fakeDpi : 96;
        const double pixels = data.fontSize * dpi / 72;
        const double extra = trace.oversizedTexture ? 100 : 0;
        return std::make_shared<Render::ITexture>(Render::ITexture{
            {std::min(data.maxSize.x, pixels * data.text.size()) + extra,
             std::min(data.maxSize.y, pixels) + extra}, data.text});
    }
};
FakeRenderer* g_pHyprRenderer = nullptr;
class CWorkspaceTitleUI;
CWorkspaceTitleUI* g_editor = nullptr;
struct FakeIME {
    int ends = 0;
    void end() { ++ends; }
};
struct FakeTimer {
    int stops = 0;
    void updateTimeout(std::nullopt_t) { ++stops; }
};
std::unique_ptr<FakeIME> g_ime;
std::unique_ptr<FakeTimer> g_imeTimer;
class CWorkspaceTitleUI {
  public:
    explicit CWorkspaceTitleUI(CScrollOverview& overview) : m_overview(overview) {}
    CBox titleBox(size_t index) const;
    void render();
    void renderInputPanel();
    bool editing() const;
    void cancel();
    struct STexture { std::string signature; SP<Render::ITexture> texture; };
    CScrollOverview& m_overview;
    std::string m_selector;
    WorkspaceTitles::Editor m_editor;
    bool m_saveFailed = false;
    std::unordered_map<std::string, STexture> m_textures;
    WorkspaceTitleIME::Panel m_inputPanel;
    std::vector<STexture> m_panelTextures;
    CBox m_panelBox;
    std::vector<std::pair<CBox, int>> m_candidateBoxes;
};

#include "workspace-title-render-methods.inc"

struct Fixture {
    PHLMONITOR monitor = std::make_shared<Monitor>();
    CScrollOverview overview;
    FakeRenderer renderer;
    CWorkspaceTitleUI ui{overview};
    Fixture() {
        trace = {};
        g_editor = nullptr;
        g_ime = std::make_unique<FakeIME>();
        g_imeTimer = std::make_unique<FakeTimer>();
        fakeDpi = 96;
        editorWidth = prefixMeasurements = 0;
        // Existing rendering cases exercise the configurable legacy position.
        monitor->settings.offset = 0;
        overview.pMonitor = monitor;
        g_store = std::make_unique<WorkspaceTitles::Store>();
        g_pHyprRenderer = &renderer;
        add("1", "1", {100, 100, 300, 200});
    }
    ~Fixture() { g_editor = nullptr; g_ime.reset(); g_imeTimer.reset(); g_pHyprRenderer = nullptr; g_store.reset(); }
    void add(std::string key, std::string name, CBox box, float alpha = 1) {
        overview.images.push_back(std::make_shared<Image>(Image{
            std::make_shared<Workspace::Item>(Workspace::Item{std::move(key), std::move(name)})}));
        overview.previews.push_back(box);
        overview.alphas.push_back(alpha);
    }
    void render() {
        ui.render();
        CHECK(trace.emptyImmediate == 0);
        CHECK(trace.immediate == 0);
        CHECK(renderer.ctx.pass.empty());
        for (const auto& damage : trace.damage)
            CHECK(!damage.empty());
    }
};
std::string topAt(double x, double y) {
    for (auto it = trace.painted.rbegin(); it != trace.painted.rend(); ++it)
        if (it->box.contains(x, y))
            return it->label;
    return {};
}
bool sameColor(const CHyprColor& color, int r, int g, int b, float alpha) {
    return std::abs(color.r - r / 255.F) < 0.00001F &&
        std::abs(color.g - g / 255.F) < 0.00001F &&
        std::abs(color.b - b / 255.F) < 0.00001F &&
        std::abs(color.a - alpha) < 0.00001F;
}
void configuredThemeDefaults() {
    // Check the actual registered defaults, not just the fixture's settings.
    std::ifstream file(std::filesystem::path(__FILE__).parent_path() / "../Config.cpp");
    CHECK(file.good());
    const std::string source{std::istreambuf_iterator<char>(file), {}};
    const auto registration = [&](const std::string& key) {
        const auto quotedKey = '"' + key + '"';
        const auto begin = source.find(quotedKey);
        CHECK(begin != std::string::npos);
        CHECK(source.find(quotedKey, begin + quotedKey.size()) == std::string::npos);
        const auto end = source.find(';', begin);
        CHECK(end != std::string::npos);
        return source.substr(begin, end - begin);
    };
    const auto font = registration("plugin:scrolloverview:titles:font");
    CHECK(font.contains("Hyprlang::STRING{\"" + Settings{}.font + "\"}"));
    auto* description = pango_font_description_from_string(Settings{}.font.c_str());
    CHECK(description->family == "DejaVuSansM Nerd Font,FontAwesome 7 Free");
    CHECK(pango_font_description_get_weight(description) == 600);
    pango_font_description_free(description);
    auto size = registration("plugin:scrolloverview:titles:font_size");
    std::erase_if(size, [](unsigned char c) { return std::isspace(c); });
    CHECK(size.contains("\"," + std::to_string(Settings{}.fontSize) + ",SIntValueOptions{"));
}
void defaultsAndOrdering() {
    Fixture f;
    f.add("name:dev", "Development", {100, 350, 300, 200});
    CHECK(g_store->values.empty());
    // A queued preview covering the title must be executed BEFORE the title.
    f.renderer.ctx.pass.add(makeUnique<CRectPassElement>(CRectPassElement::SRectData{
        .box = {0, 0, 800, 600}, .color = {1, 0, 0, 1}}));
    f.render();
    CHECK((trace.submitted == std::vector<std::string>{
        "rect", "rect", "text:1", "rect", "text:Development"}));
    CHECK(trace.flushes == 1 && trace.clears == 1);
    CHECK(sameBox(trace.damage.front().box, {0, 0, 800, 600}));
    CHECK(trace.events.at(trace.events.size() - 2) == "flush");
    CHECK(trace.events.back() == "clear");
    CHECK(trace.raster.size() == 2);
    CHECK(trace.raster[0].text == "1" && trace.raster[1].text == "Development");
    CHECK(trace.raster[0].font == Settings{}.font);
    CHECK(trace.raster[0].weight == 600);
    const auto rgb = trace.raster[0].color.asRgb();
    CHECK(rgb.r == 167.0 / 255.0 && rgb.g == 175.0 / 255.0 && rgb.b == 213.0 / 255.0);
    CHECK(!trace.raster[0].wrap && trace.raster[0].ellipsize);
    CHECK(sameBox(f.ui.titleBox(0), {100, 100, 300, 40}));
    CHECK(trace.rectangles.size() == 3); // Preview, then one fill per title.
    for (size_t i = 1; i < trace.rectangles.size(); ++i) {
        CHECK(sameBox(trace.rectangles[i].box, f.ui.titleBox(i - 1)));
        CHECK(sameColor(trace.rectangles[i].color, 30, 30, 46, 0.8F));
    }
    CHECK(trace.raster[0].fontSize == 15); // 20 logical px converted to integer points.
    CHECK(topAt(250, 120) == "text:1");
    CHECK(topAt(250, 370) == "text:Development");
    CHECK(topAt(390, 110) == "rect");
    // Top/overlay surfaces are submitted after titles by the caller.
    f.renderer.ctx.pass.add(makeUnique<CRectPassElement>(CRectPassElement::SRectData{
        .box = {240, 100, 20, 40}, .color = {0, 1, 0, 1}}));
    OverviewRender::flushPass(f.renderer.ctx, f.monitor);
    CHECK(topAt(250, 120) == "rect");
    CHECK(topAt(250, 370) == "text:Development");
}
void guardsAndCancellation() {
    Fixture f;
    f.monitor->settings.enabled = false;
    g_editor = &f.ui;
    f.ui.m_selector = "1";
    f.render();
    CHECK(f.overview.damages == 1 && !f.ui.editing());
    CHECK(f.ui.titleBox(0).empty());
    CHECK(trace.submitted.empty() && trace.raster.empty() && trace.flushes == 0);
    f.monitor->settings.enabled = true;
    f.overview.closing = true;
    f.render();
    CHECK(trace.submitted.empty());
    f.overview.closing = false;
    f.overview.pMonitor.reset();
    f.render();
    CHECK(f.ui.titleBox(0).empty() && trace.submitted.empty());
    f.overview.pMonitor = f.monitor;
    g_store.reset();
    f.render();
    CHECK(trace.submitted.empty());
    g_store = std::make_unique<WorkspaceTitles::Store>();
    g_editor = &f.ui;
    f.ui.m_selector = "removed";
    f.render();
    CHECK(f.overview.damages == 2 && !f.ui.editing());
    CHECK(trace.raster.front().text == "1");
}
void cacheAndInvalidation() {
    Fixture f;
    f.render();
    const auto first = f.ui.m_textures.at("1").texture;
    f.render();
    CHECK(trace.raster.size() == 1 && f.ui.m_textures.at("1").texture == first);
    CHECK(trace.flushes == 2 && trace.submitted.size() == 4);
    f.overview.previews[0].x += 10;
    f.render();
    CHECK(trace.raster.size() == 1); // Translation alone does not rerasterize.
    g_store->values["1"] = "Custom";
    f.render();
    CHECK(trace.raster.size() == 2 && trace.raster.back().text == "1    Custom");
    g_store->values.erase("1");
    f.overview.images[0]->pWorkspace->name = "Renamed";
    f.render();
    CHECK(trace.raster.size() == 3 && trace.raster.back().text == "Renamed");
    f.monitor->settings.font = "Other Font Bold";
    f.render();
    CHECK(trace.raster.size() == 4 && trace.raster.back().font == "Other Font Bold");
    f.monitor->settings.fontSize = 18;
    f.render();
    CHECK(trace.raster.size() == 5 && trace.raster.back().fontSize == 13);
    CHECK(trace.raster.back().weight == 700);
    f.overview.previews[0].width -= 20;
    f.render();
    CHECK(trace.raster.size() == 6);
    f.overview.images.clear();
    f.render();
    CHECK(f.ui.m_textures.empty());
    f.add("1", "Recreated", {100, 100, 300, 200});
    f.render();
    CHECK(trace.raster.size() == 7 && trace.raster.back().text == "Recreated");
    f.monitor->settings.font = "Other Font";
    f.render();
    CHECK(trace.raster.size() == 8 && trace.raster.back().font == "Other Font");
    CHECK(trace.raster.back().weight == 400); // No forced Semibold on user fonts.
}
void editingAndAlpha() {
    Fixture f;
    g_editor = &f.ui;
    f.ui.m_selector = "1";
    f.ui.m_editor = {"Draft", 2};
    f.overview.alphas[0] = 0.5F;
    f.render();
    CHECK(trace.raster.back().text == "1    Dr|aft");
    CHECK(trace.textures.back().a == 0.5F);
    CHECK(trace.rectangles.size() == 2);
    CHECK(sameColor(trace.rectangles.front().color, 30, 30, 46, 0.4F));
    CHECK(sameBox(trace.rectangles.front().box, f.ui.titleBox(0)));
    CHECK(sameColor(trace.rectangles.back().color, 22, 160, 133, 0.5F));
    CHECK(trace.raster.back().weight == 600);
    const auto rgb = trace.raster.back().color.asRgb();
    CHECK(rgb.r == 167.0 / 255.0 && rgb.g == 175.0 / 255.0 && rgb.b == 213.0 / 255.0);
    CHECK(sameBox(trace.rectangles.back().box, {100, 139, 300, 1}));
    const auto size = trace.textures.back().tex->m_size;
    CHECK(trace.textures.back().box.x == 100 + (300 - size.x) / 2);
    CHECK(topAt(101, 101) == "rect");
    f.ui.m_editor.cursor = 3;
    f.render();
    CHECK(trace.raster.size() == 2 && trace.raster.back().text == "1    Dra|ft");
    f.ui.m_saveFailed = true;
    f.render();
    CHECK(trace.raster.size() == 2);
    CHECK(sameColor(trace.rectangles.back().color, 247, 118, 142, 0.5F));
    CHECK(sameBox(trace.rectangles.back().box, {100, 139, 300, 1}));
    f.overview.alphas[0] = 0;
    const auto painted = trace.painted.size();
    f.render();
    CHECK(trace.painted.size() == painted);
    CHECK(trace.raster.size() == 2);
}
void clippingAndOffscreen() {
    Fixture f;
    f.overview.previews[0] = {-50, -10, 200, 100};
    f.add("2", "offscreen right", {800, 100, 200, 100});
    f.add("3", "title above screen", {100, -40, 200, 200});
    f.add("4", "zero width", {100, 100, 0, 100});
    f.add("5", "negative height", {100, 100, 200, -1});
    // Deliberately oversized raster output: clipBox must still constrain paint.
    trace.oversizedTexture = true;
    f.render();
    CHECK((trace.submitted == std::vector<std::string>{"rect", "text:1"}));
    CHECK(sameBox(trace.textures[0].clipBox, {-50, -10, 200, 40}));
    CHECK(trace.rectangles.size() == 1);
    CHECK(!trace.painted[0].box.empty());
    for (const auto& p : trace.painted)
        CHECK(sameBox(intersect(p.box, {0, 0, 150, 30}), p.box));
    const auto rasters = trace.raster.size();
    f.overview.previews[0].x = 900;
    f.render();
    CHECK(trace.raster.size() == rasters && trace.flushes == 1);
    CHECK(f.ui.m_textures.contains("1")); // Offscreen is not a deleted workspace.
}
void narrowAndSmallBars() {
    Fixture f;
    f.overview.previews[0] = {100, 100, 20, 100};
    g_editor = &f.ui;
    f.ui.m_selector = "1";
    f.render();
    CHECK((trace.submitted == std::vector<std::string>{"rect", "rect"}));
    CHECK(trace.raster.empty() && trace.flushes == 1);
    CHECK(sameBox(trace.rectangles.back().box, {100, 139, 20, 1}));
    CHECK(topAt(105, 105) == "rect"); // Background survives a skipped text raster.
    CHECK(topAt(105, 139) == "rect"); // Early text-skip still flushes the underline.
    f.overview.previews[0] = {100, 100, 200, 8};
    f.render();
    CHECK(f.ui.titleBox(0).height == 8);
    CHECK(trace.raster.back().fontSize == 3);
    CHECK(trace.raster.back().maxSize.y == 8);
}
void scalingAndBounds() {
    for (const double scale : {1.0, 1.25, 2.0}) {
        Fixture f;
        f.monitor->m_scale = scale;
        f.monitor->m_transformedSize = {800 * scale, 600 * scale};
        f.overview.previews[0] = {100 * scale, 100 * scale, 300 * scale, 200 * scale};
        f.render();
        const auto bar = f.ui.titleBox(0);
        CHECK(sameBox(bar, {100 * scale, 100 * scale, 300 * scale, 40 * scale}));
        const int padding = std::max(1, static_cast<int>(6 * scale));
        CHECK(trace.raster.back().maxSize.x == 300 * scale - 2 * padding);
        CHECK(trace.raster.back().fontSize == static_cast<int>(20 * scale * 0.75));
        const auto size = trace.textures.back().tex->m_size;
        CHECK(trace.textures.back().box.x == bar.x + (bar.width - size.x) / 2);
        CHECK(trace.rectangles.size() == 1);
        CHECK(trace.textures.back().box.y == bar.y + (bar.height - size.y) / 2);
        CHECK(sameBox(trace.damage.back().box, {0, 0, 800 * scale, 600 * scale}));
    }
    Fixture f;
    f.monitor->settings.height = 0;
    f.monitor->settings.fontSize = 0;
    f.render();
    CHECK(f.ui.titleBox(0).height == 16 && trace.raster.back().fontSize == 4);
    f.monitor->settings.height = 999;
    f.monitor->settings.fontSize = 999;
    f.render();
    CHECK(f.ui.titleBox(0).height == 128 && trace.raster.back().fontSize == 48);
    fakeDpi = -1;
    f.overview.previews[0].width += 1; // Force rasterization for the fallback DPI.
    f.render();
    CHECK(trace.raster.back().fontSize == 48);
}
void titleOffsetAndHitbox() {
    for (const double scale : {1.0, 1.25, 2.0}) {
        Fixture f;
        f.monitor->m_scale = scale;
        f.overview.previews[0] = {100 * scale, 100 * scale, 300 * scale, 200 * scale};
        for (const int offset : {Settings{}.offset, 0, 24, -1, 999}) {
            f.monitor->settings.offset = offset;
            const double shift = std::clamp(offset, 0, 128) * scale;
            const auto bar = f.ui.titleBox(0);
            CHECK(sameBox(bar, {100 * scale, 100 * scale - shift, 300 * scale, 40 * scale}));
            CHECK(bar.contains(bar.x + bar.width / 2, bar.y + bar.height / 2));
            CHECK(!bar.contains(bar.x + bar.width / 2, bar.y + bar.height + scale));
            CHECK(sameBox(f.overview.previews[0],
                {100 * scale, 100 * scale, 300 * scale, 200 * scale}));
        }
        f.monitor->settings.offset = Settings{}.offset;
        g_editor = &f.ui;
        f.ui.m_selector = "1";
        f.ui.m_inputPanel.preedit = "拼音";
        f.render();
        const auto bar = f.ui.titleBox(0);
        CHECK(sameBox(trace.rectangles.front().box, bar));
        CHECK(sameBox(trace.rectangles[1].box,
            {bar.x, bar.y + bar.height - scale, bar.width, scale}));
        CHECK(sameBox(trace.textures.front().clipBox, bar));
        CHECK(f.ui.m_panelBox.y == bar.y + bar.height + 4 * scale);
        const auto rasters = trace.raster.size();
        f.monitor->settings.offset = 24;
        f.render();
        CHECK(trace.raster.size() == rasters); // Position changes reuse textures.
        CHECK(f.ui.m_panelBox.y == bar.y - 12 * scale + bar.height + 4 * scale);
        f.overview.previews[0].y = -40 * scale;
        trace = {};
        f.render();
        CHECK(trace.submitted.empty()); // Offset title fully above the monitor.
    }
}
void overlappingCardsAndRetry() {
    Fixture f;
    f.add("name:1", "Named one", {100, 100, 300, 200});
    g_store->values["1"] = "Numbered one";
    f.render();
    CHECK((trace.submitted == std::vector<std::string>{
        "rect", "text:1    Numbered one", "rect", "text:Named one"}));
    CHECK(topAt(250, 120) == "text:Named one");
    CHECK(f.ui.m_textures.size() == 2); // Numeric and named selectors stay distinct.
    f.ui.m_textures.clear();
    trace.failRaster = true;
    f.render();
    CHECK(!f.ui.m_textures.at("1").texture);
    CHECK(trace.flushes == 2); // The second title still paints after the first raster fails.
    const auto rasters = trace.raster.size();
    f.render();
    CHECK(f.ui.m_textures.at("1").texture);
    CHECK(trace.raster.size() == rasters + 1);
}
void nativePrefixAndEditorBudget() {
    Fixture f;
    g_store->values["1"] = "Custom";
    f.render();
    CHECK(trace.raster.back().text == "1    Custom");
    CHECK(trace.rectangles.size() == 1 && prefixMeasurements == 0);
    const auto first = f.ui.m_textures.at("1").texture;
    const auto size = first->m_size;
    CHECK(trace.textures.back().box.x == 100 + (300 - size.x) / 2);
    f.overview.images[0]->pWorkspace->name = "W";
    f.render();
    CHECK(trace.raster.size() == 2 && trace.raster.back().text == "W    Custom");
    CHECK(f.ui.m_textures.at("1").texture != first);
    CHECK(prefixMeasurements == 0); // No prefix measurement outside editing.
    g_editor = &f.ui;
    f.ui.m_selector = "1";
    f.ui.m_editor = {std::string(4096, 'x'), 4096};
    f.render();
    CHECK(trace.raster.back().text.starts_with("W    "));
    CHECK(prefixMeasurements == 1);
    CHECK(editorWidth == 288 - measuredWidth("W    ", trace.raster.back().fontSize));
    CHECK(trace.raster.back().maxSize.x == 288); // Full texture remains bounded.
    const auto rasters = trace.raster.size();
    f.render();
    CHECK(trace.raster.size() == rasters && prefixMeasurements == 1);
    const int oldWidth = editorWidth;
    f.overview.images[0]->pWorkspace->name = "WW";
    f.render();
    CHECK(trace.raster.size() == rasters + 1 && prefixMeasurements == 2);
    CHECK(editorWidth < oldWidth);
    CHECK(editorWidth == 288 - measuredWidth("WW    ", trace.raster.back().fontSize));
    f.ui.m_editor = {"", 0};
    f.render();
    CHECK(trace.raster.back().text == "WW    |");
    g_editor = nullptr;
    g_store->values.erase("1");
    f.render();
    CHECK(trace.raster.back().text == "WW"); // No trailing spaces or duplicate name.
    for (const double scale : {1.25, 2.0}) {
        f.monitor->m_scale = scale;
        f.overview.previews[0] = {100, 100, 300 * scale, 200 * scale};
        g_editor = &f.ui;
        f.render();
        CHECK(editorWidth == trace.raster.back().maxSize.x -
            measuredWidth("WW    ", trace.raster.back().fontSize));
        const auto bar = f.ui.titleBox(0);
        CHECK(sameBox(trace.rectangles.back().box,
            {bar.x, bar.y + bar.height - scale, bar.width, scale}));
    }
}
void longNativePrefix() {
    std::string unicodeName;
    for (int i = 0; i < 1000; ++i)
        unicodeName += "工";
    for (const auto& nativeName : {std::string(4096, 'W'), unicodeName}) {
        for (const double scale : {1.0, 1.25, 2.0}) {
            for (const double dpi : {96.0, 144.0}) {
                Fixture f;
                fakeDpi = dpi;
                f.monitor->m_scale = scale;
                f.overview.previews[0] = {100, 100, 300 * scale, 200 * scale};
                f.overview.images[0]->pWorkspace->name = nativeName;
                g_editor = &f.ui;
                f.ui.m_selector = "1";
                f.render(); // Empty editor still has a visible insertion cursor.
                const auto& data = trace.raster.back();
                CHECK(data.text.ends_with("…    |"));
                const auto prefix = data.text.substr(0, data.text.size() - 1);
                const auto visibleName = prefix.substr(0, prefix.size() - std::string("…    ").size());
                CHECK(nativeName.starts_with(visibleName));
                if (nativeName == unicodeName)
                    CHECK(visibleName.size() % std::string("工").size() == 0);
                const int width = static_cast<int>(data.maxSize.x);
                const int prefixWidth = measuredWidth(prefix, data.fontSize);
                CHECK(prefixWidth <= static_cast<int>(width * 0.4));
                CHECK(editorWidth == width - prefixWidth);
                CHECK(editorWidth >= width * 0.6);
                CHECK(measuredWidth(data.text, data.fontSize) <= width);
                CHECK(prefixMeasurements < 20); // Binary search, not one measurement per byte.
                const auto measurements = prefixMeasurements;
                const auto rasterCount = trace.raster.size();
                f.render();
                CHECK(prefixMeasurements == measurements && trace.raster.size() == rasterCount);
                f.ui.m_editor = {std::string(4096, 'x'), 4096};
                f.render();
                CHECK(trace.raster.back().text.starts_with(prefix));
                CHECK(editorWidth == width - prefixWidth);
                CHECK(trace.raster.back().maxSize.x == width);
                f.overview.images[0]->pWorkspace->name += "W";
                f.render();
                CHECK(trace.raster.size() == rasterCount + 2); // Even an elided suffix invalidates.
            }
        }
    }
    Fixture f;
    f.overview.previews[0].width = 50;
    f.overview.images[0]->pWorkspace->name = std::string(100, 'W');
    g_editor = &f.ui;
    f.ui.m_selector = "1";
    f.render();
    CHECK(trace.raster.back().text == "    |"); // Too narrow even for an ellipsis.
    CHECK(editorWidth > 0);
    CHECK(measuredWidth(trace.raster.back().text, trace.raster.back().fontSize) <=
        trace.raster.back().maxSize.x);
}
void emptyPanelAndGuards() {
    Fixture f;
    f.ui.m_panelBox = {1, 2, 3, 4};
    f.ui.m_candidateBoxes.push_back({{1, 2, 3, 4}, 0});
    f.ui.m_inputPanel.preedit = "拼音";
    f.render(); // Inactive editors cannot show a stale composition panel.
    CHECK(f.ui.m_panelBox.empty() && f.ui.m_candidateBoxes.empty());
    CHECK(trace.rectangles.size() == 1);
    CHECK((trace.submitted == std::vector<std::string>{"rect", "text:1"}));
    trace = {};
    g_editor = &f.ui;
    f.ui.m_selector = "1";
    f.ui.m_inputPanel = {};
    f.render();
    CHECK(f.ui.m_panelBox.empty() && f.ui.m_candidateBoxes.empty());
    CHECK(f.ui.m_panelTextures.empty());
    CHECK(trace.rectangles.size() == 2);
    CHECK(sameBox(trace.rectangles[1].box, {100, 139, 300, 1}));
    CHECK((trace.submitted == std::vector<std::string>{"rect", "rect", "text:1    |"}));
    f.ui.m_inputPanel.preedit = "拼音";
    f.render();
    CHECK(!f.ui.m_panelBox.empty());
    trace = {};
    f.overview.previews[0].x = 900;
    f.render();
    CHECK(f.ui.m_panelBox.empty() && f.ui.m_candidateBoxes.empty());
    CHECK(trace.submitted.empty());
    f.overview.previews[0].x = 100;
    f.ui.m_selector = "missing";
    f.ui.renderInputPanel(); // The helper itself rejects a missing workspace.
    CHECK(f.ui.m_panelBox.empty() && trace.submitted.empty());
    f.ui.m_selector = "1";
    f.overview.pMonitor.reset();
    f.ui.m_panelBox = {1, 2, 3, 4};
    f.ui.renderInputPanel();
    CHECK(f.ui.m_panelBox.empty() && trace.submitted.empty());
}
void panelCompositionCandidatesAndOrder() {
    Fixture f;
    g_editor = &f.ui;
    f.ui.m_selector = "1";
    f.ui.m_inputPanel = {
        .preedit = "拼音", .auxiliary = "中文输入", .cursor = 3, .selected = 1,
        .candidates = {{"1", "拼音"}, {"2", "中文"}},
        .hasPrevious = true, .hasNext = true,
    };
    f.render();
    CHECK((trace.submitted == std::vector<std::string>{
        "rect", "rect", "text:1    |", "rect", "text:拼│音", "text:中文输入",
        "text:1  拼音", "rect", "text:2  中文", "text:‹  ›"}));
    CHECK(trace.flushes == 1 && trace.clears == 1);
    CHECK(trace.events[trace.events.size() - 2] == "flush");
    CHECK(trace.events.back() == "clear");
    CHECK(sameBox(f.ui.m_panelBox, {0, 144, 560, 180}));
    CHECK(trace.rectangles.size() == 4); // Title, underline, popup, selected candidate.
    CHECK(sameBox(trace.rectangles[2].box, f.ui.m_panelBox));
    CHECK(trace.rectangles[2].color.a == 0.98F);
    CHECK(f.ui.m_candidateBoxes.size() == 2);
    for (size_t i = 0; i < f.ui.m_candidateBoxes.size(); ++i) {
        const auto& [box, candidate] = f.ui.m_candidateBoxes[i];
        CHECK(candidate == static_cast<int>(i));
        CHECK(sameBox(box, {10, 218 + 32.0 * i, 540, 32}));
        CHECK(sameBox(box, trace.textures[3 + i].clipBox));
        CHECK(box.contains(box.x + box.width / 2, box.y + box.height / 2));
        CHECK(!box.contains(box.x + box.width, box.y + box.height / 2));
        CHECK(!box.contains(5, box.y + 1)); // Panel padding is not a candidate.
        CHECK(!box.contains(20, 160)); // Composition row is not a candidate.
        CHECK(!box.contains(20, 200)); // Auxiliary row is not a candidate.
        CHECK(!box.contains(20, 300)); // Paging indicators are not candidates.
    }
    CHECK(sameBox(trace.rectangles[3].box, f.ui.m_candidateBoxes[1].first));
    CHECK(trace.rectangles[3].color.r == 0.18F && trace.rectangles[3].color.b == 0.55F);
    CHECK(topAt(20, 270) == "text:2  中文");
    CHECK(topAt(101, 101) == "rect"); // Title background remains independent of the popup.
    for (size_t i = 1; i < trace.raster.size(); ++i) {
        CHECK(trace.raster[i].maxSize.x == 540 && trace.raster[i].maxSize.y == 32);
        CHECK(!trace.raster[i].wrap && trace.raster[i].ellipsize);
        CHECK(trace.raster[i].font == Settings{}.font && trace.raster[i].fontSize == 15);
        CHECK(trace.raster[i].weight == 600);
    }
}
void panelCaretBoundaries() {
    Fixture f;
    g_editor = &f.ui;
    f.ui.m_selector = "1";
    f.ui.m_inputPanel.preedit = "拼音";
    const std::pair<int, const char*> cases[] = {
        {0, "│拼音"}, {3, "拼│音"}, {6, "拼音│"},
        {-1, "拼音"}, {1, "拼音"}, {4, "拼音"}, {7, "拼音"},
    };
    for (const auto& [cursor, expected] : cases) {
        trace = {};
        f.ui.m_inputPanel.cursor = cursor;
        f.render();
        CHECK(trace.textures.size() == 2);
        CHECK(trace.textures.back().tex->text == expected);
        CHECK(f.ui.m_candidateBoxes.empty());
        CHECK(f.ui.m_panelTextures.size() == 1);
    }
    f.ui.m_inputPanel = {};
    f.ui.m_inputPanel.hasNext = true;
    trace = {};
    f.render();
    CHECK(trace.textures.back().tex->text == "›");
    CHECK(f.ui.m_candidateBoxes.empty());
}
void panelScreenBoundsAndClipping() {
    for (const double scale : {1.0, 1.25, 2.0}) {
        Fixture f;
        g_editor = &f.ui;
        f.ui.m_selector = "1";
        f.monitor->m_scale = scale;
        // Noninteger scaled geometry, with a title crossing the right edge.
        f.monitor->m_transformedSize = {803, 607};
        f.overview.previews[0] = {750.25, 565.5, 200 * scale, 200 * scale};
        f.ui.m_inputPanel = {
            .preedit = "zhong", .auxiliary = "", .cursor = 5, .selected = 0,
            .candidates = {{"1", "中文"}, {"2", "中国"}},
        };
        f.render();
        const CBox screen{0, 0, 803, 607};
        const auto panel = f.ui.m_panelBox;
        const auto bar = f.ui.titleBox(0);
        CHECK(sameBox(intersect(panel, screen), panel));
        CHECK(panel.x + panel.width == screen.width);
        CHECK(panel.width == std::min(560 * scale, screen.width));
        CHECK(panel.y + panel.height <= bar.y - 4 * scale);
        CHECK(f.ui.m_candidateBoxes.size() == 2);
        for (const auto& [box, candidate] : f.ui.m_candidateBoxes) {
            CHECK(candidate >= 0 && candidate < 2);
            CHECK(sameBox(intersect(box, panel), box));
            CHECK(box.height == 32 * scale);
        }
        // Force raster overflow and prove actual submission clips every row.
        f.ui.m_panelTextures.clear();
        trace = {};
        trace.oversizedTexture = true;
        f.render();
        for (size_t i = 1; i < trace.textures.size(); ++i) {
            const auto& texture = trace.textures[i];
            CHECK(sameBox(intersect(texture.clipBox, panel), texture.clipBox));
            CHECK(texture.box.height > texture.clipBox.height);
            const auto painted = std::ranges::find_if(trace.painted, [&](const Paint& p) {
                return p.label == "text:" + texture.tex->text;
            });
            CHECK(painted != trace.painted.end());
            CHECK(sameBox(intersect(painted->box, texture.clipBox), painted->box));
        }
        trace = {};
        f.ui.m_inputPanel = {};
        for (int i = 0; i < 50; ++i)
            f.ui.m_inputPanel.candidates.push_back({std::to_string(i + 1), "候选"});
        f.render();
        CHECK(sameBox(intersect(f.ui.m_panelBox, screen), f.ui.m_panelBox));
        const auto maxRows = static_cast<size_t>(std::floor((607 - 20 * scale) / (32 * scale)));
        CHECK(f.ui.m_panelTextures.size() == maxRows);
        CHECK(f.ui.m_candidateBoxes.size() == maxRows);
        for (size_t i = 0; i < maxRows; ++i) {
            CHECK(f.ui.m_candidateBoxes[i].second == static_cast<int>(i));
            CHECK(sameBox(intersect(f.ui.m_candidateBoxes[i].first, screen), f.ui.m_candidateBoxes[i].first));
        }
        f.monitor->m_transformedSize = {120, 607};
        f.overview.previews[0] = {0, 0, 100, 100};
        f.render();
        CHECK(f.ui.m_panelBox.x == 0 && f.ui.m_panelBox.width == 120);
        f.monitor->m_transformedSize = {120, 20};
        trace = {};
        f.render();
        CHECK(f.ui.m_panelBox.empty() && f.ui.m_candidateBoxes.empty());
        CHECK(trace.rectangles.size() == 2); // Title and underline, no room for popup rows.
    }
}
void panelCacheAlphaAndRetry() {
    Fixture f;
    g_editor = &f.ui;
    f.ui.m_selector = "1";
    f.ui.m_inputPanel = {.preedit = "", .auxiliary = "", .selected = 0,
                        .candidates = {{"1", "中文"}, {"2", "中国"}}};
    f.overview.alphas[0] = 0.5F;
    f.render();
    CHECK(trace.raster.size() == 3);
    CHECK(sameColor(trace.rectangles[0].color, 30, 30, 46, 0.4F));
    CHECK(std::abs(trace.rectangles[2].color.a - 0.49F) < 0.0001F);
    CHECK(trace.rectangles[3].color.a == 0.5F);
    for (const auto& texture : trace.textures)
        CHECK(texture.a == 0.5F);
    const auto first = f.ui.m_panelTextures[0].texture;
    f.render();
    CHECK(trace.raster.size() == 3 && f.ui.m_panelTextures[0].texture == first);
    f.ui.m_inputPanel.selected = 1;
    f.overview.previews[0].y += 10;
    f.render();
    CHECK(trace.raster.size() == 3); // Selection and position do not rerasterize.
    CHECK(sameBox(trace.rectangles.back().box, f.ui.m_candidateBoxes[1].first));
    f.ui.m_inputPanel.candidates[1].text = "中国人";
    f.render();
    CHECK(trace.raster.size() == 4 && trace.raster.back().text == "2  中国人");
    CHECK(f.ui.m_panelTextures[0].texture == first);
    f.monitor->settings.font = "Other Font Bold";
    f.render();
    CHECK(trace.raster.size() == 7); // Title and both popup rows change font.
    for (size_t i = 4; i < 7; ++i) {
        CHECK(trace.raster[i].font == "Other Font Bold");
        CHECK(trace.raster[i].weight == 700);
    }
    f.monitor->settings.fontSize = 24;
    f.render();
    CHECK(trace.raster.size() == 10);
    f.monitor->m_transformedSize.x = 400;
    f.render();
    CHECK(trace.raster.size() == 12); // Panel width changes, title width does not.
    CHECK(trace.raster.back().maxSize.x == 380);
    f.ui.m_panelTextures[0].texture.reset();
    trace.failRaster = true;
    f.render();
    CHECK(!f.ui.m_panelTextures[0].texture);
    CHECK(trace.raster.size() == 13);
    CHECK(f.ui.m_candidateBoxes.size() == 2); // Hitboxes do not depend on raster success.
    f.render();
    CHECK(f.ui.m_panelTextures[0].texture && trace.raster.size() == 14);
    f.ui.m_inputPanel.candidates.resize(1);
    f.render();
    CHECK(f.ui.m_panelTextures.size() == 1 && f.ui.m_candidateBoxes.size() == 1);
    f.overview.alphas[0] = 0;
    trace = {};
    f.render();
    CHECK(trace.painted.empty() && trace.raster.empty());
    CHECK(!trace.rectangles.empty());
    for (const auto& rect : trace.rectangles)
        CHECK(rect.color.a == 0);
    for (const auto& texture : trace.textures)
        CHECK(texture.a == 0);
    f.ui.m_inputPanel = {};
    trace = {};
    f.render();
    CHECK(f.ui.m_panelBox.empty() && f.ui.m_candidateBoxes.empty());
    CHECK(trace.rectangles.size() == 2); // Title background and editor underline remain.
}
void panelCancellationClearsState() {
    Fixture f;
    g_editor = &f.ui;
    f.ui.m_selector = "1";
    f.ui.m_editor = {"draft", 2};
    f.ui.m_saveFailed = true;
    f.ui.m_inputPanel = {.preedit = "拼音", .auxiliary = "", .cursor = 3, .candidates = {{"1", "中文"}}};
    f.render();
    CHECK(!f.ui.m_panelBox.empty() && !f.ui.m_panelTextures.empty());
    CHECK(!f.ui.m_candidateBoxes.empty());
    f.ui.cancel(); // Extracted production method, not a fake state reset.
    CHECK(!f.ui.editing() && g_editor == nullptr);
    CHECK(f.ui.m_inputPanel.preedit.empty() && f.ui.m_inputPanel.candidates.empty());
    CHECK(f.ui.m_panelTextures.empty() && f.ui.m_candidateBoxes.empty() && f.ui.m_panelBox.empty());
    CHECK(f.ui.m_selector.empty() && f.ui.m_editor.text.empty() && !f.ui.m_saveFailed);
    CHECK(g_ime->ends == 1 && g_imeTimer->stops == 1 && f.overview.damages == 1);
    trace = {};
    f.render();
    CHECK(trace.rectangles.size() == 1);
    CHECK((trace.submitted == std::vector<std::string>{"rect", "text:1"}));
    f.ui.cancel();
    CHECK(g_ime->ends == 1 && g_imeTimer->stops == 1 && f.overview.damages == 1);
    g_editor = &f.ui;
    f.ui.m_selector = "1";
    f.ui.m_inputPanel.candidates = {{"1", "中文"}};
    f.render();
    CHECK(!f.ui.m_candidateBoxes.empty());
    f.monitor->settings.enabled = false;
    trace = {};
    f.render(); // render() invokes actual cancel() before returning early.
    CHECK(!f.ui.editing() && f.ui.m_panelTextures.empty());
    CHECK(f.ui.m_panelBox.empty() && f.ui.m_candidateBoxes.empty());
    CHECK(trace.submitted.empty());
    CHECK(g_ime->ends == 2 && g_imeTimer->stops == 2 && f.overview.damages == 2);
}
int main() {
    const std::pair<const char*, void (*)()> tests[] = {
        {"registered theme defaults and real Pango fallback/weight", configuredThemeDefaults},
        {"default names, visibility and submission order", defaultsAndOrdering},
        {"enabled/closing/dependency guards and cancellation", guardsAndCancellation},
        {"texture reuse, invalidation and eviction", cacheAndInvalidation},
        {"editing, cursor cache, save failure and alpha", editingAndAlpha},
        {"clipped and offscreen title bars", clippingAndOffscreen},
        {"narrow bars still flush; short previews", narrowAndSmallBars},
        {"monitor scaling and configuration bounds", scalingAndBounds},
        {"title offset, hitbox, popup and scale", titleOffsetAndHitbox},
        {"overlapping cards, selector identity and raster retry", overlappingCardsAndRetry},
        {"native prefix cache invalidation and measured editor budget", nativePrefixAndEditorBudget},
        {"long native names retain cursor space and UTF-8 boundaries", longNativePrefix},
        {"empty/inactive/offscreen panels and themed idle", emptyPanelAndGuards},
        {"popup composition, Chinese candidates, hitboxes and paint order", panelCompositionCandidatesAndOrder},
        {"popup caret respects UTF-8 byte boundaries", panelCaretBoundaries},
        {"popup screen bounds, fractional scale and row clipping", panelScreenBoundsAndClipping},
        {"popup texture cache, selection, alpha and raster retry", panelCacheAlphaAndRetry},
        {"actual cancellation clears popup and restores themed idle", panelCancellationClearsState},
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << std::size(tests) - failures << '/' << std::size(tests)
              << " workspace title render tests passed\n";
    return failures ? 1 : 0;
}
