// Character select screen: requests the character list on entry and shows three slots (a character
// card or "Boş Slot — Karakter Yarat") next to a large CharacterPreview. Up/Down (or the mouse)
// selects, Enter / double click / "Oyuna Gir" enters the world, "Karakter Yarat" opens creation,
// "Sil" asks for confirmation by typing the character's name, "Çıkış Yap" logs out (Esc asks
// first). An account without characters goes straight to creation when autoCreateWhenEmpty is set.
// A dropped connection returns to the login screen with the reason.
#pragma once

#include "client/scenes/CharacterSlots.h"
#include "client/scenes/UiScene.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace vo::client::ui {
class Button;
class Label;
class Panel;
} // namespace vo::client::ui

namespace vo::client::scenes {

class CharacterPreview;
class CharacterSlotCard;
class Spinner;

class CharSelectScene final : public UiScene
{
public:
    struct Options
    {
        bool autoCreateWhenEmpty = false;          // jump to creation if the list is empty
        std::optional<std::uint32_t> selectCharId; // select this character once the list arrives
        std::string status;                        // initial status line (e.g. "X yaratıldı.")
    };

    explicit CharSelectScene(Options options);
    ~CharSelectScene() override;

    [[nodiscard]] const char* name() const noexcept override { return "charselect"; }

protected:
    void build() override;
    void onUnhandledKey(const platform::InputEvent& e) override;
    void onNetEvent(const net::NetEvent& e) override;
    [[nodiscard]] bool isBusy() const override { return m_pending != Pending::None; }
    void extraStates(std::vector<std::string>& out) const override;

private:
    enum class Pending : std::uint8_t
    {
        None,
        List,
        Enter,
        Delete,
    };

    void requestList();
    void refresh();
    void setPending(Pending pending, std::string_view status = {});
    void setStatus(std::string_view text, bool error = false);
    void selectSlot(int index);
    void activateSlot(int index);
    void enterWorld();
    void openCreate();
    void askDelete();
    void sendDelete(std::uint32_t charId, std::string name);
    void logout();
    void syncAccount();

    Options m_options;
    CharacterSlots m_slots;
    Pending m_pending = Pending::None;
    bool m_listLoaded = false;
    std::string m_deletingName;

    ui::Panel* m_listPanel = nullptr;
    std::array<CharacterSlotCard*, CharacterSlots::kSlotCount> m_cards{};
    CharacterPreview* m_preview = nullptr;
    ui::Button* m_enter = nullptr;
    ui::Button* m_create = nullptr;
    ui::Button* m_delete = nullptr;
    ui::Button* m_logout = nullptr;
    ui::Label* m_status = nullptr;
    Spinner* m_spinner = nullptr;
};

} // namespace vo::client::scenes
