#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>

namespace WorkspaceTitles {

    // At most 4096 bytes of strict UTF-8. Empty is valid. Rejects ASCII controls,
    // DEL, and Unicode line separators U+0085, U+2028, U+2029.
    bool validTitle(std::string_view title);

    class Editor {
      public:
        std::string text;
        std::size_t cursor = 0;

        // A valid reset positions the cursor at the end; invalid input clears it.
        void reset(std::string value);
        bool insert(std::string_view value);
        void left();
        void right();
        void home();
        void end();
        void backspace();
        void deleteForward();

        // Operations clamp a public cursor to the preceding codepoint boundary.
        // If public text was made invalid, insert fails and the other operations
        // leave text unchanged, with cursor zero. reset repairs such text.
    };

    class Store {
      public:
        explicit Store(std::filesystem::path path);

        // $XDG_STATE_HOME/hyprland-scroll-overview/workspace-titles.v1, or
        // $HOME/.local/state/hyprland-scroll-overview/workspace-titles.v1.
        // Relative environment paths are ignored. No usable root yields an
        // empty path; load/set then fail rather than using the working directory.
        static std::filesystem::path defaultPath();

        // Missing files load an empty store. Any failure preserves memory.
        bool        load(std::string& error);
        std::string get(const std::string& selector) const;

        // Empty title removes the exact selector. Selectors must be nonempty
        // and pass validTitle(). Reloads before each write, refusing corrupt
        // files and preserving other persisted selectors. Limits: 4096 bytes
        // per selector/title, 4096 entries, 8 MiB serialized file.
        // Not a concurrent-writer transaction: callers must serialize writers.
        // Pre-rename failures preserve memory and the existing file. If the
        // post-rename directory fsync fails, returns false but memory reflects
        // the replaced file; the error explicitly reports that replacement.
        bool set(const std::string& selector, const std::string& title, std::string& error);

      private:
        std::filesystem::path              m_path;
        std::map<std::string, std::string> m_titles;
    };

}
