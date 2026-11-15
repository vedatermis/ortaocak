#include "client/ui/TextEditModel.h"

#include "client/core/Utf8.h"

namespace vo::client::ui {
namespace {

bool isControl(char32_t cp) noexcept
{
    return cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0);
}

bool isSpace(char32_t cp) noexcept
{
    return cp == ' ' || cp == 0xA0 || cp == '\t';
}

} // namespace

void TextEditModel::setMaxCodePoints(std::size_t max)
{
    m_maxCodePoints = max;
    if (max != 0 && m_codePoints > max)
    {
        m_text.resize(utf8::offsetOfIndex(m_text, max));
        m_codePoints = max;
        if (m_caret > m_text.size())
            m_caret = m_text.size();
        if (m_anchor > m_text.size())
            m_anchor = m_text.size();
        ++m_revision;
    }
}

std::string TextEditModel::clean(std::string_view input) const
{
    std::string out;
    out.reserve(input.size());
    for (std::size_t pos = 0; pos < input.size();)
    {
        const utf8::Decoded d = utf8::decode(input, pos);
        if (!d.valid)
            utf8::append(out, utf8::kReplacement);
        else if (!isControl(d.codePoint))
            out.append(input.substr(pos, d.length));
        pos += d.length;
    }
    return out;
}

void TextEditModel::setText(std::string_view text)
{
    m_text = clean(text);
    m_codePoints = utf8::countCodePoints(m_text);
    if (m_maxCodePoints != 0 && m_codePoints > m_maxCodePoints)
    {
        m_text.resize(utf8::offsetOfIndex(m_text, m_maxCodePoints));
        m_codePoints = m_maxCodePoints;
    }
    m_caret = m_anchor = m_text.size();
    ++m_revision;
}

std::string_view TextEditModel::selectedText() const noexcept
{
    return std::string_view(m_text).substr(selectionBegin(), selectionEnd() - selectionBegin());
}

void TextEditModel::eraseRange(std::size_t begin, std::size_t end)
{
    if (end <= begin)
        return;
    m_codePoints -= utf8::countCodePoints(std::string_view(m_text).substr(begin, end - begin));
    m_text.erase(begin, end - begin);
    m_caret = m_anchor = begin;
    ++m_revision;
}

bool TextEditModel::insert(std::string_view utf8Text)
{
    std::string piece = clean(utf8Text);
    if (piece.empty())
        return false; // nothing insertable (e.g. only control characters): leave the selection alone

    const std::size_t selectedCount = utf8::countCodePoints(selectedText());
    std::size_t pieceCount = utf8::countCodePoints(piece);
    if (m_maxCodePoints != 0)
    {
        const std::size_t available = m_maxCodePoints - (m_codePoints - selectedCount);
        if (pieceCount > available)
        {
            piece.resize(utf8::offsetOfIndex(piece, available));
            pieceCount = available;
        }
    }
    if (piece.empty())
        return false; // full: keep the selection rather than deleting it for nothing

    eraseRange(selectionBegin(), selectionEnd());
    m_text.insert(m_caret, piece);
    m_caret += piece.size();
    m_anchor = m_caret;
    m_codePoints += pieceCount;
    ++m_revision;
    return true;
}

bool TextEditModel::insertCodePoint(char32_t cp)
{
    char buf[4];
    const std::size_t n = utf8::encode(cp, buf);
    return insert(std::string_view(buf, n));
}

bool TextEditModel::deleteSelection()
{
    if (!hasSelection())
        return false;
    eraseRange(selectionBegin(), selectionEnd());
    return true;
}

bool TextEditModel::backspace()
{
    if (hasSelection())
        return deleteSelection();
    if (m_caret == 0)
        return false;
    eraseRange(utf8::prevBoundary(m_text, m_caret), m_caret);
    return true;
}

bool TextEditModel::deleteForward()
{
    if (hasSelection())
        return deleteSelection();
    if (m_caret >= m_text.size())
        return false;
    eraseRange(m_caret, utf8::nextBoundary(m_text, m_caret));
    return true;
}

bool TextEditModel::deleteWordBackward()
{
    if (hasSelection())
        return deleteSelection();
    if (m_caret == 0)
        return false;
    eraseRange(wordLeft(m_caret), m_caret);
    return true;
}

void TextEditModel::moveLeft(bool extend)
{
    if (hasSelection() && !extend)
        m_caret = selectionBegin();
    else
        m_caret = utf8::prevBoundary(m_text, m_caret);
    if (!extend)
        m_anchor = m_caret;
}

void TextEditModel::moveRight(bool extend)
{
    if (hasSelection() && !extend)
        m_caret = selectionEnd();
    else
        m_caret = utf8::nextBoundary(m_text, m_caret);
    if (!extend)
        m_anchor = m_caret;
}

std::size_t TextEditModel::wordLeft(std::size_t pos) const noexcept
{
    // Skip spaces to the left, then the word.
    while (pos > 0)
    {
        const std::size_t prev = utf8::prevBoundary(m_text, pos);
        if (!isSpace(utf8::decode(m_text, prev).codePoint))
            break;
        pos = prev;
    }
    while (pos > 0)
    {
        const std::size_t prev = utf8::prevBoundary(m_text, pos);
        if (isSpace(utf8::decode(m_text, prev).codePoint))
            break;
        pos = prev;
    }
    return pos;
}

std::size_t TextEditModel::wordRight(std::size_t pos) const noexcept
{
    // Skip the word, then the spaces after it (Windows edit control behaviour).
    while (pos < m_text.size() && !isSpace(utf8::decode(m_text, pos).codePoint))
        pos = utf8::nextBoundary(m_text, pos);
    while (pos < m_text.size() && isSpace(utf8::decode(m_text, pos).codePoint))
        pos = utf8::nextBoundary(m_text, pos);
    return pos;
}

void TextEditModel::moveWordLeft(bool extend)
{
    m_caret = wordLeft(m_caret);
    if (!extend)
        m_anchor = m_caret;
}

void TextEditModel::moveWordRight(bool extend)
{
    m_caret = wordRight(m_caret);
    if (!extend)
        m_anchor = m_caret;
}

void TextEditModel::moveHome(bool extend)
{
    m_caret = 0;
    if (!extend)
        m_anchor = m_caret;
}

void TextEditModel::moveEnd(bool extend)
{
    m_caret = m_text.size();
    if (!extend)
        m_anchor = m_caret;
}

void TextEditModel::selectAll()
{
    m_anchor = 0;
    m_caret = m_text.size();
}

void TextEditModel::setCaret(std::size_t offset, bool extend)
{
    m_caret = utf8::floorBoundary(m_text, offset);
    if (!extend)
        m_anchor = m_caret;
}

void TextEditModel::selectWordAt(std::size_t offset)
{
    std::size_t pos = utf8::floorBoundary(m_text, offset);
    if (pos >= m_text.size() && pos > 0)
        pos = utf8::prevBoundary(m_text, pos);
    if (m_text.empty())
    {
        m_caret = m_anchor = 0;
        return;
    }
    const bool space = isSpace(utf8::decode(m_text, pos).codePoint);
    std::size_t begin = pos;
    while (begin > 0)
    {
        const std::size_t prev = utf8::prevBoundary(m_text, begin);
        if (isSpace(utf8::decode(m_text, prev).codePoint) != space)
            break;
        begin = prev;
    }
    std::size_t end = pos;
    while (end < m_text.size() && isSpace(utf8::decode(m_text, end).codePoint) == space)
        end = utf8::nextBoundary(m_text, end);
    m_anchor = begin;
    m_caret = end;
}

} // namespace vo::client::ui
