#pragma once

#include "WorkspaceTitles.hpp"
#include "globals.hpp"

#include <unordered_map>

class CScrollOverview;
namespace Render { class ITexture; }

// Input guards outlive individual overview objects, so closing an editor does
// not leak the release of Enter/Escape (or another still-held key) to clients.
namespace WorkspaceTitleInput {
void init();
void shutdown();
}

class CWorkspaceTitleUI {
  public:
    explicit CWorkspaceTitleUI(CScrollOverview& overview);
    ~CWorkspaceTitleUI();
    void cancel();
    bool editing() const;
    bool mouseButton(uint32_t button, bool pressed);
    void key(uint32_t keycode);
    void render();

  private:
    struct STexture {
        std::string signature;
        SP<Render::ITexture> texture;
    };
    CBox titleBox(size_t index) const;
    void begin(size_t index);
    void save();

    CScrollOverview& m_overview;
    std::string m_selector;
    WorkspaceTitles::Editor m_editor;
    bool m_saveFailed = false;
    std::unordered_map<std::string, STexture> m_textures;
};
