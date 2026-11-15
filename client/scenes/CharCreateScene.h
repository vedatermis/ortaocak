// Character creation in two steps.
//   1. Race: two large cards (Aydor / Kargan) with portrait/placeholder, name and lore. Left/Right
//      or a click selects, double click / Enter / "İleri" continues, "Geri" / Esc goes back to the
//      character select screen.
//   2. Details: class (Savaşçı, Haydut, Büyücü, Şifacı) with a one-line description in the chosen
//      race's words (docs/LORE.md), the "Görünüm n/N" look selector (◀ ▶, complete character looks
//      of the race; hidden while the race has only one), name box with live rule hints, the
//      CharacterPreview, "Yarat" and "Geri". The look index goes out as CHAR_CREATE_REQ.face
//      (hair = 0).
// The first character also sees the Közaran oath line on the race step.
// CHAR_CREATE_RES failures are shown with a specific Turkish message; success returns to the select
// screen with the new character selected.
//
// Automation: states "race" | "details", "race.<aydor|kargan>", "looks<N>" (looks of the race),
// "look<n>" (current look index); `scene setlook <n>` (picks look n if the race has it, otherwise
// logs and keeps the current one), `scene expectpreviewmesh` (the 3D preview shows the mesh the look
// table resolves for the current race/look).
#pragma once

#include "client/scenes/UiScene.h"

#include <array>
#include <cstdint>

namespace vo::client::ui {
class Button;
class Label;
class StackPanel;
class TextBox;
class Widget;
} // namespace vo::client::ui
namespace vo::client::world {
class CharacterLookTable;
}

namespace vo::client::scenes {

class CharacterPreview;
class NameHints;
class RaceCard;
class Spinner;
class ToggleButton;

class CharCreateScene final : public UiScene
{
public:
    struct Options
    {
        bool firstCharacter = false; // the account has no characters yet (came here automatically)
    };

    explicit CharCreateScene(Options options);
    ~CharCreateScene() override;

    [[nodiscard]] const char* name() const noexcept override { return "charcreate"; }
    bool automationCommand(std::string_view command, std::string& error) override;

protected:
    void build() override;
    void onUnhandledKey(const platform::InputEvent& e) override;
    void onNetEvent(const net::NetEvent& e) override;
    [[nodiscard]] bool isBusy() const override { return m_creating; }
    void extraStates(std::vector<std::string>& out) const override;

private:
    enum class Step : std::uint8_t
    {
        Race,
        Details,
    };

    void buildRaceStep(ui::Widget& root);
    void buildDetailsStep(ui::Widget& root);
    void showStep(Step step);
    void selectRace(std::uint8_t race);
    void selectClass(std::uint8_t cls);
    void stepLook(int delta);
    [[nodiscard]] const world::CharacterLookTable* lookTable() const;
    [[nodiscard]] int lookCount() const;
    void onNameChanged();
    void updatePreview();
    void submit();
    void back();
    void setStatus(std::string_view text, bool error = false);

    Options m_options;
    Step m_step = Step::Race;
    std::uint8_t m_race = 0;
    std::uint8_t m_class = 1;
    std::uint8_t m_look = 0; // look index (wire `face`)
    bool m_creating = false;

    ui::Widget* m_raceStep = nullptr;
    ui::Widget* m_detailsStep = nullptr;
    std::array<RaceCard*, 2> m_raceCards{};
    ui::Button* m_next = nullptr;
    ui::Label* m_header = nullptr;
    ui::Label* m_subheader = nullptr;
    ui::Label* m_oath = nullptr;

    CharacterPreview* m_preview = nullptr;
    ui::Label* m_raceLine = nullptr;
    std::array<ToggleButton*, 4> m_classButtons{};
    ui::Label* m_classDesc = nullptr;
    ui::StackPanel* m_lookRow = nullptr;
    ui::Label* m_lookLabel = nullptr;
    ui::TextBox* m_name = nullptr;
    NameHints* m_hints = nullptr;
    ui::Button* m_create = nullptr;
    ui::Label* m_status = nullptr;
    Spinner* m_spinner = nullptr;
};

} // namespace vo::client::scenes
