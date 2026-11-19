// Quest windows of the world scene (M17, docs/PROTOCOL_QUESTS.md §7): the NPC dialog, the quest log, the
// HUD tracker and the marker over an NPC. The rules and texts are in client/world/QuestView.h; this file only
// draws them and reports clicks through hooks (WorldScene wires them to the network, client/scenes/WorldQuests.cpp).
//
// QuestDialog (left side, placed by the window arranger like the shop): the NPC's name in the title bar, its
// title and greeting, then either the list of its quests (a status gem, the name, a status word; a click
// opens one) or one quest: its name, the NPC's text (the offer, the "not yet" line or the hand-in line),
// "Hedefler" with the progress, "Ödüller" (experience, Akçe, the items with icons and tooltips; a choice row
// whose items are picked by a click), and the buttons Kabul et / Vazgeç, Teslim et / Vazgeç or Kapat. A merchant
// also has "Alışveriş" (back to its shop). The body scrolls with the wheel when it is taller than the window.
// Widget names (automation): quest.dialog, quest.accept, quest.handin, quest.decline, quest.back, quest.shop,
// quest.close, quest.text (invisible: the shown texts).
//
// QuestLogWindow (L or the HUD button "Görevler"): the active quests (oldest first; a click selects one), the
// selected quest's objectives, its text and who takes it back, "Bırak" (asks first). Names: quest.log,
// quest.log.abandon, quest.log.close, quest.log.text.
//
// QuestTracker (HUD, under the zone name on the right): up to five active quests with their objectives
// ("Kül Çömezi 3/8"; done ones green with a tick); hidden while a window covers it. Names: quest.tracker.
//
// drawQuestMarker(): the original marker over an NPC (a faceted köz gem on a stem; three looks).
#pragma once

#include "client/core/Geometry.h"
#include "client/ui/Panel.h"
#include "client/world/QuestView.h"

#include <functional>
#include <string>
#include <vector>

namespace vo::client::render {
class SpriteBatch;
}
namespace vo::client::ui {
class Button;
class Label;
struct DrawContext;
} // namespace vo::client::ui
namespace vo::client::world {
class ClientInventory;
}

namespace vo::client::scenes {

class SceneResources;

// The marker: centre of the gem at `center`, `size` = its height in physical pixels, `time` for the pulse.
void drawQuestMarker(render::SpriteBatch& batch, Vec2 center, float size, game::QuestMarker marker, double time,
                     float alpha = 1.0f);

// What the quest body (dialog detail / log) shows for one quest.
struct QuestBodyView
{
    const game::QuestDef* quest = nullptr;
    game::QuestStatus status = game::QuestStatus::Hidden;
    const game::QuestRecord* record = nullptr; // active: the progress
    std::uint8_t charClass = 0;
    std::string npcText;     // the giver's / turn-in NPC's words shown above the objectives
    std::string footnote;    // e.g. "Teslim: Avcı Selvi" (log), "Seviye 5'te alınabilir" (dialog)
    int selectedChoice = -1; // the picked choice item (-1 none)
};

class QuestDialog final : public ui::Panel
{
public:
    static constexpr float kWidth = 360.0f; // logical
    static constexpr float kHeight = 450.0f; // the tallest it gets (less on a small screen: the body scrolls)

    struct Hooks
    {
        std::function<void(game::QuestId)> select; // a list row
        std::function<void()> back;                // to the list
        std::function<void(game::QuestId)> accept;
        std::function<void(game::QuestId, std::uint8_t choice)> handIn;
        std::function<void()> decline; // Vazgeç / Kapat: the dialog closes
        std::function<void()> shop;    // a merchant: its shop instead
        std::function<void(int)> choose; // a choice item picked (index)
    };

    QuestDialog(const SceneResources& resources, const world::ClientInventory& inventory, Hooks hooks);

    struct View
    {
        std::string npcName;
        std::string npcTitle;
        std::string greeting;
        bool merchant = false;
        std::vector<world::NpcQuestEntry> entries; // the list (when no quest is selected)
        QuestBodyView body;                        // the selected quest (body.quest null: the list)
        bool busy = false;                         // a request is in flight (the buttons wait)
        std::string message;
        bool messageError = false;
    };
    void setView(View v);
    [[nodiscard]] const View& view() const noexcept { return m_view; }
    void setMaxHeight(float logical);
    [[nodiscard]] float preferredHeight() const noexcept;
    // The shown texts as one string (automation probe quest.text).
    [[nodiscard]] std::string plainText() const;

