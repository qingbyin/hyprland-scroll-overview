#pragma once

#include "globals.hpp"
#include "WindowSearch.hpp"
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/render/Texture.hpp>

#include <string_view>
#include <vector>

class CScrollOverview;

namespace OverviewSearch {
// Install before overview listeners; consume modal input even with a submap.
void initInput();
void shutdownInput();
bool active();
bool validArgument(std::string_view argument);
SDispatchResult dispatch(std::string argument);
bool eligible(const PHLWINDOW& window);
}

class COverviewSearch {
  public:
    explicit COverviewSearch(CScrollOverview& overview);
    ~COverviewSearch();
    SDispatchResult action(const std::string& argument);
    void cancel();
    bool active() const;
    void render();
    void changed();
    void key(const IKeyboard::SKeyEvent& event);
    void pointerButton(uint32_t button);
    void moveSelection(int delta);

  private:
    void refresh();
    void confirm();
    void repaint();
    void updateGeometry();
    void buildTexture();

    CScrollOverview& m_overview;
    WindowSearch::Editor m_editor;
    std::vector<WindowSearch::Candidate> m_candidates;
    std::vector<PHLWINDOWREF> m_windows;
    std::vector<size_t> m_matches;
    size_t m_selected = 0;
    size_t m_first = 0;
    size_t m_rows = 8;
    CBox m_box;
    double m_pixelScale = 1;
    bool m_dirty = true;
    std::string m_message;
    std::string m_textureKey;
    SP<Render::ITexture> m_texture;
};
