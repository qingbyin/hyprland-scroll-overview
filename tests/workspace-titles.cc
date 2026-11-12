#include "WorkspaceTitles.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using WorkspaceTitles::Editor;
using WorkspaceTitles::Store;
using WorkspaceTitles::validTitle;

#define CHECK(condition)                                                                                                                                                           \
    do {                                                                                                                                                                           \
        if (!(condition))                                                                                                                                                          \
            throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #condition);                                                                    \
    } while (false)

namespace {
    enum class Fault {
        None,
        Write,
        ZeroWrite,
        ShortWrite,
        InterruptedWrite,
        FileSync,
        DirectorySync,
        AfterRenameDirectorySync,
        InterruptedSync,
        Rename,
        Close
    };
    Fault    fault                = Fault::None;
    bool     faultObserved        = false;
    unsigned directorySyncsToSkip = 0;

    bool     consume(Fault expected) {
        if (fault != expected)
            return false;
        faultObserved = true;
        fault         = Fault::None;
        errno         = EIO;
        return true;
    }
}

// GNU linker wrappers inject failures without adding test hooks or dependencies
// to the implementation. The ordinary plugin build never uses these wrappers.
extern "C" {
ssize_t __real_write(int, const void*, size_t);
int     __real_fsync(int);
int     __real_rename(const char*, const char*);
int     __real_close(int);

ssize_t __wrap_write(int fd, const void* data, size_t count) {
    if (consume(Fault::Write))
        return -1;
    if (consume(Fault::ZeroWrite))
        return 0;
    if (consume(Fault::InterruptedWrite)) {
        errno = EINTR;
        return -1;
    }
    if (fault == Fault::ShortWrite) {
        faultObserved = true;
        count         = std::min(count, size_t{3});
    }
    return __real_write(fd, data, count);
}

int __wrap_fsync(int fd) {
    struct stat info{};
    if (::fstat(fd, &info) != 0)
        return -1;
    if (S_ISDIR(info.st_mode)) {
        if (fault == Fault::DirectorySync && directorySyncsToSkip > 0)
            --directorySyncsToSkip;
        else if (consume(Fault::DirectorySync))
            return -1;
    } else if (consume(Fault::FileSync)) {
        return -1;
    }
    if (S_ISREG(info.st_mode) && consume(Fault::InterruptedSync)) {
        errno = EINTR;
        return -1;
    }
    return __real_fsync(fd);
}

int __wrap_rename(const char* from, const char* to) {
    if (consume(Fault::Rename))
        return -1;
    const int result = __real_rename(from, to);
    if (result == 0 && fault == Fault::AfterRenameDirectorySync)
        fault = Fault::DirectorySync;
    return result;
}

int __wrap_close(int fd) {
    const int flags = ::fcntl(fd, F_GETFL);
    // mkstemp opens read/write; do not fault the preceding read-only load.
    if (flags >= 0 && (flags & O_ACCMODE) == O_RDWR && consume(Fault::Close)) {
        __real_close(fd);
        errno = EIO;
        return -1;
    }
    return __real_close(fd);
}
}

namespace {
    constexpr std::string_view MAGIC    = "workspace-titles.v1\n";
    constexpr std::size_t      MAX_FILE = 8 * 1024 * 1024;

