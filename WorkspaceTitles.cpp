#include "WorkspaceTitles.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace WorkspaceTitles {
    namespace {
        constexpr std::size_t      MAX_TITLE    = 4096;
        constexpr std::size_t      MAX_SELECTOR = 4096;
        constexpr std::size_t      MAX_ENTRIES  = 4096;
        constexpr std::size_t      MAX_FILE     = 8 * 1024 * 1024;
        constexpr std::string_view MAGIC        = "workspace-titles.v1\n";
        using Titles                            = std::map<std::string, std::string>;

        // Returns zero for malformed, truncated, overlong, surrogate, or
        // out-of-range encodings. A successful decode advances by one scalar.
        std::size_t decode(std::string_view value, std::size_t at, std::uint32_t& scalar) {
            if (at >= value.size())
                return 0;
            const auto first = static_cast<unsigned char>(value[at]);
            if (first < 0x80) {
                scalar = first;
                return 1;
            }
            std::size_t   length;
            std::uint32_t minimum;
            if (first >= 0xc2 && first <= 0xdf) {
                length  = 2;
                minimum = 0x80;
                scalar  = first & 0x1f;
            } else if (first >= 0xe0 && first <= 0xef) {
                length  = 3;
                minimum = 0x800;
                scalar  = first & 0x0f;
            } else if (first >= 0xf0 && first <= 0xf4) {
                length  = 4;
                minimum = 0x10000;
                scalar  = first & 0x07;
            } else {
                return 0;
            }
            if (value.size() - at < length)
                return 0;
            for (std::size_t i = 1; i < length; ++i) {
                const auto byte = static_cast<unsigned char>(value[at + i]);
                if ((byte & 0xc0) != 0x80)
                    return 0;
                scalar = (scalar << 6) | (byte & 0x3f);
            }
            if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff))
                return 0;
            return length;
        }

        bool prepare(Editor& editor) {
            if (!validTitle(editor.text)) {
                editor.cursor = 0;
                return false;
            }
            editor.cursor = std::min(editor.cursor, editor.text.size());
            while (editor.cursor > 0 && editor.cursor < editor.text.size() && (static_cast<unsigned char>(editor.text[editor.cursor]) & 0xc0) == 0x80)
                --editor.cursor;
            return true;
        }

        std::size_t previous(std::string_view text, std::size_t cursor) {
            if (cursor == 0)
                return 0;
            --cursor;
            while (cursor > 0 && (static_cast<unsigned char>(text[cursor]) & 0xc0) == 0x80)
                --cursor;
            return cursor;
        }

        struct File {
            int fd = -1;
            explicit File(int descriptor) : fd(descriptor) {}
            ~File() {
                if (fd >= 0)
                    ::close(fd);
            }
            File(const File&)            = delete;
            File& operator=(const File&) = delete;
            int   close() {
                return ::close(std::exchange(fd, -1));
            }
        };

        struct TemporaryFile {
            std::string path;
            ~TemporaryFile() {
                if (!path.empty())
                    ::unlink(path.c_str());
            }
        };

        bool systemError(std::string& error, std::string_view operation, int code) {
            error = std::string(operation) + ": " + std::error_code(code, std::generic_category()).message();
            return false;
        }

        bool usablePath(const std::filesystem::path& path, std::string& error) {
            if (path.empty()) {
                error = "No workspace-title file path; set an absolute XDG_STATE_HOME or HOME";
                return false;
            }
            // POSIX APIs would silently truncate an embedded NUL.
            if (path.native().find('\0') != std::string::npos || path.filename().empty()) {
                error = "Invalid workspace-title file path";
                return false;
            }
            return true;
        }

        bool readBytes(const std::filesystem::path& path, std::string& bytes, bool& missing, std::string& error) {
            File file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
            if (file.fd < 0) {
                if (errno == ENOENT) {
                    missing = true;
                    return true;
                }
                return systemError(error, "Cannot open workspace-title file", errno);
            }
            struct stat info{};
            if (::fstat(file.fd, &info) < 0)
                return systemError(error, "Cannot inspect workspace-title file", errno);
            if (!S_ISREG(info.st_mode)) {
                error = "Workspace-title file is not a regular file";
                return false;
            }
            if (info.st_size < 0 || static_cast<std::uintmax_t>(info.st_size) > MAX_FILE) {
                error = "Workspace-title file exceeds the 8 MiB limit";
                return false;
            }
            std::array<char, 8192> buffer;
            for (;;) {
                // Also enforce the bound if another process grows the file.
                const auto count = ::read(file.fd, buffer.data(), std::min(buffer.size(), MAX_FILE - bytes.size() + 1));
                if (count < 0) {
                    if (errno == EINTR)
                        continue;
                    return systemError(error, "Cannot read workspace-title file", errno);
                }
                if (count == 0)
                    break;
                if (static_cast<std::size_t>(count) > MAX_FILE - bytes.size()) {
                    error = "Workspace-title file exceeds the 8 MiB limit";
                    return false;
                }
                bytes.append(buffer.data(), static_cast<std::size_t>(count));
            }
            return true;
        }

        void appendNumber(std::string& bytes, std::size_t number) {
            for (unsigned shift = 0; shift < 32; shift += 8)
                bytes.push_back(static_cast<char>((number >> shift) & 0xff));
        }

        bool takeNumber(std::string_view bytes, std::size_t& at, std::size_t& number) {
            if (bytes.size() - at < 4)
                return false;
            std::uint32_t result = 0;
            for (unsigned shift = 0; shift < 32; shift += 8)
                result |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at++])) << shift;
            number = result;
            return true;
        }

        bool parse(std::string_view bytes, Titles& titles, std::string& error) {
            const auto malformed = [&error]() {
                error = "Malformed or unsupported workspace-title file";
                return false;
            };
            if (!bytes.starts_with(MAGIC))
                return malformed();
            std::size_t at    = MAGIC.size();
            std::size_t count = 0;
            if (!takeNumber(bytes, at, count) || count > MAX_ENTRIES)
                return malformed();
            for (std::size_t i = 0; i < count; ++i) {
                std::size_t keySize   = 0;
                std::size_t titleSize = 0;
                if (!takeNumber(bytes, at, keySize) || !takeNumber(bytes, at, titleSize) || keySize == 0 || keySize > MAX_SELECTOR || titleSize == 0 || titleSize > MAX_TITLE ||
                    keySize > bytes.size() - at)
                    return malformed();
                std::string key(bytes.substr(at, keySize));
                at += keySize;
                if (titleSize > bytes.size() - at)
                    return malformed();
                const auto title = bytes.substr(at, titleSize);
                if (!validTitle(key) || !validTitle(title) || !titles.emplace(std::move(key), std::string(title)).second)
                    return malformed();
                at += titleSize;
            }
            return at == bytes.size() || malformed();
        }

        bool readTitles(const std::filesystem::path& path, Titles& titles, std::string& error) {
            if (!usablePath(path, error))
                return false;
            // An empty existing file is corrupt, not equivalent to a missing file.
            // Determine absence from open itself, not a separate existence check.
            std::string bytes;
            bool        missing = false;
            return readBytes(path, bytes, missing, error) && (missing || parse(bytes, titles, error));
        }

        bool serialize(const Titles& titles, std::string& bytes, std::string& error) {
            if (titles.size() > MAX_ENTRIES) {
                error = "Workspace-title store exceeds the 4096-entry limit";
                return false;
            }
            bytes = MAGIC;
            appendNumber(bytes, titles.size());
            for (const auto& [key, title] : titles) {
                const auto added = 8 + key.size() + title.size();
                if (added > MAX_FILE - bytes.size()) {
                    error = "Workspace-title store exceeds the 8 MiB limit";
                    return false;
                }
                appendNumber(bytes, key.size());
                appendNumber(bytes, title.size());
                bytes += key;
                bytes += title;
            }
            return true;
        }

        bool sync(int fd) {
            int result;
            do {
                result = ::fsync(fd);
            } while (result < 0 && errno == EINTR);
            return result == 0;
        }

        bool save(const std::filesystem::path& path, std::string_view bytes, bool& renamed, std::string& error) {
            auto parent = path.parent_path();
            if (parent.empty())
                parent = ".";
            std::error_code code;
            std::filesystem::create_directories(parent, code);
            if (code) {
                error = "Cannot create workspace-title directory: " + code.message();
                return false;
            }
            // Always retry the ancestor chain: an earlier failed save may have
            // created directories without successfully syncing their entries.
            // Walk toward the root before rename, preserving memory on failure.
            auto ancestor = parent;
            for (;;) {
                File folder(::open(ancestor.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
                if (folder.fd < 0)
                    return systemError(error, "Cannot open workspace-title ancestor directory", errno);
                if (!sync(folder.fd))
                    return systemError(error, "Cannot sync workspace-title ancestor directory", errno);
                const auto next = ancestor.parent_path();
                if (next == ancestor || ancestor == ".")
                    break;
                ancestor = next.empty() ? std::filesystem::path(".") : next;
            }
            File directory(::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
            if (directory.fd < 0)
                return systemError(error, "Cannot open workspace-title directory", errno);

            auto              pattern = (parent / ".workspace-titles.tmp.XXXXXX").string();
            std::vector<char> name(pattern.begin(), pattern.end());
            name.push_back('\0');
            const int descriptor = ::mkstemp(name.data());
            if (descriptor < 0)
                return systemError(error, "Cannot create workspace-title temporary file", errno);
            TemporaryFile temporary{std::string(name.data())};
            File          file(descriptor);
            if (::fcntl(file.fd, F_SETFD, FD_CLOEXEC) < 0)
                return systemError(error, "Cannot protect workspace-title file descriptor", errno);
            std::size_t written = 0;
            while (written < bytes.size()) {
                const auto count = ::write(file.fd, bytes.data() + written, bytes.size() - written);
                if (count < 0) {
                    if (errno == EINTR)
                        continue;
                    return systemError(error, "Cannot write workspace-title temporary file", errno);
                }
                if (count == 0)
                    return systemError(error, "Cannot write workspace-title temporary file", EIO);
                written += static_cast<std::size_t>(count);
            }
            if (!sync(file.fd))
                return systemError(error, "Cannot sync workspace-title temporary file", errno);
            if (file.close() < 0)
                return systemError(error, "Cannot close workspace-title temporary file", errno);
            if (::rename(temporary.path.c_str(), path.c_str()) < 0)
                return systemError(error, "Cannot replace workspace-title file", errno);
            renamed = true;
            temporary.path.clear();
            if (!sync(directory.fd))
                return systemError(error, "Workspace-title file replaced, but cannot sync its directory", errno);
            return true;
        }
    }

    std::string formatTitle(std::string_view format, std::string_view workspace, std::string_view title) {
        std::string result;
        for (std::size_t at = 0; at < format.size();) {
            if (format[at] != '$') {
                result += format[at++];
                continue;
            }
            const auto start = at++;
            if (at < format.size() && format[at] == '$') {
                result += '$';
                ++at;
                continue;
            }
            // Consume a whole identifier, so $title_suffix stays literal.
            while (at < format.size()) {
                const auto c = format[at];
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
                    break;
                ++at;
            }
            const auto token = format.substr(start, at - start);
            if (token == "$workspace")
                result += workspace;
            else if (token == "$title")
                result += title;
            else
                result += token;
        }
        // Empty titles should not leave the default separator off-center.
        const auto first = result.find_first_not_of(' ');
        if (first == std::string::npos)
            return {};
        return result.substr(first, result.find_last_not_of(' ') - first + 1);
    }

    bool validTitle(std::string_view title) {
        if (title.size() > MAX_TITLE)
            return false;
        for (std::size_t at = 0; at < title.size();) {
            std::uint32_t scalar = 0;
            const auto    length = decode(title, at, scalar);
            if (length == 0 || scalar < 0x20 || scalar == 0x7f || scalar == 0x85 || scalar == 0x2028 || scalar == 0x2029)
                return false;
            at += length;
        }
        return true;
    }

    void Editor::reset(std::string value) {
        text   = validTitle(value) ? std::move(value) : std::string{};
        cursor = text.size();
    }

    bool Editor::insert(std::string_view value) {
        if (!prepare(*this) || !validTitle(value) || value.size() > MAX_TITLE - text.size())
            return false;
        // value may be a view into text; copy before an insertion reallocates it.
        const std::string copy(value);
        text.insert(cursor, copy);
        cursor += copy.size();
        return true;
    }

    void Editor::left() {
        if (prepare(*this))
            cursor = previous(text, cursor);
    }

    void Editor::right() {
        if (prepare(*this) && cursor < text.size()) {
            std::uint32_t scalar = 0;
            cursor += decode(text, cursor, scalar);
        }
    }

    void Editor::home() {
        cursor = 0;
    }

    void Editor::end() {
        if (prepare(*this))
            cursor = text.size();
    }

    void Editor::backspace() {
        if (!prepare(*this) || cursor == 0)
            return;
        const auto start = previous(text, cursor);
        text.erase(start, cursor - start);
        cursor = start;
    }

    void Editor::deleteForward() {
        if (!prepare(*this) || cursor == text.size())
            return;
        std::uint32_t scalar = 0;
        text.erase(cursor, decode(text, cursor, scalar));
    }

    Store::Store(std::filesystem::path path) : m_path(std::move(path)) {}

    std::filesystem::path Store::defaultPath() {
        std::filesystem::path root;
        if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg && *xdg && std::filesystem::path(xdg).is_absolute())
            root = xdg;
        else if (const char* home = std::getenv("HOME"); home && *home && std::filesystem::path(home).is_absolute())
            root = std::filesystem::path(home) / ".local/state";
        if (root.empty())
            return {};
        return root / "hyprland-scroll-overview/workspace-titles.v1";
    }

    bool Store::load(std::string& error) {
        error.clear();
        Titles loaded;
        if (!readTitles(m_path, loaded, error))
            return false;
        m_titles.swap(loaded);
        return true;
    }

    std::string Store::get(const std::string& selector) const {
        const auto found = m_titles.find(selector);
        return found == m_titles.end() ? std::string{} : found->second;
    }

    bool Store::set(const std::string& selector, const std::string& title, std::string& error) {
        if (selector.empty() || !validTitle(selector) || !validTitle(title)) {
            error = "Invalid workspace selector or title (nonempty selector, single-line UTF-8, maximum 4096 bytes each)";
            return false;
        }
        // The output error reference may alias either input reference.
        const std::string key   = selector;
        const std::string value = title;
        error.clear();
        Titles updated;
        if (!readTitles(m_path, updated, error))
            return false;
        if (value.empty())
            updated.erase(key);
        else
            updated[key] = value;
        std::string bytes;
        if (!serialize(updated, bytes, error))
            return false;
        bool       renamed = false;
        const bool saved   = save(m_path, bytes, renamed, error);
        if (renamed)
            m_titles.swap(updated);
        return saved;
    }
}
