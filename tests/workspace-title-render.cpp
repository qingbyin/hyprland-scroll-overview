// Offline integration regression for the actual title render/titleBox methods.
// Run: bash scripts/test-workspace-title-render.sh [extra compiler flags]
// The script extracts unchanged production definitions plus the real flushPass.
// No copied render algorithm, Hyprland header tree, GL context or compositor.
// Fakes model submission, damage, rectangular coverage and painter's order only.
// Pango DPI is deterministic; shaping, glyph pixels, real layout geometry,
// input, persistence, renderer state and Hyprland occlusion are NOT tested here.
#include <algorithm>
#include <cmath>
#include <format>
#include <iostream>
#include <memory>
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
    int height = 30, fontSize = 14;
    std::string font = "sans-serif";
};
struct Monitor {
    double m_scale = 1;
    Vector2D m_transformedSize{800, 600};
    Settings settings;
};
using PHLMONITOR = SP<Monitor>;
namespace ScrollOverview::Config {
    template <typename T> T getValue(const std::string& key, PHLMONITOR monitor) {
        const auto& s = monitor->settings;
        if constexpr (std::is_same_v<T, bool>) {
            CHECK(key == "plugin:scrolloverview:titles:enabled");
            return s.enabled;
        } else if constexpr (std::is_same_v<T, int>) {
            if (key == "plugin:scrolloverview:titles:height")
                return s.height;
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
    std::weak_ptr<Monitor> pMonitor;
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
    struct Editor { std::string text; size_t cursor = 0; };
}
std::unique_ptr<WorkspaceTitles::Store> g_store;
// Editor display and Pango are external dependencies, not extracted methods.
std::string editorDisplay(const WorkspaceTitles::Editor& editor, int, int, const std::string&) {
    return editor.text.substr(0, editor.cursor) + "|" + editor.text.substr(editor.cursor);
}
double fakeDpi = 96;
void* pango_cairo_font_map_get_default() { return nullptr; }
void* PANGO_CAIRO_FONT_MAP(void* map) { return map; }
double pango_cairo_font_map_get_resolution(void*) { return fakeDpi; }

namespace Hyprgraphics {
    struct CTextResource {
        struct STextResourceData {
            std::string text, font;
            int fontSize = 0;
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
class CWorkspaceTitleUI {
  public:
    explicit CWorkspaceTitleUI(CScrollOverview& overview) : m_overview(overview) {}
    CBox titleBox(size_t index) const;
    void render();
    bool editing() const { return isEditing; }
    void cancel() { isEditing = false; ++cancelled; m_selector.clear(); m_editor = {}; m_saveFailed = false; }
    struct STexture { std::string signature; SP<Render::ITexture> texture; };
    CScrollOverview& m_overview;
    std::string m_selector;
    WorkspaceTitles::Editor m_editor;
    bool m_saveFailed = false, isEditing = false;
    int cancelled = 0;
    std::unordered_map<std::string, STexture> m_textures;
};

#include "workspace-title-render-methods.inc"

struct Fixture {
    PHLMONITOR monitor = std::make_shared<Monitor>();
    CScrollOverview overview;
    FakeRenderer renderer;
    CWorkspaceTitleUI ui{overview};
    Fixture() {
        trace = {};
        fakeDpi = 96;
        overview.pMonitor = monitor;
        g_store = std::make_unique<WorkspaceTitles::Store>();
        g_pHyprRenderer = &renderer;
        add("1", "1", {100, 100, 300, 200});
    }
    ~Fixture() { g_pHyprRenderer = nullptr; g_store.reset(); }
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
    CHECK(trace.raster[0].font == "sans-serif");
    CHECK(!trace.raster[0].wrap && trace.raster[0].ellipsize);
    CHECK(sameBox(f.ui.titleBox(0), {100, 100, 300, 30}));
    CHECK(topAt(107, 115) == "text:1");
    CHECK(topAt(107, 365) == "text:Development");
    CHECK(topAt(390, 110) == "rect");
    // Top/overlay surfaces are submitted after titles by the caller.
    f.renderer.ctx.pass.add(makeUnique<CRectPassElement>(CRectPassElement::SRectData{
        .box = {100, 100, 20, 30}, .color = {0, 1, 0, 1}}));
    OverviewRender::flushPass(f.renderer.ctx, f.monitor);
    CHECK(topAt(107, 115) == "rect");
    CHECK(topAt(107, 365) == "text:Development");
}
void guardsAndCancellation() {
    Fixture f;
    f.monitor->settings.enabled = false;
    f.ui.isEditing = true;
    f.ui.m_selector = "1";
    f.render();
    CHECK(f.ui.cancelled == 1 && !f.ui.editing());
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
    f.ui.isEditing = true;
    f.ui.m_selector = "removed";
    f.render();
    CHECK(f.ui.cancelled == 2 && !f.ui.editing());
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
    CHECK(trace.raster.size() == 2 && trace.raster.back().text == "Custom");
    g_store->values.erase("1");
    f.overview.images[0]->pWorkspace->name = "Renamed";
    f.render();
    CHECK(trace.raster.size() == 3 && trace.raster.back().text == "Renamed");
    f.monitor->settings.font = "Other Font";
    f.render();
    CHECK(trace.raster.size() == 4 && trace.raster.back().font == "Other Font");
    f.monitor->settings.fontSize = 18;
    f.render();
    CHECK(trace.raster.size() == 5);
    f.overview.previews[0].width -= 20;
    f.render();
    CHECK(trace.raster.size() == 6);
    f.overview.images.clear();
    f.render();
    CHECK(f.ui.m_textures.empty());
    f.add("1", "Recreated", {100, 100, 300, 200});
    f.render();
    CHECK(trace.raster.size() == 7 && trace.raster.back().text == "Recreated");
}
void editingAndAlpha() {
    Fixture f;
    f.ui.isEditing = true;
    f.ui.m_selector = "1";
    f.ui.m_editor = {"Draft", 2};
    f.overview.alphas[0] = 0.5F;
    f.render();
    CHECK(trace.raster.back().text == "Dr|aft");
    CHECK(trace.textures.back().a == 0.5F);
    CHECK(std::abs(trace.rectangles.back().color.a - 0.49F) < 0.0001F);
    f.ui.m_editor.cursor = 3;
    f.render();
    CHECK(trace.raster.size() == 2 && trace.raster.back().text == "Dra|ft");
    f.ui.m_saveFailed = true;
    f.render();
    CHECK(trace.raster.size() == 2);
    CHECK(trace.rectangles.back().color.r == 0.55F);
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
    CHECK(sameBox(trace.textures[0].clipBox, {-50, -10, 200, 30}));
    CHECK(sameBox(trace.painted[0].box, {0, 0, 150, 20}));
    CHECK(!trace.painted[1].box.empty());
    for (const auto& p : trace.painted)
        CHECK(sameBox(intersect(p.box, {0, 0, 150, 20}), p.box));
    const auto rasters = trace.raster.size();
    f.overview.previews[0].x = 900;
    f.render();
    CHECK(trace.raster.size() == rasters && trace.flushes == 1);
    CHECK(f.ui.m_textures.contains("1")); // Offscreen is not a deleted workspace.
}
void narrowAndSmallBars() {
    Fixture f;
    f.overview.previews[0] = {100, 100, 20, 100};
    f.render();
    CHECK((trace.submitted == std::vector<std::string>{"rect"}));
    CHECK(trace.raster.empty() && trace.flushes == 1);
    CHECK(topAt(105, 105) == "rect"); // Early text-skip must not skip the flush.
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
        CHECK(sameBox(bar, {100 * scale, 100 * scale, 300 * scale, 30 * scale}));
        const int padding = std::max(1, static_cast<int>(6 * scale));
        CHECK(trace.raster.back().maxSize.x == 300 * scale - 2 * padding);
        CHECK(trace.raster.back().fontSize == static_cast<int>(14 * scale * 0.75));
        CHECK(trace.textures.back().box.x == bar.x + padding);
        const auto size = trace.textures.back().tex->m_size;
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
void overlappingCardsAndRetry() {
    Fixture f;
    f.add("name:1", "Named one", {100, 100, 300, 200});
    g_store->values["1"] = "Numbered one";
    f.render();
    CHECK((trace.submitted == std::vector<std::string>{
        "rect", "text:Numbered one", "rect", "text:Named one"}));
    CHECK(topAt(107, 115) == "text:Named one");
    CHECK(f.ui.m_textures.size() == 2); // Numeric and named selectors stay distinct.
    f.ui.m_textures.clear();
    trace.failRaster = true;
    f.render();
    CHECK(!f.ui.m_textures.at("1").texture);
    CHECK(trace.flushes == 2); // Background survives a text rasterization failure.
    const auto rasters = trace.raster.size();
    f.render();
    CHECK(f.ui.m_textures.at("1").texture);
    CHECK(trace.raster.size() == rasters + 1);
}
int main() {
    const std::pair<const char*, void (*)()> tests[] = {
        {"default names, visibility and submission order", defaultsAndOrdering},
        {"enabled/closing/dependency guards and cancellation", guardsAndCancellation},
        {"texture reuse, invalidation and eviction", cacheAndInvalidation},
        {"editing, cursor cache, save failure and alpha", editingAndAlpha},
        {"clipped and offscreen title bars", clippingAndOffscreen},
        {"narrow bars still flush; short previews", narrowAndSmallBars},
        {"monitor scaling and configuration bounds", scalingAndBounds},
        {"overlapping cards, selector identity and raster retry", overlappingCardsAndRetry},
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
