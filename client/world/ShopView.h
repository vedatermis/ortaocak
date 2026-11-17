// Client-side merchant and potion logic (M14 "Pazar", docs/PROTOCOL_SHOP.md §8), no D3D, no sockets:
// everything here is pure and unit-tested (tests/client/ShopViewTests.cpp). The server stays
// authoritative (SHOP_*_RES, ITEM_USE_RES, ITEM_COOLDOWN, ITEM_USED); these helpers keep the NPCs in
// view, the open shop and its one request in flight, the shared potion cooldown, and produce the
// Turkish texts of §8.
//
//   NpcBook               the merchants in view (ENTITY_SPAWN of entity type 4 / ENTITY_DESPAWN), with
//                         their npcs.toml data; nearest() for E
//   ShopSession           the open shop (stock, prices), the open request, the buy / sell in flight
//   ConsumableCooldown    the shared 10 s potion cooldown (ITEM_COOLDOWN), sweep / remaining
//   QuantityModel         the quantity box ("Kaç tane?", 1..max stack, up / down, total)
//   shopEntryView()       how a stock entry is drawn (affordable, requirement in red)
//   buyNeedsConfirm() ... when a purchase / sale asks first, and its texts
//   shopResultText() ...  the result texts of §8.3
//   consumableTipLines()  the tooltip of a potion (§8.4)
//   escapeActionShop()    Esc: loot window, shop window, Tuşlar, Yetenekler, Künye, inventory, target, menu
//   parseShopCommand()    the shop `scene` automation commands
#pragma once

#include "client/world/Animator.h"
#include "client/world/LootView.h"
#include "client/world/WorldMath.h"
#include "shared/game/Items.h"
#include "shared/game/Npcs.h"
#include "shared/net/Messages.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vo::assets {
struct TerrainData;
}

namespace vo::client::world {

// ---- Presentation constants (§8.1, docs/SHOP_ART.md §1) ----

inline constexpr std::uint32_t kNpcNameRgb = 0x86E3D3;  // merchant name plate (not a monster / player colour)
inline constexpr std::uint32_t kNpcTitleRgb = 0xB9D9CF; // the title line under it
inline constexpr float kNpcMarkerLift = 0.70f;     // marker centre above the head top (pin 0.45 + half 0.25)
inline constexpr float kNpcMarkerWorldSize = 0.55f; // metres
inline constexpr float kNpcMarkerMinPx = 30.0f;     // logical pixels on screen
inline constexpr float kNpcMarkerMaxPx = 52.0f;
inline constexpr float kNpcMarkerFullAlpha = 40.0f; // the marker fades between these distances (m)
inline constexpr float kNpcMarkerHidden = 60.0f;
inline constexpr float kNpcWalkStop = 3.2f;       // walking to a merchant stops this close (talk range 4 m)
inline constexpr float kNpcInteractSearch = 6.0f; // E talks to a merchant this close ("next to one")
inline constexpr float kNpcTalkingRange = 4.0f;   // idle_talking while a player of ours is this close
// The client closes the shop window itself a little before the server would (8 m, §2.2).
inline constexpr float kShopAutoClose = 7.8f;

// ---- NPCs in view ----

struct NpcEntity
{
    std::uint32_t id = 0;     // entity id
    std::uint16_t npcId = 0;  // ENTITY_SPAWN template_id = npcs.toml id
    const game::NpcDef* def = nullptr; // nullptr: unknown to this client (drawn with the wire name only)
    std::string name;
    Vec3 position{};
    float yaw = 0.0f;
    float age = 0.0f;   // seconds since the spawn (fade-in)
    float interact = -1.0f; // seconds since the last purchase / sale animation started (< 0: none)
    Animator animator;
    const void* animatorMesh = nullptr;
    const void* animatorClips = nullptr;
    [[nodiscard]] std::string_view title() const noexcept { return def != nullptr ? std::string_view(def->title) : ""; }
    [[nodiscard]] bool sellsPotions() const noexcept;
};

class NpcBook
{
public:
    // ENTITY_SPAWN: false (nothing kept) unless entity type 4. A spawn of a known id replaces it. y
    // comes from `terrain` when given.
    bool spawn(const vo::net::EntitySpawn& m, const game::NpcCatalog& catalog, const assets::TerrainData* terrain);
    // ENTITY_DESPAWN: true if it was one of ours.
    bool despawn(std::uint32_t entityId);
    void clear() noexcept { m_npcs.clear(); }
    [[nodiscard]] NpcEntity* find(std::uint32_t entityId) noexcept;
    [[nodiscard]] const NpcEntity* find(std::uint32_t entityId) const noexcept;
    [[nodiscard]] std::span<NpcEntity> entities() noexcept { return m_npcs; }
    [[nodiscard]] std::span<const NpcEntity> entities() const noexcept { return m_npcs; }
    [[nodiscard]] std::size_t size() const noexcept { return m_npcs.size(); }
    // The nearest NPC within `maxDistance` (horizontal) of `from`; 0 if none. Ties: the lower id.
    [[nodiscard]] std::uint32_t nearest(Vec3 from, float maxDistance) const noexcept;
    // By name (automation): 0 if none.
    [[nodiscard]] std::uint32_t byName(std::string_view name) const noexcept;
    [[nodiscard]] std::uint32_t spawns() const noexcept { return m_spawns; }

private:
    std::vector<NpcEntity> m_npcs;
    std::uint32_t m_spawns = 0;
};

// ---- The open shop ----

struct ShopOffer
{
    std::uint16_t templateId = 0;
    std::uint32_t price = 0;
    bool operator==(const ShopOffer&) const = default;
};

class ShopSession
{
public:
    enum class Op : std::uint8_t
    {
        Buy,
        Sell,
    };
    struct Request
    {
        Op op = Op::Buy;
        std::uint32_t seq = 0;
        std::uint32_t npc = 0;
        std::uint16_t templateId = 0; // buy: what; sell: what the slot held when asked
        std::uint16_t quantity = 0;
        std::uint8_t bagSlot = 0;     // sell
    };