    void drawSelf(ui::DrawContext& ctx) override;
    void draw(ui::DrawContext& ctx) override;
    bool onMouseDown(const platform::InputEvent& e) override;
    bool onMouseMove(const platform::InputEvent& e) override;
    bool onMouseWheel(const platform::InputEvent& e) override;

private:
    void relayout();
    Vec2 m_mouse{-1.0f, -1.0f}; // physical: the cursor over the window (hover rows, tooltips)

    const SceneResources& m_resources;
    const world::ClientInventory& m_inventory;
    Hooks m_hooks;
    View m_view;
    float m_maxHeight = 0.0f;
    float m_scroll = 0.0f;  // physical pixels the body is scrolled
    float m_bodyH = 0.0f;   // physical height of the body content (last draw)
    ui::Button* m_accept = nullptr;
    ui::Button* m_handIn = nullptr;
    ui::Button* m_decline = nullptr;
    ui::Button* m_back = nullptr;
    ui::Button* m_shop = nullptr;
    ui::Label* m_probe = nullptr;
    struct Hit
    {
        Rect rect;
        game::QuestId quest = 0; // list row
        std::uint16_t item = 0;  // reward icon (tooltip)
        int choice = -1;         // choice item
    };
    std::vector<Hit> m_hits; // physical, last draw
};

class QuestLogWindow final : public ui::Panel
{
public:
    static constexpr float kWidth = 380.0f;
    static constexpr float kHeight = 430.0f;

    struct Hooks
    {
        std::function<void(game::QuestId)> select;
        std::function<void(game::QuestId)> abandon;
        std::function<void()> close;
    };
    QuestLogWindow(const SceneResources& resources, const world::ClientInventory& inventory, Hooks hooks);

    struct Row
    {
        game::QuestId quest = 0;
        std::string name;
        bool ready = false;
    };
    struct View
    {
        std::vector<Row> rows;  // active quests
        QuestBodyView body;     // the selected one (quest null: none)
        std::size_t limit = 10; // "3 / 10"
        bool busy = false;
    };
    void setView(View v);
    [[nodiscard]] const View& view() const noexcept { return m_view; }
    void setMaxHeight(float logical);
    [[nodiscard]] float preferredHeight() const noexcept;
    [[nodiscard]] std::string plainText() const;

    void drawSelf(ui::DrawContext& ctx) override;
    void draw(ui::DrawContext& ctx) override;
    bool onMouseDown(const platform::InputEvent& e) override;
    bool onMouseMove(const platform::InputEvent& e) override;
    bool onMouseWheel(const platform::InputEvent& e) override;

private:
    Vec2 m_mouse{-1.0f, -1.0f};
    const SceneResources& m_resources;
    const world::ClientInventory& m_inventory;
    Hooks m_hooks;
    View m_view;
    float m_maxHeight = 0.0f;
    float m_scroll = 0.0f;
    float m_bodyH = 0.0f;
    ui::Button* m_abandon = nullptr;
    ui::Label* m_probe = nullptr;
    struct Hit
    {
        Rect rect;
        game::QuestId quest = 0;
        std::uint16_t item = 0;
    };
    std::vector<Hit> m_hits;
};

class QuestTracker final : public ui::Widget
{
public:
    static constexpr float kWidth = 250.0f; // logical

    struct Entry
    {
        std::string name;
        std::vector<world::ObjectiveLine> lines;
        bool ready = false;
        std::string turnIn; // ready: "→ Avcı Selvi"
    };
    QuestTracker();
    void setEntries(std::vector<Entry> entries);
    [[nodiscard]] const std::vector<Entry>& entries() const noexcept { return m_entries; }
    // Logical height of the current entries.
    [[nodiscard]] float contentHeight() const noexcept;
    [[nodiscard]] std::string plainText() const; // one line per row ("Kül Tozu: Kül Tozu 2/5")
    void drawSelf(ui::DrawContext& ctx) override;

private:
    std::vector<Entry> m_entries;
};

} // namespace vo::client::scenes