    void                       writeFile(const fs::path& path, std::string_view bytes) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        CHECK(output.good());
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        CHECK(output.good());
    }

    std::string readFile(const fs::path& path) {
        std::ifstream input(path, std::ios::binary);
        CHECK(input.good());
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    void number(std::string& bytes, std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<char>((value >> shift) & 255));
    }

    std::string header(std::uint32_t count) {
        std::string bytes(MAGIC);
        number(bytes, count);
        return bytes;
    }

    std::string record(std::string_view key, std::string_view title) {
        std::string bytes;
        number(bytes, static_cast<std::uint32_t>(key.size()));
        number(bytes, static_cast<std::uint32_t>(title.size()));
        bytes += key;
        bytes += title;
        return bytes;
    }

    std::string encode(std::uint32_t scalar) {
        std::string result;
        if (scalar < 0x80) {
            result += static_cast<char>(scalar);
        } else if (scalar < 0x800) {
            result += static_cast<char>(0xc0 | (scalar >> 6));
            result += static_cast<char>(0x80 | (scalar & 63));
        } else if (scalar < 0x10000) {
            result += static_cast<char>(0xe0 | (scalar >> 12));
            result += static_cast<char>(0x80 | ((scalar >> 6) & 63));
            result += static_cast<char>(0x80 | (scalar & 63));
        } else {
            result += static_cast<char>(0xf0 | (scalar >> 18));
            result += static_cast<char>(0x80 | ((scalar >> 12) & 63));
            result += static_cast<char>(0x80 | ((scalar >> 6) & 63));
            result += static_cast<char>(0x80 | (scalar & 63));
        }
        return result;
    }

    void testValidation() {
        CHECK(validTitle(""));
        CHECK(validTitle("  中文 \\\" title 😀 e\xcc\x81  "));
        CHECK(validTitle(std::string(4096, 'x')));
        CHECK(!validTitle(std::string(4097, 'x')));
        std::string chinese;
        for (int i = 0; i < 1365; ++i)
            chinese += "中";
        CHECK(validTitle(chinese + "a"));
        CHECK(!validTitle(chinese + "ab"));
        for (int byte = 0; byte < 32; ++byte)
            CHECK(!validTitle(std::string("a") + static_cast<char>(byte) + "b"));
        CHECK(!validTitle("\x7f"));
        const std::vector<std::string> invalid = {
            "\x80",
            "\xbf",
            "\xc0\x80",
            "\xc1\xbf",
            "\xc2",
            "\xc2\x41",
            "\xdf\xff",
            "\xe0\x80\x80",
            "\xe0\x9f\xbf",
            "\xe1\x80",
            "\xe1\x41\x80",
            "\xed\xa0\x80",
            "\xed\xbf\xbf",
            "\xf0\x80\x80\x80",
            "\xf0\x8f\xbf\xbf",
            "\xf1\x80\x80",
            "\xf4\x90\x80\x80",
            "\xf5\x80\x80\x80",
            "\xf8\x88\x80\x80\x80",
            "\xfe",
            "\xff",
            "\xc2\x85",
            "\xe2\x80\xa8",
            "\xe2\x80\xa9",
        };
        for (const auto& bytes : invalid) {
            CHECK(!validTitle(bytes));
            CHECK(!validTitle("prefix" + bytes + "suffix"));
        }
        // Exhaustively cover scalar boundaries, including surrogate encodings.
        for (std::uint32_t scalar = 0; scalar <= 0x10ffff; ++scalar) {
            const bool allowed = scalar >= 32 && scalar != 127 && scalar != 0x85 && scalar != 0x2028 && scalar != 0x2029 && !(scalar >= 0xd800 && scalar <= 0xdfff);
            CHECK(validTitle(encode(scalar)) == allowed);
        }
    }

    void testEditor() {
        Editor editor;
        CHECK(editor.text.empty() && editor.cursor == 0);
        editor.left();
        editor.right();
        editor.backspace();
        editor.deleteForward();
        CHECK(editor.text.empty() && editor.cursor == 0);
        editor.reset("A中😀Z");
        CHECK(editor.cursor == 9);
        editor.left();
        CHECK(editor.cursor == 8);
        editor.left();
        CHECK(editor.cursor == 4);
        editor.left();
        CHECK(editor.cursor == 1);
        editor.left();
        CHECK(editor.cursor == 0);
        editor.left();
        CHECK(editor.cursor == 0);
        editor.right();
        CHECK(editor.cursor == 1);
        editor.right();
        CHECK(editor.cursor == 4);
        CHECK(editor.insert("文"));
        CHECK(editor.text == "A中文😀Z" && editor.cursor == 7);
        editor.backspace();
        CHECK(editor.text == "A中😀Z" && editor.cursor == 4);
        editor.deleteForward();
        CHECK(editor.text == "A中Z" && editor.cursor == 4);
        editor.home();
        CHECK(editor.cursor == 0);
        editor.backspace();
        CHECK(editor.cursor == 0);
        editor.end();
        CHECK(editor.cursor == 5);
        editor.deleteForward();
        CHECK(editor.text == "A中Z");
        CHECK(!editor.insert("\n"));
        CHECK(!editor.insert("\xff"));
        CHECK(editor.text == "A中Z" && editor.cursor == 5);
        editor.reset("bad\nreset");
        CHECK(editor.text.empty() && editor.cursor == 0);
        editor.reset(std::string(4096, 'a'));
        CHECK(!editor.insert("中"));
        CHECK(editor.text.size() == 4096 && editor.cursor == 4096);
        CHECK(editor.insert(""));
        editor.reset("中");
        editor.home();
        CHECK(editor.insert(std::string_view(editor.text)));
        CHECK(editor.text == "中中" && editor.cursor == 3);

        // Public cursor offsets may point anywhere in a multibyte codepoint.
        for (std::size_t offset : {2u, 3u, 5u, 6u, 7u}) {
            const std::size_t start = offset < 4 ? 1 : 4;
            editor.reset("A中😀Z");
            editor.cursor = offset;
            CHECK(editor.insert("x"));
            CHECK(editor.cursor == start + 1 && validTitle(editor.text));
            CHECK(editor.text == (start == 1 ? "Ax中😀Z" : "A中x😀Z"));
            editor.reset("A中😀Z");
            editor.cursor = offset;
            editor.right();
            CHECK(editor.cursor == (start == 1 ? 4u : 8u));
            editor.reset("A中😀Z");
            editor.cursor = offset;
            editor.left();
            CHECK(editor.cursor == (start == 1 ? 0u : 1u));
            editor.reset("A中😀Z");
            editor.cursor = offset;
            editor.deleteForward();
            CHECK(editor.cursor == start && editor.text == (start == 1 ? "A😀Z" : "A中Z"));
            editor.reset("A中😀Z");
            editor.cursor = offset;
            editor.backspace();
            CHECK(editor.cursor == (start == 1 ? 0u : 1u));
            CHECK(editor.text == (start == 1 ? "中😀Z" : "A😀Z"));
        }
        editor.reset("中");
        editor.cursor = std::numeric_limits<std::size_t>::max();
        editor.right();
        CHECK(editor.cursor == 3);
        editor.backspace();
        CHECK(editor.text.empty() && editor.cursor == 0);
        editor.text   = "\xff";
        editor.cursor = 99;
        CHECK(!editor.insert("x"));
        editor.left();
        editor.right();
        editor.backspace();
        editor.deleteForward();
        editor.end();
        CHECK(editor.text == "\xff" && editor.cursor == 0);
        editor.reset("e\xcc\x81");
        editor.backspace();
        CHECK(editor.text == "e" && editor.cursor == 1); // Codepoints, not graphemes.
    }

    void testRandomEditor() {
        std::mt19937                   random(417);
        const std::vector<std::string> alphabet = {"A", "中", "😀", "é", "\xcc\x81"};
        std::vector<std::string>       reference;
        std::size_t                    position = 0;
        Editor                         editor;
        for (int iteration = 0; iteration < 10000; ++iteration) {
            switch (random() % 8) {
                case 0: {
                    const auto& value = alphabet[random() % alphabet.size()];
                    CHECK(editor.insert(value));
                    reference.insert(reference.begin() + static_cast<std::ptrdiff_t>(position), value);
                    ++position;
                    break;
                }
                case 1:
                    editor.left();
                    if (position > 0)
                        --position;
                    break;
                case 2:
                    editor.right();
                    if (position < reference.size())
                        ++position;
                    break;
                case 3:
                    editor.home();
                    position = 0;
                    break;
                case 4:
                    editor.end();
                    position = reference.size();
                    break;
                case 5:
                    editor.backspace();
                    if (position > 0)
                        reference.erase(reference.begin() + static_cast<std::ptrdiff_t>(--position));
                    break;
                case 6:
                    editor.deleteForward();
                    if (position < reference.size())
                        reference.erase(reference.begin() + static_cast<std::ptrdiff_t>(position));
                    break;
                case 7: CHECK(!editor.insert("\x80")); break;
            }
            std::string expected;
            std::size_t bytePosition = 0;
            for (std::size_t i = 0; i < reference.size(); ++i) {
                expected += reference[i];
                if (i < position)
                    bytePosition += reference[i].size();
            }
            CHECK(editor.text == expected && editor.cursor == bytePosition);
        }
    }

    void testStore(const fs::path& root) {
        const auto  path = root / "nested/state/workspace-titles.v1";
        Store       store(path);
        std::string error = "stale error";
        CHECK(store.load(error) && error.empty());
        CHECK(store.get("absent").empty() && !fs::exists(path));
        const std::vector<std::pair<std::string, std::string>> entries = {
            {"name:中文 \\\" workspace", "  中文 \\\" title 😀  "},
            {"1", "one"},
            {"01", "not one"},
            {"name:1", "named one"},
            {"name:back\\slash \"quotes\" 中文 space", "exact selector bytes"},
        };
        for (const auto& [key, title] : entries)
            CHECK(store.set(key, title, error) && error.empty());
        Store reloaded(path);
        CHECK(reloaded.load(error) && error.empty());
        for (const auto& [key, title] : entries)
            CHECK(reloaded.get(key) == title);
        CHECK(reloaded.get("name:中文").empty());
        CHECK(readFile(path).starts_with(MAGIC));
        struct stat info{};
        CHECK(::stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600);
        const auto oldInode = info.st_ino;
        CHECK(reloaded.set("1", "updated", error));
        CHECK(::stat(path.c_str(), &info) == 0 && info.st_ino != oldInode);
        CHECK(reloaded.set("01", "", error));
        CHECK(reloaded.get("01").empty() && reloaded.get("1") == "updated");
        CHECK(reloaded.set("absent", "", error));
        // An old Store must preserve updates from another instance on disk.
        CHECK(store.set("late", "new entry", error));
        CHECK(store.get("1") == "updated" && store.get("01").empty());
        CHECK(store.get("late") == "new entry");
        const auto unchanged = readFile(path);
        CHECK(!store.set("1", "bad\n", error) && !error.empty());
        CHECK(!store.set("1", std::string(4097, 'x'), error) && !error.empty());
        CHECK(!store.set(std::string(4097, 'k'), "title", error) && !error.empty());
        CHECK(readFile(path) == unchanged && store.get("1") == "updated");
        for (const auto& key : {std::string{}, std::string("bad\0key", 7), std::string("bad\nkey"), std::string("\xff")}) {
            CHECK(!store.set(key, "title", error) && !error.empty());
            CHECK(!store.set(key, "", error) && !error.empty());
        }
        CHECK(readFile(path) == unchanged);
        const std::string longKey(4096, 'k');
        CHECK(store.set(longKey, std::string(4096, 'x'), error));
        CHECK(reloaded.load(error) && reloaded.get(longKey) == std::string(4096, 'x'));
        std::string aliasedTitle = "aliased title";
        CHECK(store.set("alias", aliasedTitle, aliasedTitle) && aliasedTitle.empty());
        CHECK(store.get("alias") == "aliased title");
        std::string aliasedKey = "aliased key";
        CHECK(store.set(aliasedKey, "value", aliasedKey) && aliasedKey.empty());
        CHECK(store.get("aliased key") == "value");
        fs::remove(path);
        CHECK(store.load(error) && error.empty() && store.get("1").empty());
    }

    void testMalformed(const fs::path& root) {
        const auto  path = root / "malformed.v1";
        Store       store(path);
        std::string error;
        CHECK(store.set("keep", "old value", error));
        const auto  valid           = readFile(path);
        std::string oversizedLength = header(1);
        number(oversizedLength, 0xffffffff);
        number(oversizedLength, 1);
        const std::vector<std::string> malformed = {
            "",
            "garbage",
            "workspace-titles.v2\n",
            std::string(MAGIC),
            header(0xffffffff),
            header(4097),
            header(1),
            header(1) + "\x01",
            oversizedLength,
            header(1) + record("key", ""),
            header(1) + record("key", "\xff"),
            header(1) + record("key", "bad\n"),
            header(1) + record("key", std::string(4097, 'x')),
            header(1) + record(std::string(4097, 'k'), "title"),
            header(1) + record("", "title"),
            header(1) + record(std::string("bad\0key", 7), "title"),
            header(1) + record("bad\nkey", "title"),
            header(1) + record("\xff", "title"),
            header(2) + record("duplicate", "first") + record("duplicate", "second"),
            header(2) + record("partial", "must not load") + record("other", "value").substr(0, 10),
            header(1) + record("key", "value").substr(0, 12),
            valid + "trailing",
            header(0) + "junk",
        };
        for (const auto& bytes : malformed) {
            writeFile(path, bytes);
            CHECK(!store.load(error) && !error.empty());
            CHECK(store.get("keep") == "old value" && store.get("partial").empty());
            CHECK(!store.set("new", "must not overwrite", error) && !error.empty());
            CHECK(!store.set("keep", "", error) && !error.empty());
            CHECK(readFile(path) == bytes && store.get("keep") == "old value");
            Store fresh(path);
            CHECK(!fresh.set("new", "must not overwrite", error));
            CHECK(readFile(path) == bytes && fresh.get("new").empty());
        }
        // Every truncation of a complete single-record file is malformed.
        for (std::size_t size = 0; size < valid.size(); ++size) {
            writeFile(path, std::string_view(valid).substr(0, size));
            CHECK(!store.load(error));
            CHECK(store.get("keep") == "old value");
        }
        writeFile(path, valid);
        fs::resize_file(path, MAX_FILE + 1);
        CHECK(!store.load(error) && !error.empty());
        CHECK(!store.set("new", "value", error) && fs::file_size(path) == MAX_FILE + 1);
        CHECK(store.get("keep") == "old value");
        writeFile(path, header(0));
        CHECK(store.load(error) && store.get("keep").empty());
    }

    void testLimits(const fs::path& root) {
        const auto  path  = root / "limits.v1";
        std::string bytes = header(4096);
        for (int i = 0; i < 4096; ++i)
            bytes += record(std::to_string(i), "v");
        writeFile(path, bytes);
        Store       store(path);
        std::string error;
        CHECK(store.load(error));
        CHECK(!store.set("extra", "v", error) && !error.empty());
        CHECK(readFile(path) == bytes && store.get("extra").empty());
        CHECK(store.set("4095", "updated", error));
        CHECK(store.set("0", "", error));
        CHECK(store.set("extra", "v", error));

        // A valid file just below the byte limit must not grow beyond it.
        const std::string title(4096, 't');
        std::string       records;
        std::uint32_t     count = 0;
        while (true) {
            const auto key  = std::string(12 - std::to_string(count).size(), '0') + std::to_string(count);
            const auto item = record(key, title);
            if (MAGIC.size() + 4 + records.size() + item.size() > MAX_FILE)
                break;
            records += item;
            ++count;
        }
        bytes = header(count) + records;
        writeFile(path, bytes);
        CHECK(store.load(error));
        CHECK(!store.set("new-key-1234", title, error) && !error.empty());
        CHECK(store.get("new-key-1234").empty() && readFile(path) == bytes);
    }

    void testFailures(const fs::path& root) {
        const auto  directory = root / "failures";
        const auto  path      = directory / "titles.v1";
        Store       store(path);
        std::string error;
        CHECK(store.set("key", "old", error));
        const auto original = readFile(path);
        for (const auto failure : {Fault::Write, Fault::ZeroWrite, Fault::FileSync, Fault::DirectorySync, Fault::Rename, Fault::Close}) {
            fault             = failure;
            faultObserved     = false;
            const bool result = store.set("key", "new", error);
            fault             = Fault::None;
            CHECK(faultObserved && !result && !error.empty());
            CHECK(store.get("key") == "old" && readFile(path) == original);
            CHECK(std::distance(fs::directory_iterator(directory), fs::directory_iterator{}) == 1);
        }
        for (const auto retry : {Fault::ShortWrite, Fault::InterruptedWrite, Fault::InterruptedSync}) {
            fault             = retry;
            faultObserved     = false;
            const bool result = store.set("key", "old", error);
            fault             = Fault::None;
            CHECK(faultObserved && result && error.empty());
            CHECK(readFile(path) == original);
        }
        fault             = Fault::AfterRenameDirectorySync;
        faultObserved     = false;
        const bool result = store.set("key", "committed", error);
        fault             = Fault::None;
        CHECK(faultObserved && !result && error.find("replaced") != std::string::npos);
        CHECK(store.get("key") == "committed");
        Store committed(path);
        CHECK(committed.load(error) && committed.get("key") == "committed");
        CHECK(std::distance(fs::directory_iterator(directory), fs::directory_iterator{}) == 1);

        const auto newPath = root / "new-directories/a/b/titles.v1";
        Store      newDirectories(newPath);
        // Syncing b succeeds, then syncing a fails. Both initial creation and
        // a retry with already-existing directories must fail before rename.
        for (int attempt = 0; attempt < 2; ++attempt) {
            fault                = Fault::DirectorySync;
            directorySyncsToSkip = 1;
            faultObserved        = false;
            const bool saved     = newDirectories.set("key", "title", error);
            fault                = Fault::None;
            directorySyncsToSkip = 0;
            CHECK(faultObserved && !saved && !error.empty());
            CHECK(newDirectories.get("key").empty() && !fs::exists(newPath));
            CHECK(fs::is_directory(newPath.parent_path()));
        }
        CHECK(newDirectories.set("key", "title", error) && error.empty());
        Store retried(newPath);
        CHECK(retried.load(error) && retried.get("key") == "title");

        const auto blocker = root / "not-a-directory";
        writeFile(blocker, "block");
        Store badParent(blocker / "titles.v1");
        CHECK(!badParent.load(error) && !error.empty());
        CHECK(!badParent.set("key", "title", error) && !error.empty());
        CHECK(readFile(blocker) == "block");
        Store directoryFile(directory);
        CHECK(!directoryFile.load(error) && !error.empty());
        CHECK(!directoryFile.set("key", "title", error));
        const auto fifo = root / "fifo";
        CHECK(::mkfifo(fifo.c_str(), 0600) == 0);
        Store pipe(fifo);
        CHECK(!pipe.load(error) && !error.empty());
        CHECK(!pipe.set("key", "title", error));
        Store embeddedNul(fs::path(std::string("bad\0path", 8)));
        CHECK(!embeddedNul.load(error) && !error.empty());
        CHECK(!embeddedNul.set("key", "title", error));
    }

    struct Environment {
        std::string                name;
        std::optional<std::string> previous;
        explicit Environment(std::string variable) : name(std::move(variable)) {
            if (const char* value = std::getenv(name.c_str()))
                previous = value;
        }
        ~Environment() {
            if (previous)
                ::setenv(name.c_str(), previous->c_str(), 1);
            else
                ::unsetenv(name.c_str());
        }
        void set(const char* value) const {
            CHECK(value ? ::setenv(name.c_str(), value, 1) == 0 : ::unsetenv(name.c_str()) == 0);
        }
    };

    void testDefaultPath(const fs::path& root) {
        Environment xdg("XDG_STATE_HOME");
        Environment home("HOME");
        const auto  suffix = fs::path("hyprland-scroll-overview/workspace-titles.v1");
        home.set((root / "home").c_str());
        xdg.set((root / "xdg state 中文").c_str());
        CHECK(Store::defaultPath() == root / "xdg state 中文" / suffix);
        home.set(nullptr);
        CHECK(Store::defaultPath() == root / "xdg state 中文" / suffix);
        home.set((root / "home").c_str());
        for (const char* value : {"relative/xdg", "", static_cast<const char*>(nullptr)}) {
            xdg.set(value);
            CHECK(Store::defaultPath() == root / "home/.local/state" / suffix);
        }
        xdg.set("relative");
        for (const char* value : {"relative/home", "", static_cast<const char*>(nullptr)}) {
            home.set(value);
            CHECK(Store::defaultPath().empty());
            Store       unavailable(Store::defaultPath());
            std::string error;
            CHECK(!unavailable.load(error) && !error.empty());
            CHECK(!unavailable.set("key", "title", error) && !error.empty());
        }
        xdg.set(nullptr);
        home.set(nullptr);
        CHECK(Store::defaultPath().empty());
        Store       missingEnvironment(Store::defaultPath());
        std::string missingError;
        CHECK(!missingEnvironment.load(missingError) && !missingError.empty());
        CHECK(!missingEnvironment.set("key", "title", missingError) && !missingError.empty());
        xdg.set((root / "xdg state 中文").c_str());
        Store       store(Store::defaultPath());
        std::string error;
        CHECK(store.set("name:中文", "persisted", error));
        CHECK(fs::exists(Store::defaultPath()));
    }
}

