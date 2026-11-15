// Visual theme for the UI toolkit: colors, font sizes and metrics (logical units, scaled by the UI
// scale at draw time) plus optional 9-slice skins. Widgets read everything look-related from here,
// so screens can be reskinned (e.g. with generated art) without touching widget code.
// makeDefaultTheme(): dark slate panels with warm gold accents, drawn procedurally (no images).
#pragma once

#include "client/core/Geometry.h"

namespace vo::client::render {
class Texture2D;
}

namespace vo::client::ui {

// Optional image skin. If `texture` is null the widget draws its procedural look.
struct NineSliceSkin
{
    const render::Texture2D* texture = nullptr;
    Insets srcBorder; // texture pixels
    Insets dstBorder; // logical units
    Color tint = colors::kWhite;
};

enum class TextRole : std::uint8_t
{
    Small,
    Normal,
    Large,
    Title,   // panel titles
    Heading, // screen titles
};

struct Theme
{
    // ---- Screen ----
    Color backgroundTop = Color::hex(0x1B222B);
    Color backgroundBottom = Color::hex(0x07090C);

    // ---- Panels ----
    Color panelTop = Color::hex(0x2B333D, 0.97f);
    Color panelBottom = Color::hex(0x1A1F26, 0.97f);
    Color panelBorder = Color::hex(0x7A6236);
    Color panelHighlight = Color::hex(0xFFFFFF, 0.06f);
    Color panelShadow = Color::hex(0x000000, 0.45f);
    Color titleBarTop = Color::hex(0x3A3426);
    Color titleBarBottom = Color::hex(0x262219);
    Color ornament = Color::hex(0xC9A247);
    Color insetFill = Color::hex(0x11151A, 0.85f);
    Color insetBorder = Color::hex(0x3C4450);

    // ---- Text ----
    Color text = Color::hex(0xE9E2CF);
    Color textDim = Color::hex(0xA39B88);
    Color textDisabled = Color::hex(0x6B675F);
    Color textTitle = Color::hex(0xF2CA64);
    Color placeholder = Color::hex(0x77736A);
    Color textShadow = Color::hex(0x000000, 0.7f);

    // ---- Accent ----
    Color accent = Color::hex(0xD9AE4A);
    Color accentBright = Color::hex(0xF5D27A);

    // ---- Buttons ----
    Color buttonTop = Color::hex(0x4A3F27);
    Color buttonBottom = Color::hex(0x2E2717);
    Color buttonHoverTop = Color::hex(0x5E5031);
    Color buttonHoverBottom = Color::hex(0x3B321D);
    Color buttonPressedTop = Color::hex(0x221D11);
    Color buttonPressedBottom = Color::hex(0x342C1A);
    Color buttonDisabledTop = Color::hex(0x2C2C2C);
    Color buttonDisabledBottom = Color::hex(0x222222);
    Color buttonBorder = Color::hex(0x9C7D3C);
    Color buttonBorderHover = Color::hex(0xE0B850);
    Color buttonText = Color::hex(0xF3E7C6);

    // ---- Inputs (text box, list, checkbox) ----
    Color inputFill = Color::hex(0x0E1216);
    Color inputFillHover = Color::hex(0x12171C);
    Color inputBorder = Color::hex(0x4A5361);
    Color inputBorderHover = Color::hex(0x6C7686);
    Color focusRing = Color::hex(0xE0B850);
    Color selection = Color::hex(0x8C6A2A, 0.75f);
    Color caret = Color::hex(0xF5D27A);
    Color rowHover = Color::hex(0x26303B);
    Color rowSelected = Color::hex(0x5B4824);
    Color rowSelectedText = Color::hex(0xFFF1CC);
    Color scrollTrack = Color::hex(0x000000, 0.35f);
    Color scrollThumb = Color::hex(0x8A7444);
    Color scrollThumbHover = Color::hex(0xB8974F);
    Color checkMark = Color::hex(0xF2C75C);

    // ---- Modal ----
    Color overlayDim = Color::hex(0x000000, 0.6f);

    // ---- Font sizes (logical px, em size) ----
    float fontSmall = 13.0f;
    float fontNormal = 15.0f;
    float fontLarge = 18.0f;
    float fontTitle = 19.0f;
    float fontHeading = 40.0f;

    // ---- Metrics (logical px) ----
    float cornerRadius = 5.0f;
    float borderWidth = 1.0f;
    float padding = 14.0f;
    float spacing = 8.0f;
    float controlHeight = 34.0f;
    float textPaddingX = 9.0f;
    float rowHeight = 26.0f;
    float titleBarHeight = 38.0f;
    float scrollbarWidth = 8.0f;
    float checkboxSize = 18.0f;
    float caretWidth = 1.0f;
    float focusRingWidth = 1.5f;

    // ---- Optional skins ----
    NineSliceSkin panelSkin;
    NineSliceSkin buttonSkin;
    NineSliceSkin inputSkin;

    [[nodiscard]] float fontSize(TextRole role) const noexcept
    {
        switch (role)
        {
        case TextRole::Small:
            return fontSmall;
        case TextRole::Large:
            return fontLarge;
        case TextRole::Title:
            return fontTitle;
        case TextRole::Heading:
            return fontHeading;
        case TextRole::Normal:
        default:
            return fontNormal;
        }
    }
};

[[nodiscard]] inline Theme makeDefaultTheme()
{
    return Theme{};
}

} // namespace vo::client::ui
