#include "WindowSearch.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

namespace WindowSearch {
namespace {

unsigned char byteAt(std::string_view text, std::size_t offset) noexcept {
    return static_cast<unsigned char>(text[offset]);
}

bool nextCodepoint(std::string_view text, std::size_t& offset,
                   std::uint32_t& codepoint) noexcept {
    if (offset >= text.size())
        return false;

    const auto lead = byteAt(text, offset);
    std::size_t count = 0;
    std::uint32_t minimum = 0;
    if (lead <= 0x7F) {
        codepoint = lead;
        ++offset;
        return true;
    } else if (lead >= 0xC2 && lead <= 0xDF) {
        count = 2;
        minimum = 0x80;
        codepoint = lead & 0x1F;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        count = 3;
        minimum = 0x800;
        codepoint = lead & 0x0F;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        count = 4;
        minimum = 0x10000;
        codepoint = lead & 0x07;
    } else {
        return false;
    }

    if (text.size() - offset < count)
        return false;
    for (std::size_t i = 1; i < count; ++i) {
        const auto byte = byteAt(text, offset + i);
        if ((byte & 0xC0) != 0x80)
            return false;
        codepoint = (codepoint << 6) | (byte & 0x3F);
    }
    if (codepoint < minimum || codepoint > 0x10FFFF ||
        (codepoint >= 0xD800 && codepoint <= 0xDFFF))
        return false;

    offset += count;
    return true;
}

void requireValidQuery(std::string_view query) {
    if (!validQuery(query))
        throw std::invalid_argument(
            "WindowSearch: expected at most 4096 bytes of valid UTF-8 without C0/C1 controls");
}

bool isWhitespace(std::uint32_t codepoint) noexcept {
    // Unicode White_Space, excluding C0/C1 controls rejected by validation.
    return codepoint == 0x20 || codepoint == 0xA0 || codepoint == 0x1680 ||
           (codepoint >= 0x2000 && codepoint <= 0x200A) ||
           codepoint == 0x2028 || codepoint == 0x2029 || codepoint == 0x202F ||
           codepoint == 0x205F || codepoint == 0x3000;
}

std::string foldASCII(std::string_view text) {
    std::string result(text);
    for (char& character : result) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte >= 'A' && byte <= 'Z')
            character = static_cast<char>(byte + ('a' - 'A'));
    }
    return result;
}

std::vector<std::string> words(std::string_view query) {
    std::vector<std::string> result;
    std::size_t start = 0;
    std::size_t offset = 0;
    while (offset < query.size()) {
        const auto previous = offset;
        std::uint32_t codepoint = 0;
        // The caller has already validated the entire query.
        nextCodepoint(query, offset, codepoint);
        if (isWhitespace(codepoint)) {
            if (previous > start)
                result.push_back(foldASCII(query.substr(start, previous - start)));
            start = offset;
        }
    }
    if (start < query.size())
        result.push_back(foldASCII(query.substr(start)));
    return result;
}

bool isDigit(unsigned char byte) noexcept {
    return byte >= '0' && byte <= '9';
}

// Inputs are already ASCII-folded. Compare arbitrary-length decimal runs
// without parsing into an integer, so even huge workspace names cannot overflow.
int naturalCompare(std::string_view left, std::string_view right) noexcept {
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < left.size() && j < right.size()) {
        if (isDigit(byteAt(left, i)) && isDigit(byteAt(right, j))) {
            const auto leftStart = i;
            const auto rightStart = j;
            while (i < left.size() && isDigit(byteAt(left, i)))
                ++i;
            while (j < right.size() && isDigit(byteAt(right, j)))
                ++j;
            auto leftSignificant = leftStart;
            auto rightSignificant = rightStart;
            while (leftSignificant < i && left[leftSignificant] == '0')
                ++leftSignificant;
            while (rightSignificant < j && right[rightSignificant] == '0')
                ++rightSignificant;
            const auto leftLength = i - leftSignificant;
            const auto rightLength = j - rightSignificant;
            if (leftLength != rightLength)
                return leftLength < rightLength ? -1 : 1;
            const auto comparison = left.substr(leftSignificant, leftLength)
                                        .compare(right.substr(rightSignificant, rightLength));
            if (comparison != 0)
                return comparison < 0 ? -1 : 1;
            if (i - leftStart != j - rightStart)
                return i - leftStart < j - rightStart ? -1 : 1;
        } else {
            const auto a = byteAt(left, i++);
            const auto b = byteAt(right, j++);
            if (a != b)
                return a < b ? -1 : 1;
        }
    }
    if (i == left.size() && j == right.size())
        return 0;
    return i == left.size() ? -1 : 1;
}

std::size_t previousBoundary(std::string_view text, std::size_t cursor) noexcept {
    --cursor;
    while (cursor > 0 && (byteAt(text, cursor) & 0xC0) == 0x80)
        --cursor;
    return cursor;
}

std::size_t nextBoundary(std::string_view text, std::size_t cursor) noexcept {
    ++cursor;
    while (cursor < text.size() && (byteAt(text, cursor) & 0xC0) == 0x80)
        ++cursor;
    return cursor;
}

} // namespace

