// Platform-neutral input event record consumed by the UI and scenes. Produced by platform::Input
// from Win32 messages and by the automation script player (same queue, same code path).
// Key codes are Win32 virtual-key values; the common ones are named in `keys`.
#pragma once

#include <cstdint>

namespace vo::client::platform {

enum class EventType : std::uint8_t
{
    KeyDown,    // key, repeat, mods
    KeyUp,      // key, mods
    Char,       // codePoint (printable text only; control characters arrive as KeyDown)
    MouseMove,  // x, y
    MouseDown,  // button, x, y, clickCount (2 = double click)
    MouseUp,    // button, x, y
    MouseWheel, // wheel (+1.0 per notch away from the user), x, y
    FocusLost,  // window lost keyboard focus; all keys/buttons were released
};

enum class MouseButton : std::uint8_t
{
    Left,
    Right,
    Middle,
};

namespace mods {
inline constexpr std::uint8_t kShift = 1u << 0;
inline constexpr std::uint8_t kCtrl = 1u << 1;
inline constexpr std::uint8_t kAlt = 1u << 2;
} // namespace mods

struct InputEvent
{
    EventType type = EventType::MouseMove;
    std::uint8_t mods = 0;
    MouseButton button = MouseButton::Left;
    bool repeat = false;
    std::uint8_t clickCount = 1;
    std::uint16_t key = 0;
    char32_t codePoint = 0;
    std::int32_t x = 0; // client-area pixels
    std::int32_t y = 0;
    float wheel = 0.0f;

    [[nodiscard]] bool shift() const noexcept { return (mods & mods::kShift) != 0; }
    // Ctrl without Alt: AltGr (reported as Ctrl+Alt) must not trigger Ctrl shortcuts.
    [[nodiscard]] bool ctrlOnly() const noexcept
    {
        return (mods & (mods::kCtrl | mods::kAlt)) == mods::kCtrl;
    }
    [[nodiscard]] bool alt() const noexcept { return (mods & mods::kAlt) != 0; }
};

namespace keys {
inline constexpr std::uint16_t kBackspace = 0x08;
inline constexpr std::uint16_t kTab = 0x09;
inline constexpr std::uint16_t kEnter = 0x0D;
inline constexpr std::uint16_t kShift = 0x10;
inline constexpr std::uint16_t kControl = 0x11;
inline constexpr std::uint16_t kAlt = 0x12;
inline constexpr std::uint16_t kPause = 0x13;
inline constexpr std::uint16_t kCapsLock = 0x14;
inline constexpr std::uint16_t kEscape = 0x1B;
inline constexpr std::uint16_t kSpace = 0x20;
inline constexpr std::uint16_t kPageUp = 0x21;
inline constexpr std::uint16_t kPageDown = 0x22;
inline constexpr std::uint16_t kEnd = 0x23;
inline constexpr std::uint16_t kHome = 0x24;
inline constexpr std::uint16_t kLeft = 0x25;
inline constexpr std::uint16_t kUp = 0x26;
inline constexpr std::uint16_t kRight = 0x27;
inline constexpr std::uint16_t kDown = 0x28;
inline constexpr std::uint16_t kInsert = 0x2D;
inline constexpr std::uint16_t kDelete = 0x2E;
inline constexpr std::uint16_t k0 = 0x30;  // '0'..'9' = 0x30..0x39
inline constexpr std::uint16_t kA = 0x41;  // 'A'..'Z' = 0x41..0x5A
inline constexpr std::uint16_t kF1 = 0x70; // F1..F12 = 0x70..0x7B
inline constexpr std::uint16_t kLShift = 0xA0;
inline constexpr std::uint16_t kRShift = 0xA1;
inline constexpr std::uint16_t kLControl = 0xA2;
inline constexpr std::uint16_t kRControl = 0xA3;
inline constexpr std::uint16_t kLAlt = 0xA4;
inline constexpr std::uint16_t kRAlt = 0xA5;

[[nodiscard]] constexpr std::uint16_t letter(char c) noexcept
{
    return static_cast<std::uint16_t>(kA + (c >= 'a' ? c - 'a' : c - 'A'));
}
} // namespace keys

} // namespace vo::client::platform