    // SHOP_OPEN_REQ sent for `npc` (replaces an earlier one still waiting).
    void beginOpen(std::uint32_t npc) noexcept { m_opening = npc; }
    [[nodiscard]] std::uint32_t opening() const noexcept { return m_opening; }
    // SHOP_OPEN_RES: false if it is not the answer to the open request (stale: ignored). On Ok the
    // shop of `npc` is open with its stock (another open one is replaced: the server closed it).
    bool finishOpen(const vo::net::ShopOpenRes& m);
    // The window closed (any path): the shop is forgotten; requests in flight stay (their answers
    // still come) unless `forgetRequests` (a zone change).
    void close(bool forgetRequests = false) noexcept;

    [[nodiscard]] bool isOpen() const noexcept { return m_npc != 0; }
    [[nodiscard]] std::uint32_t npc() const noexcept { return m_npc; }
    [[nodiscard]] std::span<const ShopOffer> offers() const noexcept { return m_offers; }
    [[nodiscard]] const ShopOffer* offer(std::uint16_t templateId) const noexcept;

    // One buy or sell in flight at a time (the window's buttons wait meanwhile).
    [[nodiscard]] bool busy() const noexcept { return m_request.has_value(); }
    [[nodiscard]] const std::optional<Request>& request() const noexcept { return m_request; }
    // The request to send, or nullopt when busy / not open.
    std::optional<vo::net::ShopBuyReq> beginBuy(std::uint16_t templateId, std::uint16_t quantity);
    std::optional<vo::net::ShopSellReq> beginSell(std::uint8_t bagSlot, std::uint16_t templateId, std::uint16_t quantity);
    // SHOP_BUY_RES / SHOP_SELL_RES: the request it answers (nullopt: unknown seq, ignored).
    std::optional<Request> finishBuy(const vo::net::ShopBuyRes& m);
    std::optional<Request> finishSell(const vo::net::ShopSellRes& m);
    // The request could not be sent.
    void abandon() noexcept { m_request.reset(); }

private:
    std::uint32_t m_opening = 0;
    std::uint32_t m_npc = 0;
    std::vector<ShopOffer> m_offers;
    std::optional<Request> m_request;
    std::uint32_t m_nextSeq = 1;
};

// ---- The shared consumable cooldown (§2.3, §8.4) ----

class ConsumableCooldown
{
public:
    // ITEM_COOLDOWN at local time `now` (seconds).
    void set(std::uint32_t remainingMs, std::uint32_t totalMs, double now) noexcept;
    void clear() noexcept { m_end = -1e9; }
    [[nodiscard]] bool active(double now) const noexcept { return now < m_end; }
    [[nodiscard]] double remaining(double now) const noexcept { return now < m_end ? m_end - now : 0.0; }
    // 0 ready .. 1 just started (for the sweep of the bar and bag icons).
    [[nodiscard]] float sweep(double now) const noexcept;
    [[nodiscard]] double end() const noexcept { return m_end; }

private:
    double m_end = -1e9;
    double m_total = 10.0;
};

// ---- Quantity box ----

struct QuantityModel
{
    std::uint16_t max = 1;
    std::uint16_t value = 1;
    QuantityModel() = default;
    QuantityModel(std::uint16_t maxValue, std::uint16_t initial) noexcept;
    void set(int v) noexcept; // clamped to 1..max
    void up(int step = 1) noexcept { set(static_cast<int>(value) + step); }
    void down(int step = 1) noexcept { set(static_cast<int>(value) - step); }
    [[nodiscard]] std::uint64_t total(std::uint32_t unit) const noexcept { return static_cast<std::uint64_t>(unit) * value; }
};

// ---- Stock entries ----

struct ShopEntryView
{
    bool affordable = true; // price <= balance (the price is red otherwise)
    std::string detail;     // second line: "Can +70" / "Saldırı +9 · Sv. 3" / "Yalnız Büyücü"
    bool detailUnmet = false; // the requirement is not met (red; it can still be bought)
};
[[nodiscard]] ShopEntryView shopEntryView(const game::ItemTemplate& item, std::uint32_t price, std::uint32_t coins,
                                          const game::ItemUser& user);
// "12 Akçe" (thousands grouped).
[[nodiscard]] std::string priceText(std::uint64_t akce);
// "Akçe: 1.234".
[[nodiscard]] std::string balanceText(std::uint32_t coins);

// ---- Buying / selling decisions (§8.2) ----

inline constexpr std::uint32_t kBuyConfirmAbove = 50;   // gear above this asks first
inline constexpr std::uint32_t kSellConfirmValue = 40;  // items worth this much (value per unit) ask first
// A stackable item asks for a quantity; gear buys one.
[[nodiscard]] bool buyNeedsQuantity(const game::ItemTemplate& item) noexcept;
// Gear (not stackable) costing more than kBuyConfirmAbove.
[[nodiscard]] bool buyNeedsConfirm(const game::ItemTemplate& item, std::uint64_t total) noexcept;
// A stack of more than one asks for a quantity (default the whole stack).
[[nodiscard]] bool sellNeedsQuantity(std::uint16_t stackQuantity) noexcept;
// Rarity above common or a value of kSellConfirmValue Akçe or more.
[[nodiscard]] bool sellNeedsConfirm(const game::ItemTemplate& item) noexcept;
// "<Ad> için <n> Akçe ödensin mi?"
[[nodiscard]] std::string buyConfirmText(std::string_view name, std::uint64_t total);
// "<Ad> <n> Akçe'ye satılsın mı?" (several: "<k> × <Ad> ...").
[[nodiscard]] std::string sellConfirmText(std::string_view name, std::uint16_t quantity, std::uint64_t gained);
inline constexpr std::string_view kBuyQuantityPrompt = "Kaç tane?";
inline constexpr std::string_view kSellQuantityPrompt = "Kaç tane satılsın?";
// "Toplam: <n> Akçe" / "Alınacak: <n> Akçe".
[[nodiscard]] std::string buyTotalText(std::uint64_t total);
[[nodiscard]] std::string sellTotalText(std::uint64_t gained);
inline constexpr std::string_view kUnequipFirstText = "Önce çıkarmalısın.";
inline constexpr std::string_view kSellHintText = "Satmak için çantada sağ tıkla ya da eşyayı buraya sürükle.";
inline constexpr std::string_view kBarOnlyPotionsText = "Çubuğa yalnız iksir konabilir.";
// Tooltip line of a bag item while a shop is open: "Satış: <n> Akçe" (per unit; a stack adds the
// total) or "Satılamaz".
[[nodiscard]] std::string sellTipText(const game::ItemTemplate& item, std::uint16_t quantity);

// ---- Texts (§8.3) ----

// The line for a refused SHOP_OPEN / BUY / SELL ("" for Ok). `forUse` false.
[[nodiscard]] std::string_view shopResultText(vo::net::ShopResult r) noexcept;
// "<Ad> alındı (<n> Akçe)." / "<n> × <Ad> alındı (<toplam> Akçe).".
[[nodiscard]] std::string buyOkText(std::string_view name, std::uint16_t quantity, std::uint32_t cost);
// "<Ad> satıldı (+<n> Akçe)." / "<n> × <Ad> satıldı (+<toplam> Akçe).".
[[nodiscard]] std::string sellOkText(std::string_view name, std::uint16_t quantity, std::uint32_t gained);
// SHOP_CLOSED: OutOfRange "<Ad> çok uzakta kaldı."; YouDied "".
[[nodiscard]] std::string shopClosedText(vo::net::ShopClosedReason r, std::string_view npcName);
// The hover tooltip of an NPC: "<Ad> — <Unvan>", "Konuşmak için tıkla".
[[nodiscard]] std::vector<std::string> npcTipLines(std::string_view name, std::string_view title);
// The title line of the name plate: "‹Şerbetçi›".
[[nodiscard]] std::string npcTitlePlate(std::string_view title);

// ---- Potions (§8.4) ----

// Units of a template in the bag (equipment does not hold consumables).
[[nodiscard]] std::uint32_t bagCount(const game::Inventory& inv, game::ItemTemplateId templateId) noexcept;
// Tooltip of a consumable: name (rarity colour), "Tüketilebilir", "Can: +<n>" / "Mana: +<n>", "Bekleme: 10 sn
// (bütün iksirler)", the description, "<n> / 20" (when `quantity` > 0 or `showZero`).
[[nodiscard]] std::vector<TipLine> consumableTipLines(const game::ItemTemplate& item, std::uint32_t quantity,
                                                      bool showZero = false);
// "Çubuktan kullan: <tuş>".
[[nodiscard]] std::string barUseText(std::string_view key);
// The floating text of ITEM_USED: "+70" (hp, green) / "+40" (mp, blue); both may come at once.
[[nodiscard]] std::string restoreText(std::uint32_t amount);

// ---- Input ----

// Esc (§8.2): loot window, shop window, Tuşlar, Yetenekler, Künye, inventory, target, menu.
[[nodiscard]] EscapeAction escapeActionShop(bool lootOpen, bool shopOpen, bool controlsOpen, bool skillsOpen,
                                            bool kunyeOpen, bool inventoryOpen, bool hasTarget, bool menuOpen) noexcept;

// ---- Automation (the shop `scene` commands of WorldScene) ----

enum class ShopVerb : std::uint8_t
{
    Talk,          // talk <npc name|nearest>: as a click on the merchant (walks into range, opens)
    OpenForce,     // shopopenforce <npc name>: SHOP_OPEN_REQ from where we stand (tests OutOfRange)
    WalkTo,        // walktonpc <npc name> [metres]: path-found walk to that far from it (default 2.5)
    Buy,           // shopbuy <template> [quantity]: as a click on the entry (+ quantity box / confirmation)
    BuyForce,      // shopbuyforce <template> <quantity>: the request without dialogs (refusal tests)
    Sell,          // shopsell <bagSlot> [quantity]: as a right click on the bag slot
    Close,         // shopclose: as the X
    ExpectOpen,    // expectshop <npc name>|none
    ExpectMessage, // expectshopmessage <text>: the last shop / potion line contains <text>
    ExpectCount,   // expectbarcount <slot 1-8> <n>: the count shown on a bar item slot
    ExpectCooldown,// expectpotioncd <minSec> <maxSec>
    Use,           // usepotion <template> [bagSlot]: as a double click in the bag (ITEM_USE_REQ)
    BarItem,       // baritem <slot 1-8> <template>|none: as a drop of a bag potion on the bar
    Hover,         // hovernpc <npc name>: the mouse over the merchant (tooltip / highlight)
    Log,           // shoplog
};

struct ShopCommand
{
    ShopVerb verb = ShopVerb::Log;
    std::string text;   // npc name, message
    std::uint32_t a = 0; // template / slot / count
    std::uint32_t b = 0; // quantity / bag slot / n
    bool hasB = false;
    double lo = 0.0;
    double hi = 0.0;
};

// nullopt with `error` empty: not a shop verb; nullopt with a message: a malformed shop command.
[[nodiscard]] std::optional<ShopCommand> parseShopCommand(std::string_view line, std::string& error);

} // namespace vo::client::world