bool validQuery(std::string_view query) noexcept {
    if (query.size() > MAX_QUERY_BYTES)
        return false;
    std::size_t offset = 0;
    while (offset < query.size()) {
        std::uint32_t codepoint = 0;
        if (!nextCodepoint(query, offset, codepoint) || codepoint <= 0x1F ||
            (codepoint >= 0x7F && codepoint <= 0x9F))
            return false;
    }
    return true;
}

std::vector<std::size_t> match(const std::vector<Candidate>& candidates,
                               std::string_view query) {
    requireValidQuery(query);
    const auto terms = words(query);
    struct Match {
        std::size_t index;
        std::size_t cost;
        std::string title;
        std::string appID;
        std::string workspaceName;
    };
    std::vector<Match> matches;
    matches.reserve(candidates.size());
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const auto& candidate = candidates[index];
        std::array<std::string, 4> fields = {
            foldASCII(candidate.title), foldASCII(candidate.appID),
            foldASCII(candidate.workspaceName),
            candidate.workspaceNumber ? std::to_string(*candidate.workspaceNumber) : std::string{}};
        std::size_t cost = 0;
        bool matched = true;
        for (const auto& term : terms) {
            std::size_t best = 12;
            for (std::size_t field = 0; field < fields.size(); ++field) {
                const auto position = fields[field].find(term);
                if (position == std::string::npos)
                    continue;
                const std::size_t kind = fields[field].size() == term.size() ? 0 :
                                         position == 0 ? 1 : 2;
                best = std::min(best, kind * 4 + field);
            }
            if (best == 12) {
                matched = false;
                break;
            }
            cost += best;
        }
        if (matched)
            matches.push_back({index, cost, std::move(fields[0]),
                               std::move(fields[1]), std::move(fields[2])});
    }

    std::sort(matches.begin(), matches.end(), [&](const Match& left, const Match& right) {
        if (left.cost != right.cost)
            return left.cost < right.cost;
        const auto& a = candidates[left.index];
        const auto& b = candidates[right.index];
        if (a.workspaceNumber.has_value() != b.workspaceNumber.has_value())
            return a.workspaceNumber.has_value();
        if (a.workspaceNumber && a.workspaceNumber != b.workspaceNumber)
            return *a.workspaceNumber < *b.workspaceNumber;
        if (!a.workspaceNumber) {
            const auto order = naturalCompare(left.workspaceName, right.workspaceName);
            if (order != 0)
                return order < 0;
        }
        if (left.title != right.title)
            return left.title < right.title;
        if (left.appID != right.appID)
            return left.appID < right.appID;
        if (a.title != b.title)
            return a.title < b.title;
        if (a.appID != b.appID)
            return a.appID < b.appID;
        if (a.identity != b.identity)
            return a.identity < b.identity;
        return left.index < right.index;
    });

    std::vector<std::size_t> result;
    result.reserve(matches.size());
    for (const auto& match : matches)
        result.push_back(match.index);
    return result;
}

Editor::Editor(std::string_view text) {
    set(text);
}

Editor::Editor(Editor&& other) noexcept
    : m_text(std::move(other.m_text)), m_cursor(other.m_cursor) {
    other.m_text.clear();
    other.m_cursor = 0;
}

Editor& Editor::operator=(Editor&& other) noexcept {
    if (this != &other) {
        m_text = std::move(other.m_text);
        m_cursor = other.m_cursor;
        other.m_text.clear();
        other.m_cursor = 0;
    }
    return *this;
}

const std::string& Editor::text() const noexcept {
    return m_text;
}

std::size_t Editor::cursor() const noexcept {
    return m_cursor;
}

void Editor::set(std::string_view text) {
    requireValidQuery(text);
    std::string replacement(text);
    m_text.swap(replacement);
    m_cursor = m_text.size();
}

void Editor::insert(std::string_view text) {
    requireValidQuery(text);
    if (text.size() > MAX_QUERY_BYTES - m_text.size())
        throw std::invalid_argument("WindowSearch: insertion exceeds 4096 bytes");
    if (text.empty())
        return;
    // Copy before modifying, including when the input aliases our own text.
    std::string replacement(m_text);
    replacement.insert(m_cursor, text.data(), text.size());
    m_text.swap(replacement);
    m_cursor += text.size();
}

bool Editor::left() noexcept {
    if (m_cursor == 0)
        return false;
    m_cursor = previousBoundary(m_text, m_cursor);
    return true;
}

bool Editor::right() noexcept {
    if (m_cursor == m_text.size())
        return false;
    m_cursor = nextBoundary(m_text, m_cursor);
    return true;
}

bool Editor::backspace() {
    if (m_cursor == 0)
        return false;
    const auto previous = previousBoundary(m_text, m_cursor);
    m_text.erase(previous, m_cursor - previous);
    m_cursor = previous;
    return true;
}

bool Editor::eraseForward() {
    if (m_cursor == m_text.size())
        return false;
    m_text.erase(m_cursor, nextBoundary(m_text, m_cursor) - m_cursor);
    return true;
}

void Editor::home() noexcept {
    m_cursor = 0;
}

void Editor::end() noexcept {
    m_cursor = m_text.size();
}

} // namespace WindowSearch
