#include "WindowSearch.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using WindowSearch::Candidate;
using WindowSearch::Editor;
using WindowSearch::match;
using WindowSearch::validQuery;

std::size_t checks = 0;

void check(bool condition, const char* expression, int line) {
    ++checks;
    if (!condition)
        throw std::runtime_error("line " + std::to_string(line) + ": " + expression);
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

template <typename Function>
void expectInvalid(Function&& function, int line) {
    bool rejected = false;
    try {
        function();
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "expected std::invalid_argument", line);
}

#define EXPECT_INVALID(expression) \
    expectInvalid([&] { (void)(expression); }, __LINE__)

std::string bytes(std::initializer_list<unsigned int> values) {
    std::string result;
    for (const auto byte : values)
        result.push_back(static_cast<char>(byte));
    return result;
}

// Independent encoder used to exercise every scalar value, including edges.
std::string encode(std::uint32_t codepoint) {
    if (codepoint <= 0x7F)
        return bytes({codepoint});
    if (codepoint <= 0x7FF)
        return bytes({0xC0 | (codepoint >> 6), 0x80 | (codepoint & 0x3F)});
    if (codepoint <= 0xFFFF)
        return bytes({0xE0 | (codepoint >> 12), 0x80 | ((codepoint >> 6) & 0x3F),
                      0x80 | (codepoint & 0x3F)});
    return bytes({0xF0 | (codepoint >> 18), 0x80 | ((codepoint >> 12) & 0x3F),
                  0x80 | ((codepoint >> 6) & 0x3F), 0x80 | (codepoint & 0x3F)});
}

void expectOrder(const std::vector<Candidate>& candidates, std::string_view query,
                 std::initializer_list<std::size_t> expected) {
    const auto actual = match(candidates, query);
    const std::vector<std::size_t> wanted(expected);
    if (actual != wanted) {
        std::string message = "unexpected index order: got";
        for (const auto index : actual)
            message += " " + std::to_string(index);
        message += "; expected";
        for (const auto index : wanted)
            message += " " + std::to_string(index);
        throw std::runtime_error(message);
    }
    CHECK(actual == wanted);
}

void rejectEverywhere(const std::string& invalid) {
    CHECK(!validQuery(invalid));
    EXPECT_INVALID(match({}, invalid));
    EXPECT_INVALID(match({{1, "title", "app", "workspace", 1}}, invalid));
    EXPECT_INVALID(Editor(invalid));
    Editor editor(u8"a中😀z");
    CHECK(editor.left());
    const auto oldText = editor.text();
    const auto oldCursor = editor.cursor();
    EXPECT_INVALID(editor.set(invalid));
    CHECK(editor.text() == oldText);
    CHECK(editor.cursor() == oldCursor);
    EXPECT_INVALID(editor.insert(invalid));
    CHECK(editor.text() == oldText);
    CHECK(editor.cursor() == oldCursor);
}

void testUTF8() {
    CHECK(validQuery({}));
    CHECK(validQuery("plain ASCII !~"));
    CHECK(validQuery(u8"中文 café 😀"));
    // Cc controls are excluded; noncharacters and format/combining characters
    // are still Unicode scalar values and are deliberately accepted.
    for (std::uint32_t codepoint = 0; codepoint <= 0x10FFFF; ++codepoint) {
        if (codepoint >= 0xD800 && codepoint <= 0xDFFF)
            continue;
        const bool allowed = codepoint > 0x1F &&
                             !(codepoint >= 0x7F && codepoint <= 0x9F);
        CHECK(validQuery(encode(codepoint)) == allowed);
    }
    for (unsigned int byte = 0; byte <= 0xFF; ++byte)
        CHECK(validQuery(bytes({byte})) == (byte >= 0x20 && byte <= 0x7E));

    const std::vector<std::string> malformed = {
        bytes({0x80}), bytes({0xBF}), bytes({0xC0, 0x80}), bytes({0xC0, 0xAF}),
        bytes({0xC1, 0xBF}), bytes({0xC2}), bytes({0xC2, 0x20}),
        bytes({0xE0, 0x80, 0x80}), bytes({0xE0, 0x9F, 0xBF}),
        bytes({0xE1, 0x80}), bytes({0xE1, 0x41, 0x80}),
        bytes({0xE1, 0x80, 0x41}), bytes({0xED, 0xA0, 0x80}),
        bytes({0xED, 0xBF, 0xBF}), bytes({0xF0, 0x80, 0x80, 0x80}),
        bytes({0xF0, 0x8F, 0xBF, 0xBF}), bytes({0xF1, 0x80, 0x80}),
        bytes({0xF1, 0x80, 0x41, 0x80}), bytes({0xF1, 0x80, 0x80, 0x41}),
        bytes({0xF4, 0x90, 0x80, 0x80}), bytes({0xF5, 0x80, 0x80, 0x80}),
        bytes({0xF7, 0xBF, 0xBF, 0xBF}), bytes({0xF8, 0x88, 0x80, 0x80, 0x80}),
        bytes({0xFC, 0x84, 0x80, 0x80, 0x80, 0x80}), bytes({0xFE}), bytes({0xFF}),
        std::string("before\0after", 12), "line\nbreak", "tab\tstop", "\r", "\x1B"};
    for (const auto& invalid : malformed) {
        rejectEverywhere(invalid);
        rejectEverywhere("a" + invalid + "z");
    }
    for (std::uint32_t codepoint = 0; codepoint <= 0x9F; ++codepoint) {
        if (codepoint <= 0x1F || codepoint >= 0x7F)
            rejectEverywhere(encode(codepoint));
    }
    const std::vector<std::uint32_t> boundaries = {
        0xA0, 0x7FF, 0x800, 0xD7FF, 0xE000, 0xFFFF, 0x10000, 0x10FFFF};
    for (const auto codepoint : boundaries) {
        const auto encoded = encode(codepoint);
        for (std::size_t length = 1; length < encoded.size(); ++length)
            rejectEverywhere(encoded.substr(0, length));
    }
    const char bounded[] = {'a', 'b', 'c', '\0', static_cast<char>(0xFF)};
    CHECK(validQuery(std::string_view(bounded, 3)));
    CHECK(!validQuery(std::string_view(bounded, sizeof(bounded))));
}

void testLimits() {
    const auto maximum = WindowSearch::MAX_QUERY_BYTES;
    CHECK(maximum == 4096);
    const std::string full(maximum, 'a');
    CHECK(validQuery(full));
    Editor editor(full);
    CHECK(editor.cursor() == maximum);
    editor.home();
    EXPECT_INVALID(editor.insert("b"));
    CHECK(editor.text() == full);
    CHECK(editor.cursor() == 0);
    editor.insert({});
    CHECK(editor.text() == full);
    CHECK(editor.cursor() == 0);
    CHECK(editor.eraseForward());
    editor.insert("b");
    CHECK(editor.text().size() == maximum);
    CHECK(editor.cursor() == 1);
    rejectEverywhere(std::string(maximum + 1, 'a'));

    std::string emoji;
    for (std::size_t i = 0; i < maximum / 4; ++i)
        emoji += u8"😀";
    CHECK(emoji.size() == maximum);
    CHECK(validQuery(emoji));
    editor.set(emoji);
    EXPECT_INVALID(editor.insert(u8"中"));
    CHECK(editor.text() == emoji);
    CHECK(editor.cursor() == maximum);
    CHECK(editor.backspace());
    CHECK(editor.cursor() == maximum - 4);
    editor.insert(u8"中x");
    CHECK(editor.text().size() == maximum);
    CHECK(editor.cursor() == maximum);
    rejectEverywhere(emoji + "x");
    rejectEverywhere(emoji.substr(0, maximum - 1));

    const std::vector<Candidate> candidates = {{1, full, "", "", 0}};
    expectOrder(candidates, full, {0});
    expectOrder(candidates, std::string(maximum, ' '), {0});
    std::string manyWords;
    for (std::size_t i = 0; i < maximum / 2; ++i)
        manyWords += "a ";
    expectOrder(candidates, manyWords, {0});
}

void testEditor() {
    Editor editor;
    CHECK(editor.text().empty());
    CHECK(editor.cursor() == 0);
    CHECK(!editor.left());
    CHECK(!editor.right());
    CHECK(!editor.backspace());
    CHECK(!editor.eraseForward());
    editor.home();
    editor.end();
    editor.set(u8"aé中😀");
    CHECK(editor.cursor() == 10);
    for (const auto offset : {6U, 3U, 1U, 0U}) {
        CHECK(editor.left());
        CHECK(editor.cursor() == offset);
    }
    CHECK(!editor.left());
    for (const auto offset : {1U, 3U, 6U, 10U}) {
        CHECK(editor.right());
        CHECK(editor.cursor() == offset);
    }
    CHECK(!editor.right());
    CHECK(editor.backspace());
    CHECK(editor.text() == u8"aé中");
    CHECK(editor.cursor() == 6);
    CHECK(editor.left());
    CHECK(editor.eraseForward());
    CHECK(editor.text() == u8"aé");
    CHECK(editor.cursor() == 3);
    CHECK(editor.left());
    editor.insert(u8"😀中");
    CHECK(editor.text() == u8"a😀中é");
    CHECK(editor.cursor() == 8);
    CHECK(editor.eraseForward());
    CHECK(editor.text() == u8"a😀中");
    editor.home();
    CHECK(editor.eraseForward());
    CHECK(editor.text() == u8"😀中");
    CHECK(editor.cursor() == 0);
    editor.end();
    CHECK(editor.cursor() == 7);
    editor.set({});
    CHECK(editor.text().empty());
    CHECK(editor.cursor() == 0);

    // Combining marks and joined emoji are edited by codepoint, not grapheme.
    editor.set(u8"e\u0301");
    CHECK(editor.backspace());
    CHECK(editor.text() == "e");
    editor.set(u8"👩\u200D💻");
    CHECK(editor.left());
    CHECK(editor.cursor() == 7);
    CHECK(editor.backspace());
    CHECK(editor.text() == u8"👩💻");
    CHECK(editor.cursor() == 4);
    CHECK(editor.eraseForward());
    CHECK(editor.text() == u8"👩");

    // Self-referencing views must remain safe when storage is replaced.
    editor.set(u8"a中b");
    editor.home();
    CHECK(editor.right());
    editor.insert(std::string_view(editor.text()).substr(1, 3));
    CHECK(editor.text() == u8"a中中b");
    CHECK(editor.cursor() == 4);
    editor.set(std::string_view(editor.text()).substr(1, 6));
    CHECK(editor.text() == u8"中中");
    CHECK(editor.cursor() == 6);
    editor.insert(editor.text());
    CHECK(editor.text() == u8"中中中中");
    CHECK(editor.cursor() == 12);
    EXPECT_INVALID(editor.set(std::string_view(editor.text()).substr(1)));
    CHECK(editor.text() == u8"中中中中");
    CHECK(editor.cursor() == 12);

    CHECK(editor.left());
    Editor copied(editor);
    CHECK(copied.text() == editor.text());
    CHECK(copied.cursor() == editor.cursor());
    Editor assigned;
    assigned = copied;
    CHECK(assigned.text() == copied.text());
    CHECK(assigned.cursor() == copied.cursor());
    Editor moved(std::move(copied));
    CHECK(moved.text() == assigned.text());
    CHECK(moved.cursor() == assigned.cursor());
    CHECK(copied.text().empty());
    CHECK(copied.cursor() == 0);
    CHECK(!copied.left());
    copied.insert("usable");
    CHECK(copied.text() == "usable");
    assigned = std::move(moved);
    CHECK(assigned.text() == editor.text());
    CHECK(assigned.cursor() == editor.cursor());
    CHECK(moved.text().empty());
    CHECK(moved.cursor() == 0);
    auto& self = assigned;
    assigned = std::move(self);
    CHECK(assigned.text() == editor.text());
    CHECK(assigned.cursor() == editor.cursor());
}

std::string join(const std::vector<std::string>& codepoints, std::size_t count) {
    std::string result;
    for (std::size_t i = 0; i < count; ++i)
        result += codepoints[i];
    return result;
}

void testEditorModel() {
    std::mt19937 random(0xC0DE);
    const std::vector<std::string> alphabet = {
        "a", "Z", " ", u8"é", u8"中", u8"😀", u8"\u0301", u8"\u200D", encode(0x10FFFF)};
    std::vector<std::string> model;
    std::size_t cursor = 0;
    Editor editor;
    for (int step = 0; step < 10000; ++step) {
        switch (random() % 8) {
            case 0: {
                const auto& point = alphabet[random() % alphabet.size()];
                editor.insert(point);
                model.insert(model.begin() + static_cast<std::ptrdiff_t>(cursor), point);
                ++cursor;
                break;
            }
            case 1:
                CHECK(editor.left() == (cursor > 0));
                if (cursor > 0)
                    --cursor;
                break;
            case 2:
                CHECK(editor.right() == (cursor < model.size()));
                if (cursor < model.size())
                    ++cursor;
                break;
            case 3:
                CHECK(editor.backspace() == (cursor > 0));
                if (cursor > 0) {
                    --cursor;
                    model.erase(model.begin() + static_cast<std::ptrdiff_t>(cursor));
                }
                break;
            case 4:
                CHECK(editor.eraseForward() == (cursor < model.size()));
                if (cursor < model.size())
                    model.erase(model.begin() + static_cast<std::ptrdiff_t>(cursor));
                break;
            case 5:
                editor.home();
                cursor = 0;
                break;
            case 6:
                editor.end();
                cursor = model.size();
                break;
            case 7:
                editor.set(join(model, model.size()));
                cursor = model.size();
                break;
        }
        CHECK(editor.text() == join(model, model.size()));
        CHECK(editor.cursor() == join(model, cursor).size());
        CHECK(validQuery(editor.text()));
    }
}

void testMatching() {
    const std::vector<Candidate> candidates = {
        {10, "Project Atlas", "org.Code.Editor", "Research", 12},
        {11, "atlas notes", "org.Reader", "Personal", 2},
        {12, "Unrelated", "org.Terminal", "Atlas Lab", 120}};
    expectOrder(candidates, "PROJECT editor research 12", {0});
    expectOrder(candidates, "ATLAS", {1, 2, 0});
    expectOrder(candidates, "atlas atlas", {1, 2, 0});
    expectOrder(candidates, "  atlas   ", {1, 2, 0});
    expectOrder(candidates, "12", {0, 2});
    expectOrder(candidates, "20", {2});
    expectOrder(candidates, "org. editor", {0});
    expectOrder(candidates, "org.code.EDITOR", {0});
    expectOrder(candidates, "Atlasorg", {});
    expectOrder(candidates, "atlas missing", {});
    expectOrder(candidates, "atlas personal editor", {});
    expectOrder(candidates, "", {1, 0, 2});
    expectOrder(candidates, "   ", {1, 0, 2});
    expectOrder({}, "valid query", {});
    expectOrder({}, {}, {});
    for (const auto separator : {0x20U, 0xA0U, 0x1680U, 0x2000U, 0x2001U,
                                 0x2002U, 0x2003U, 0x2004U, 0x2005U, 0x2006U,
                                 0x2007U, 0x2008U, 0x2009U, 0x200AU, 0x2028U,
                                 0x2029U, 0x202FU, 0x205FU, 0x3000U}) {
        expectOrder(candidates, "atlas" + encode(separator) + "editor", {0});
        expectOrder(candidates, encode(separator), {1, 0, 2});
    }

    const std::vector<Candidate> unicode = {
        {1, u8"中文 École CAFÉ 😀", "Org.App", u8"工作区", -12},
        {2, u8"中文 école cafe\u0301 😁", "org.app", "other", std::nullopt}};
    expectOrder(unicode, u8"中文 ÉCOLE cafÉ 😀 工作 -12", {0});
    expectOrder(unicode, u8"école", {1});
    expectOrder(unicode, u8"École", {0});
    expectOrder(unicode, u8"CAFÉ", {0});
    expectOrder(unicode, u8"café", {});
    expectOrder(unicode, u8"cafe\u0301", {1});
    expectOrder(unicode, "-1", {0});
    expectOrder(unicode, "0", {});
    expectOrder({{1, "a*b [x].", "", "", std::nullopt}}, "* [x].", {0});
    expectOrder({{1, "abc", "def", "", std::nullopt}}, "cde", {});
    expectOrder({{1, "", "", "", std::nullopt}}, "0", {});
    expectOrder({{1, "", "", "", 0}}, "0", {0});
    // Candidate bytes are opaque data; malformed metadata does not reject an
    // otherwise valid query or cause ASCII folding to touch high bytes.
    expectOrder({{1, bytes({0xFF}) + "ABC", "", "", 0}}, "abc", {0});
}

void testRelevance() {
    std::vector<Candidate> candidates;
    for (std::size_t kind = 0; kind < 3; ++kind) {
        for (std::size_t field = 0; field < 4; ++field) {
            Candidate candidate{kind * 4 + field, "unrelated", "app", "ws", -999};
            const std::string value = kind == 0 ? "2" : kind == 1 ? "2rest" : "x2";
            if (field == 0)
                candidate.title = value;
            else if (field == 1)
                candidate.appID = value;
            else if (field == 2)
                candidate.workspaceName = value;
            else
                candidate.workspaceNumber = kind == 0 ? 2 : kind == 1 ? 20 : 12;
            candidates.push_back(std::move(candidate));
        }
    }
    std::reverse(candidates.begin(), candidates.end());
    const auto actual = match(candidates, "2");
    CHECK(actual.size() == 12);
    for (std::size_t i = 0; i < actual.size(); ++i)
        CHECK(actual[i] == 11 - i);

    // Use the best match among fields rather than the first matching field.
    expectOrder({{1, "x2", "2", "", 100}, {2, "2rest", "", "", -100}}, "2", {0, 1});
    const std::vector<Candidate> multi = {
        {1, "red blue", "", "", 0}, {2, "red", "blue", "", 10},
        {3, "red", "", "blue", 0}, {4, "blue", "red", "", 2}};
    expectOrder(multi, "red blue", {3, 1, 2, 0});
    expectOrder(multi, "blue red", {3, 1, 2, 0});
}

void testWorkspaceSorting() {
    const auto minimum = std::numeric_limits<std::int64_t>::min();
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    const std::vector<Candidate> numbered = {
        {1, "same", "app", "", maximum}, {2, "same", "app", "", minimum},
        {3, "same", "app", "", 10}, {4, "same", "app", "", 2},
        {5, "same", "app", "", 0}, {6, "same", "app", "", -1},
        {7, "same", "app", "", -10}, {8, "same", "app", "0", std::nullopt}};
    expectOrder(numbered, "", {1, 6, 5, 4, 3, 2, 0, 7});
    expectOrder(numbered, std::to_string(minimum), {1});
    expectOrder(numbered, std::to_string(maximum), {0});

    const std::vector<std::string> names = {
        "10", "2", "02", "0002", "0", "00", "desk10part1", "desk2part10",
        "Desk2part2", "", "9999999999999999999999999999999999999999",
        "10000000000000000000000000000000000000000"};
    std::vector<Candidate> named;
    for (std::size_t i = 0; i < names.size(); ++i)
        named.push_back({i, "same", "app", names[i], std::nullopt});
    expectOrder(named, "", {9, 4, 5, 1, 2, 3, 0, 10, 11, 8, 7, 6});
    // Optional number is authoritative, even if the display name disagrees.
    expectOrder({{1, "same", "app", "100", 2}, {2, "same", "app", "1", 10}},
                "", {0, 1});
}

void testTiesAndDeterminism() {
    const std::vector<Candidate> candidates = {
        {30, "beta", "app", "", 2}, {21, "alpha", "z", "", 2},
        {22, "alpha", "a", "", 2}, {20, "Alpha", "a", "", 2},
        {9, "Alpha", "a", "", 2}, {1, "Alpha", "A", "", 2},
        {0, "Alpha", "A", "", 2}, {0, "Alpha", "A", "", 2}};
    expectOrder(candidates, "", {6, 7, 5, 4, 3, 2, 1, 0});
    const auto maxID = std::numeric_limits<std::uint64_t>::max();
    expectOrder({{maxID, "same", "app", "", 2}, {0, "same", "app", "", 2}},
                "same", {1, 0});

    std::vector<Candidate> shuffled = {
        {99, "same", "app", "", 2}, {0, "same", "app", "", 2},
        {maxID, "same", "app", "", 2}, {42, "same", "app", "", 2}};
    const std::vector<std::uint64_t> expectedIDs = {0, 42, 99, maxID};
    std::mt19937 random(0x51A8);
    for (int iteration = 0; iteration < 100; ++iteration) {
        std::shuffle(shuffled.begin(), shuffled.end(), random);
        const auto before = shuffled;
        const auto result = match(shuffled, "SAME");
        CHECK(result == match(shuffled, "SAME"));
        CHECK(result.size() == expectedIDs.size());
        for (std::size_t i = 0; i < result.size(); ++i) {
            CHECK(shuffled[result[i]].identity == expectedIDs[i]);
            CHECK(shuffled[i].identity == before[i].identity);
            CHECK(shuffled[i].title == before[i].title);
        }
    }
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"UTF-8 validation (all Unicode scalar values)", testUTF8},
        {"4096-byte limits and failure atomicity", testLimits},
        {"codepoint editing and ownership", testEditor},
        {"randomized editor/model comparison", testEditorModel},
        {"AND matching, fields, ASCII folding and Unicode", testMatching},
        {"relevance ordering", testRelevance},
        {"numeric and natural workspace ordering", testWorkspaceSorting},
        {"tie breakers and deterministic identities", testTiesAndDeterminism}};
    try {
        for (const auto& test : tests) {
            try {
                test.second();
            } catch (const std::exception& error) {
                std::cerr << "FAIL: " << test.first << ": " << error.what() << '\n';
                return 1;
            }
            std::cout << "PASS: " << test.first << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "All " << tests.size() << " test groups passed (" << checks << " checks).\n";
    return 0;
}
