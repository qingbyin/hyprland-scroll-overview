#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace WindowSearch {

inline constexpr std::size_t MAX_QUERY_BYTES = 4096;

struct Candidate {
    std::uint64_t               identity = 0;
    std::string                 title;
    std::string                 appID;
    std::string                 workspaceName;
    std::optional<std::int64_t>  workspaceNumber;
};

// Accepts empty text and Unicode scalar values, excluding C0/C1 controls
// (U+0000..001F and U+007F..009F). Rejects malformed UTF-8 and >4096 bytes.
// This is not Unicode normalization, case folding, or grapheme segmentation.
bool validQuery(std::string_view query) noexcept;

// Splits on Unicode White_Space (control whitespace is rejected first).
// Every word must be a substring of at least one field, including the signed
// decimal workspaceNumber, if present. Only ASCII A..Z are case-insensitive;
// all other bytes are compared literally. Candidate strings are not validated.
//
// Relevance is the sum of each word's lowest matching cost:
//   exact field: 0..3; prefix: 4..7; other substring: 8..11,
// with field order title, appID, workspaceName, workspaceNumber. Lower wins.
// Repeated words count repeatedly. Empty/whitespace-only queries match all.
// Ties: numbered workspaces first, ascending signed number; when both numbers
// are absent, natural workspaceName order (ASCII-folded, numeric runs compared
// without integer conversion). Then ASCII-folded title, ASCII-folded appID,
// raw title, raw appID, identity, original index. Digit runs equal in value
// prefer fewer leading zeroes. The original index only resolves identical keys.
// Returns indices into candidates, never pointers or copies of candidates.
// Throws std::invalid_argument for any invalid query, even with no candidates.
std::vector<std::size_t> match(const std::vector<Candidate>& candidates,
                               std::string_view query);

// cursor() is a byte offset, always at a codepoint boundary. Movement/deletion
// operates on one codepoint, not one displayed grapheme (e.g. combining marks
// and emoji sequences remain separate codepoints).
// All supplied text must satisfy validQuery; the complete resulting text
// must also fit MAX_QUERY_BYTES. Invalid input throws std::invalid_argument
// and leaves text/cursor unchanged. set puts the cursor at the end.
class Editor {
  public:
    explicit Editor(std::string_view text = {});
    Editor(const Editor&) = default;
    Editor& operator=(const Editor&) = default;
    // Moved-from editors are empty, with cursor zero, and remain usable.
    Editor(Editor&& other) noexcept;
    Editor& operator=(Editor&& other) noexcept;

    const std::string& text() const noexcept;
    std::size_t cursor() const noexcept;

    void set(std::string_view text);
    void insert(std::string_view text);

    // Return false at the corresponding boundary, without changing state.
    bool left() noexcept;
    bool right() noexcept;
    bool backspace();
    bool eraseForward();
    void home() noexcept;
    void end() noexcept;

  private:
    std::string m_text;
    std::size_t m_cursor = 0;
};

} // namespace WindowSearch