void testFormat() {
    using WorkspaceTitles::formatTitle;
    CHECK(formatTitle("$workspace    $title", "1", "开发") == "1    开发");
    CHECK(formatTitle("$workspace    $title", "命名工作区", "") == "命名工作区");
    CHECK(formatTitle("$workspace   $title", "3", "开发") == "3   开发");
    CHECK(formatTitle("$workspace   $title", "3", "") == "3 ");
    CHECK(formatTitle("$title / $workspace / $title", "3", "开发") == "开发 / 3 / 开发");
    CHECK(formatTitle("$title", "3", "开发") == "开发");
    CHECK(formatTitle("$workspace", "3", "开发") == "3");
    CHECK(formatTitle("literal ", "3", "开发") == "literal ");
    CHECK(formatTitle("", "3", "开发").empty());
    CHECK(formatTitle(" $title ", "3", "").empty());
    CHECK(formatTitle("   ", "3", "开发").empty());
    CHECK(formatTitle("  $workspace  /  $title  ", "3", "开发") == "3  /  开发");
    CHECK(formatTitle("$$workspace $$title $$$title $", "3", "开发") == "$workspace $title $开发 $");
    CHECK(formatTitle("$unknown $title_suffix $workspace2 ${title}", "3", "开发") ==
          "$unknown $title_suffix $workspace2 ${title}");
    CHECK(formatTitle("$title$workspace", "3", "开发") == "开发3");
    CHECK(formatTitle("$workspace中文$title", "3", "开发") == "3中文开发");
    CHECK(formatTitle("$workspace / $title", "$title", "$workspace") == "$title / $workspace");
    CHECK(formatTitle("[$title]", "3", "<b>开发</b>") == "[<b>开发</b>]");
    CHECK(formatTitle("$title$title", "3", std::string(4096, 'x')) == std::string(8192, 'x'));
}

int main(int argc, char** argv) {
    try {
        CHECK(argc == 2);
        const auto root = fs::absolute(argv[1]);
        CHECK(fs::is_directory(root));
        testFormat();
        testValidation();
        testEditor();
        testRandomEditor();
        testStore(root);
        testMalformed(root);
        testLimits(root);
        testFailures(root);
        testDefaultPath(root);
        std::cout << "All workspace-title tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        fault = Fault::None;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
