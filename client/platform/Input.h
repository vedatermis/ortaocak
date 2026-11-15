// Keyboard + mouse state and the per-frame input event queue.
//
// Window forwards WM_* messages to handleMessage(); automation injects synthetic events through
// inject(). Both paths update the same state (held keys/buttons, mouse position) and append to the
// same queue, so the UI cannot tell them apart. The frame loop reads events() and calls endFrame().
//
// Text input arrives as WM_CHAR UTF-16 units, combined into code points (surrogate pairs), so any
// keyboard layout (Turkish Q/F, AltGr combinations, dead keys, IME commits) works. Control
// characters are not reported as Char events; use KeyDown. Losing focus releases everything and
// queues a FocusLost event.
#pragma once

#include "client/platform/InputEvent.h"

#include <Windows.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace vo::client::platform {

class Input
{
public:
    Input();
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;

    // Returns true if the message was fully handled (`result` is the WndProc return value).
    bool handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& result);

    // Automation: applies the event to the state and queues it exactly like real input.
    void inject(const InputEvent& event);

    // When false, real keyboard/mouse/focus messages are ignored (used while a script drives the
    // client so a stray real mouse over the window cannot interfere).
    void setRealInputEnabled(bool enabled) noexcept { m_realInputEnabled = enabled; }

    [[nodiscard]] std::span<const InputEvent> events() const noexcept { return m_events; }
    // Clears the queue and per-frame edge flags. Call once per frame after everyone consumed events.
    void endFrame() noexcept;

    [[nodiscard]] bool isKeyDown(std::uint16_t key) const noexcept { return key < 256 && m_keys[key]; }
    [[nodiscard]] bool wasKeyPressed(std::uint16_t key) const noexcept { return key < 256 && m_pressed[key]; }
    [[nodiscard]] bool wasKeyReleased(std::uint16_t key) const noexcept
    {
        return key < 256 && m_released[key];
    }
    [[nodiscard]] bool isMouseDown(MouseButton b) const noexcept
    {
        return m_buttons[static_cast<std::size_t>(b)];
    }
    [[nodiscard]] int mouseX() const noexcept { return m_mouseX; }
    [[nodiscard]] int mouseY() const noexcept { return m_mouseY; }
    [[nodiscard]] std::uint8_t modifiers() const noexcept;

    // Releases all keys/buttons and queues FocusLost.
    void clearState();

private:
    void push(const InputEvent& event);
    void apply(const InputEvent& event);
    void updateCapture(HWND hwnd);

    std::vector<InputEvent> m_events;
    std::array<bool, 256> m_keys{};
    std::array<bool, 256> m_pressed{};
    std::array<bool, 256> m_released{};
    std::array<bool, 3> m_buttons{};
    int m_mouseX = -1;
    int m_mouseY = -1;
    char32_t m_pendingHighSurrogate = 0;
    bool m_trackingLeave = false;
    bool m_realInputEnabled = true;
    bool m_captured = false;
};

} // namespace vo::client::platform
