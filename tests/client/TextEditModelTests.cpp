#include "client/ui/TextEditModel.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using vo::client::ui::TextEditModel;

TEST_CASE("TextEditModel inserts Turkish text at the caret", "[client][textedit]")
{
    TextEditModel m;
    REQUIRE(m.insert("şğ"));
    REQUIRE(m.text() == "şğ");
    REQUIRE(m.caret() == 4);
    m.moveLeft(false);
    REQUIRE(m.caret() == 2);
    REQUIRE(m.insertCodePoint(U'İ'));
    REQUIRE(m.text() == "şİğ");
    REQUIRE(m.codePointCount() == 3);
    REQUIRE(m.caret() == 4);
}

TEST_CASE("TextEditModel backspace/delete remove whole code points", "[client][textedit]")
{
    TextEditModel m;
    m.setText("çöı");
    REQUIRE(m.caret() == m.text().size());
    REQUIRE(m.backspace());
    REQUIRE(m.text() == "çö");
    m.moveHome(false);
    REQUIRE(m.deleteForward());
    REQUIRE(m.text() == "ö");
    REQUIRE(m.codePointCount() == 1);
    m.moveEnd(false);
    REQUIRE_FALSE(m.deleteForward());
    m.moveHome(false);
    REQUIRE_FALSE(m.backspace());
}

TEST_CASE("TextEditModel shift selection, replace and selected text", "[client][textedit]")
{
    TextEditModel m;
    m.setText("Güneş");
    m.moveHome(false);
    m.moveRight(true);
    m.moveRight(true);
    REQUIRE(m.hasSelection());
    REQUIRE(m.selectedText() == "Gü");
    m.moveLeft(true);
    REQUIRE(m.selectedText() == "G");
    m.moveEnd(true);
    REQUIRE(m.selectedText() == "Güneş");
    // Typing replaces the selection.
    REQUIRE(m.insert("Ay"));
    REQUIRE(m.text() == "Ay");
    REQUIRE_FALSE(m.hasSelection());

    // Arrow without Shift collapses to the selection edge.
    m.setText("abcd");
    m.selectAll();
    m.moveLeft(false);
    REQUIRE(m.caret() == 0);
    REQUIRE_FALSE(m.hasSelection());
    m.selectAll();
    m.moveRight(false);
    REQUIRE(m.caret() == 4);
}

TEST_CASE("TextEditModel cut/copy/paste style operations without a clipboard", "[client][textedit]")
{
    TextEditModel m;
    m.setText("çok güzel şey");
    m.setCaret(0, false);
    m.moveWordRight(true); // selects "çok "
    REQUIRE(m.selectedText() == "çok ");
    const std::string copied(m.selectedText()); // "copy"
    REQUIRE(m.deleteSelection());               // "cut"
    REQUIRE(m.text() == "güzel şey");
    m.moveEnd(false);
    REQUIRE(m.insert(" " + copied)); // "paste"
    REQUIRE(m.text() == "güzel şey çok ");
}

TEST_CASE("TextEditModel enforces max length in code points", "[client][textedit]")
{
    TextEditModel m(5);
    REQUIRE(m.insert("ğğğ"));
    REQUIRE(m.insert("ıııı")); // only two fit
    REQUIRE(m.text() == "ğğğıı");
    REQUIRE(m.codePointCount() == 5);
    REQUIRE_FALSE(m.insertCodePoint(U'x'));

    // Replacing a selection frees room.
    m.moveLeft(true);
    m.moveLeft(true);
    REQUIRE(m.insert("xyz")); // replacing 2 selected code points frees room for 2
    REQUIRE(m.text() == "ğğğxy");

    m.setText("abcdefgh");
    REQUIRE(m.text() == "abcde");
    m.setMaxCodePoints(3);
    REQUIRE(m.text() == "abc");
    REQUIRE(m.caret() <= m.text().size());
}

TEST_CASE("TextEditModel sanitises input", "[client][textedit]")
{
    TextEditModel m;
    REQUIRE(m.insert("a\tb\nc\x01"));
    REQUIRE(m.text() == "abc");
    REQUIRE(m.insert("\xFF"));
    REQUIRE(m.text() == "abc\xEF\xBF\xBD");
    REQUIRE_FALSE(m.insert("\n\r"));
}

TEST_CASE("TextEditModel word navigation and deletion", "[client][textedit]")
{
    TextEditModel m;
    m.setText("İstanbul çok güzel");
    m.moveWordLeft(false);
    REQUIRE(m.text().substr(m.caret()) == "güzel");
    m.moveWordLeft(false);
    REQUIRE(m.text().substr(m.caret()) == "çok güzel");
    m.moveWordRight(false);
    REQUIRE(m.text().substr(m.caret()) == "güzel");
    m.moveEnd(false);
    REQUIRE(m.deleteWordBackward());
    REQUIRE(m.text() == "İstanbul çok ");

    m.setText("ağaç dalı");
    m.selectWordAt(2); // inside "ağaç"
    REQUIRE(m.selectedText() == "ağaç");
    m.selectWordAt(m.text().size()); // at the end -> last word
    REQUIRE(m.selectedText() == "dalı");
}

TEST_CASE("TextEditModel setCaret snaps to code point boundaries", "[client][textedit]")
{
    TextEditModel m;
    m.setText("aşb");
    m.setCaret(2, false); // middle of 'ş'
    REQUIRE(m.caret() == 1);
    m.setCaret(100, true);
    REQUIRE(m.caret() == 4);
    REQUIRE(m.selectedText() == "şb");
}

TEST_CASE("TextEditModel revision changes only on edits", "[client][textedit]")
{
    TextEditModel m;
    const auto r0 = m.revision();
    m.insert("x");
    const auto r1 = m.revision();
    REQUIRE(r1 != r0);
    m.moveLeft(false);
    m.selectAll();
    REQUIRE(m.revision() == r1);
}
