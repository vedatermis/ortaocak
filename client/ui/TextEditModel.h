// Editing model behind TextBox: UTF-8 text, caret and selection anchor as byte offsets that always
// sit on code point boundaries, max length in code points. No rendering, no clipboard access (the
// widget moves strings in and out), so it is unit-testable.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace vo::client::ui {

class TextEditModel
{
public:
    TextEditModel() = default;
    explicit TextEditModel(std::size_t maxCodePoints) : m_maxCodePoints(maxCodePoints) {}

    // 0 = unlimited. Existing text longer than the limit is truncated.
    void setMaxCodePoints(std::size_t max);
    [[nodiscard]] std::size_t maxCodePoints() const noexcept { return m_maxCodePoints; }

    // Replaces the content (sanitised: invalid UTF-8 -> U+FFFD, control characters dropped,
    // truncated to the limit); caret goes to the end, selection is cleared.
    void setText(std::string_view text);
    [[nodiscard]] const std::string& text() const noexcept { return m_text; }
    [[nodiscard]] bool empty() const noexcept { return m_text.empty(); }
    [[nodiscard]] std::size_t codePointCount() const noexcept { return m_codePoints; }

    [[nodiscard]] std::size_t caret() const noexcept { return m_caret; }
    [[nodiscard]] std::size_t anchor() const noexcept { return m_anchor; }
    [[nodiscard]] bool hasSelection() const noexcept { return m_caret != m_anchor; }
    [[nodiscard]] std::size_t selectionBegin() const noexcept
    {
        return m_caret < m_anchor ? m_caret : m_anchor;
    }
    [[nodiscard]] std::size_t selectionEnd() const noexcept
    {
        return m_caret < m_anchor ? m_anchor : m_caret;
    }
    [[nodiscard]] std::string_view selectedText() const noexcept;

    // Incremented on every text change (not on caret moves); lets views cache derived data.
    [[nodiscard]] std::uint64_t revision() const noexcept { return m_revision; }

    // Inserts at the caret, replacing the selection. Input is sanitised like setText(); if the
    // result would exceed the limit, only the code points that fit are inserted. Returns true if
    // the text changed.
    bool insert(std::string_view utf8);
    bool insertCodePoint(char32_t cp);

    bool backspace();     // deletes the selection, or the code point before the caret
    bool deleteForward(); // deletes the selection, or the code point after the caret
    bool deleteSelection();
    bool deleteWordBackward(); // Ctrl+Backspace

    // `extend` keeps the anchor (Shift held); otherwise a selection collapses to the moved caret.
    void moveLeft(bool extend);
    void moveRight(bool extend);
    void moveWordLeft(bool extend);
    void moveWordRight(bool extend);
    void moveHome(bool extend);
    void moveEnd(bool extend);
    void selectAll();
    void clearSelection() noexcept { m_anchor = m_caret; }
    // Places the caret at a byte offset (clamped and snapped down to a code point boundary).
    void setCaret(std::size_t offset, bool extend);
    // Selects the word (or run of spaces) containing the byte offset.
    void selectWordAt(std::size_t offset);

private:
    [[nodiscard]] std::string clean(std::string_view input) const;
    void eraseRange(std::size_t begin, std::size_t end);
    [[nodiscard]] std::size_t wordLeft(std::size_t pos) const noexcept;
    [[nodiscard]] std::size_t wordRight(std::size_t pos) const noexcept;

    std::string m_text;
    std::size_t m_caret = 0;
    std::size_t m_anchor = 0;
    std::size_t m_codePoints = 0;
    std::size_t m_maxCodePoints = 0;
    std::uint64_t m_revision = 0;
};

} // namespace vo::client::ui
