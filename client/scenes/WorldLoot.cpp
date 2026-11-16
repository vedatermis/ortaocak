// WorldScene, loot half (M10 "Ganimet", docs/PROTOCOL_LOOT.md): loot containers in the world, the
// loot window, takes, the Akçe balance and pickup feedback. The pure rules live in
// client/world/LootView.h; this file wires them to the network, the input and the screen.
#include "client/scenes/WorldScene.h"

#include "client/net/GameClient.h"
#include "client/platform/Input.h"
#include "client/render/AssetCache.h"
#include "client/render/TextRenderer.h"
#include "client/render/WorldRenderer.h"
#include "client/scenes/InventoryWindow.h"
#include "client/scenes/LootWindow.h"
#include "client/scenes/SceneResources.h"
#include "client/ui/Ui.h"
#include "client/world/ItemData.h"
#include "shared/core/Log.h"
#include "shared/game/TerrainWalk.h"
#include "shared/net/Messages.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace vo::client::scenes {

namespace anchor = ui::anchor;
namespace vn = vo::net;
using world::Vec3;

namespace {

constexpr const char* kBagMesh = "props/loot_bag.vmesh";
constexpr const char* kCoinIcon = "ui/items/coin.png";
constexpr float kBagScale = 1.3f;          // the 0.46 m pouch reads better a little larger at game distance
constexpr float kPickupTime = 3.6f;         // seconds a pickup line stays
constexpr std::size_t kMaxPickups = 5;
constexpr float kPickupLogBottom = 92.0f;   // logical: the log ends this far above the screen bottom
constexpr float kPickupLogTop = 212.0f;     // ... and starts at most this far (5 lines): the loot window stays above
constexpr double kLootWindow = 1.0;         // the server's 10/s window ...
constexpr std::size_t kLootPerWindow = 8;   // ... we stay below it
constexpr std::uint32_t kCoinRgb = 0xF2CA64;

std::array<float, 4> rgba(std::uint32_t rgb, float a)
{
    return {static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
            static_cast<float>(rgb & 0xFF) / 255.0f, a};
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------------------------

void WorldScene::requestLootAssets()
{
    render::AssetCache* assets = ctx().assets;
    if (assets == nullptr)
        return;
    assets->request(render::AssetCache::Kind::Mesh, kBagMesh);
    (void)ctx().resources.uiImage(kCoinIcon); // tolerated if missing (placeholder)
}

void WorldScene::buildLootWindow()
{
    LootWindow::Hooks hooks;
    hooks.take = [this](std::uint8_t what) { requestLootTake(what); };
    hooks.takeAll = [this]() { requestLootTake(vn::kLootTakeAll); };
    hooks.close = [this]() { closeLootWindow(true, {}); };
    m_lootWindow = &ui().root().add<LootWindow>(ctx().resources, m_loot, m_inventory, std::move(hooks));
    m_lootWindow->setAnchors(anchor::kTopLeft).setMargins({14.0f, 232.0f, 0.0f, 0.0f});
    m_lootWindow->setVisible(false);
    m_loot.close();
    m_lootPendingOpen = 0;
    m_lootWalk = 0;
    m_lootHover = 0;
    m_coins = 0;
    m_coinsKnown = false;
    m_pickups.clear();
    m_lootSeen = {};
    m_lootSends.fill(-1e9);
    m_lootLayoutTop = -1.0f;
    m_lastLootMessage.clear();
}

void WorldScene::layoutLootWindow()
{
    if (m_lootWindow == nullptr || !m_lootWindow->isVisible())
        return;
    // Left side, below the HUD panel (and the F3 info panel when shown); moved up (over the info
    // panel, never over the HUD panel) if it would reach the pickup log at the bottom left.
    const float s = std::max(ui().scale(), 0.01f);
    const float logicalH = ctx().viewportHeight / s;
    float top = m_showInfo ? 232.0f : 124.0f;
    const float h = m_lootWindow->preferredHeight();
    top = std::max(124.0f, std::min(top, logicalH - kPickupLogTop - h));
    if (std::fabs(top - m_lootLayoutTop) > 0.5f)
    {
        m_lootLayoutTop = top;
        m_lootWindow->setMargins({14.0f, top, 0.0f, 0.0f});
    }
}

// ---------------------------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------------------------

bool WorldScene::lootBudget() const
{
    std::size_t recent = 0;
    for (const double t : m_lootSends)
        recent += (m_time - t) < kLootWindow ? 1 : 0;
    return recent < kLootPerWindow;
}

world::LootLabel WorldScene::lootLabelOf(const world::RemoteEntity& e) const
{
    return world::lootLabel(e.state);
}

std::string WorldScene::lootSourceText(const world::RemoteEntity& e) const
{
    return std::format("{} · Sv. {}", e.name, e.level);
}

void WorldScene::showLootMessage(std::string_view text, bool error)
{
    if (text.empty())
        return;
    m_lastLootMessage = std::string(text);
    if (m_lootWindow != nullptr && m_loot.isOpen())
        m_lootWindow->setMessage(std::string(text), error);
    showCombatMessage(text); // the bottom-centre message line, also when the window is closed
}

bool WorldScene::sendLootOpen(std::uint32_t lootId)
{
    if (m_leavingWorld || m_model.leaving() || leaving() || ctx().net.movesBlocked())
        return false;
    if (!lootBudget())
    {
        showLootMessage("Çok hızlı: biraz bekle", true);
        return false;
    }
    if (!ctx().net.send(vn::LootOpenReq{lootId}))
        return false;
    m_lootSends[m_lootSendNext] = m_time;
    m_lootSendNext = (m_lootSendNext + 1) % m_lootSends.size();
    m_lootPendingOpen = lootId;
    VO_LOG_INFO("world: LOOT_OPEN_REQ {}", lootId);
    return true;
}

void WorldScene::requestLootOpen(std::uint32_t lootId)
{
    const world::RemoteEntity* e = m_model.find(lootId);
    if (e == nullptr || !e->isLoot())
        return;
    if (m_self.dead)
    {
        showLootMessage(world::lootResultText(vn::LootResult::YouAreDead), true);
        return;
    }
    if (lootLabelOf(*e) == world::LootLabel::Locked)
        VO_LOG_INFO("world: bag {} is someone else's for now (the server decides)", lootId);
    const float d = world::horizontalDistance(e->position, m_model.local().mover().position());
    if (d <= world::kLootWalkStop + 0.3f)
    {
        m_lootWalk = 0;
        (void)sendLootOpen(lootId);
        return;
    }
    // Walk there first (the same way an attack walks into range), then open.
    if (m_localAnim.seated())
        m_localAnim.standUp();
    m_chasing = false;
    m_queuedSkill = 0;
    m_lootWalk = lootId;
    m_lootWalkReplan = 0.0f;
    VO_LOG_INFO("world: walking to bag {} ({:.1f} m away) to open it", lootId, d);
}

void WorldScene::requestLootTake(std::uint8_t what)
{
    if (!m_loot.isOpen() || m_leavingWorld || m_model.leaving() || leaving() || ctx().net.movesBlocked())
        return;
    if (m_self.dead)
    {
        showLootMessage(world::lootResultText(vn::LootResult::YouAreDead), true);
        return;
    }
    if (what == vn::kLootTakeAll ? m_loot.empty() : !m_loot.row(what))
        return; // nothing listed there (any more)
    if (!lootBudget() || m_loot.takesInFlight() >= vn::kMaxLootTakesInFlight)
    {
        showLootMessage("Çok hızlı: biraz bekle", true);
        return;
    }
    vn::LootTakeReq req;
    req.lootId = m_loot.lootId();
    req.what = what;
    req.seq = m_loot.beginTake(what);
    if (!ctx().net.send(req))
    {
        (void)m_loot.finishTake(req.seq);
        return;
    }
    m_inventory.beginTake(req.seq); // its ITEM_UPDATEs settle no item move
    m_lootSends[m_lootSendNext] = m_time;
    m_lootSendNext = (m_lootSendNext + 1) % m_lootSends.size();
    VO_LOG_INFO("world: LOOT_TAKE_REQ seq {} bag {} what {}", req.seq, req.lootId,
                what == vn::kLootTakeAll ? std::string("all")
                : what == vn::kLootTakeCoins ? std::string("coins")
                                             : std::to_string(what));
}

void WorldScene::closeLootWindow(bool sendClose, std::string_view message)
{
    const std::uint32_t id = m_loot.lootId();
    if (id != 0 && sendClose && !m_leavingWorld && !m_model.leaving() && !leaving() && !ctx().net.movesBlocked())
    {
        // Not counted against the budget on purpose: a close must always go out (it is one request).
        (void)ctx().net.send(vn::LootCloseReq{id});
        VO_LOG_INFO("world: LOOT_CLOSE_REQ {}", id);
    }
    m_loot.close();
    m_lootPendingOpen = 0;
    if (m_lootWindow != nullptr)
        m_lootWindow->setVisible(false);
    if (!message.empty())
        showLootMessage(message, false);
}

void WorldScene::lootInteract(bool includeLocked)
{
    if (m_phase != Phase::Ready || m_menuOpen)
        return;
    if (m_loot.isOpen())
    {
        requestLootTake(vn::kLootTakeAll);
        return;
    }
    std::vector<world::LootCandidate> c;
    for (const world::RemoteEntity& e : m_model.entities())
        if (e.active && e.isLoot())
            c.push_back({e.id, e.position, e.state});
    const Vec3 me = m_model.local().mover().position();
    const std::uint32_t id = world::nearestLoot(c, me, world::kLootInteractSearch, includeLocked);
    if (id != 0)
    {
        requestLootOpen(id);
        return;
    }
    if (world::nearestLoot(c, me, world::kLootInteractSearch, true) != 0)
        showLootMessage(world::lootResultText(vn::LootResult::NotYours), true);
    else
        showLootMessage("Yakında ganimet yok", false);
}

void WorldScene::addPickup(std::string text, std::uint32_t rgb)
{
    VO_LOG_INFO("world: pickup: {}", text);
    m_pickups.push_back({std::move(text), rgb, 0.0f});
    if (m_pickups.size() > kMaxPickups)
        m_pickups.erase(m_pickups.begin());
}

void WorldScene::notePickups(const game::Inventory& before)
{
    const world::ItemData* data = ctx().resources.itemData();
    const game::ItemCatalog* catalog = data != nullptr && data->catalogLoaded ? &data->catalog : nullptr;
    for (const world::ItemGain& g : world::inventoryGains(before, m_inventory.confirmed()))
    {
        const game::ItemTemplate* t = catalog != nullptr ? catalog->find(g.templateId) : nullptr;
        addPickup(world::pickupText(t, g.templateId, g.units), t != nullptr ? world::rarityRgb(t->rarity) : 0xEDE6D3);
        ++m_lootSeen.pickups;
        if (m_farmTemplate != 0 && g.templateId == m_farmTemplate)
            ++m_farmSeen;
    }
}

bool WorldScene::farmLootStep()
{
    if (m_loot.isOpen())
    {
        // Take everything; the server closes the window once the bag is empty.
        if (m_loot.takesInFlight() == 0 && m_loot.hasContents() && !m_loot.empty())
            requestLootTake(vn::kLootTakeAll);
        m_huntNextTry = m_time + 0.5;
        return true;
    }
    if (m_lootWalk != 0 || m_lootPendingOpen != 0 || m_loot.takesInFlight() > 0 || m_inventory.takesPending() > 0)
    {
        m_huntNextTry = m_time + 0.3;
        return true;
    }
    std::vector<world::LootCandidate> c;
    for (const world::RemoteEntity& e : m_model.entities())
        if (e.active && e.isLoot() && std::find(m_farmSkip.begin(), m_farmSkip.end(), e.id) == m_farmSkip.end())
            c.push_back({e.id, e.position, e.state});
    const std::uint32_t id = world::nearestLoot(c, m_model.local().mover().position(), 15.0f, false);
    if (id == 0)
        return false;
    requestLootOpen(id);
    m_huntNextTry = m_time + 0.5;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------------------------

bool WorldScene::handleLootMessage(const net::NetEvent& e)
{
    const auto op = static_cast<vn::Opcode>(e.opcode);
    switch (op)
    {
    case vn::Opcode::LootOpenRes:
    case vn::Opcode::LootContents:
    case vn::Opcode::LootTakeRes:
    case vn::Opcode::LootClosed:
    case vn::Opcode::Coins:
        break;
    default:
        return false;
    }
    const auto malformed = [&]() {
        VO_LOG_ERROR("world: malformed {} payload ({} bytes)", vn::opcodeName(op), e.payload.size());
        returnToLogin(net::connectFailureMessage(vn::ConnectResult::ProtocolError));
        return true;
    };
    const auto refreshWindow = [&]() {
        if (m_lootWindow == nullptr || !m_loot.isOpen())
            return;
        const world::RemoteEntity* bag = m_model.find(m_loot.lootId());
        m_lootWindow->refresh(bag != nullptr ? lootSourceText(*bag) : std::string(),
                              bag != nullptr ? lootLabelOf(*bag) : world::LootLabel::Free);
        layoutLootWindow();
    };
    switch (op)
    {
    case vn::Opcode::LootOpenRes:
    {
        vn::LootOpenRes m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        VO_LOG_INFO("world: LOOT_OPEN_RES {} {}", m.lootId, vn::lootResultName(m.result));
        if (m.lootId != m_lootPendingOpen)
            return true; // a stale answer (another bag was asked for since)
        m_lootPendingOpen = 0;
        if (m.result != vn::LootResult::Ok)
        {
            ++m_lootSeen.refused;
            showLootMessage(world::lootResultText(m.result), true);
            if (m_farmTemplate != 0)
                m_farmSkip.push_back(m.lootId);
            return true;
        }
        ++m_lootSeen.opened;
        if (m_self.dead || m_model.find(m.lootId) == nullptr)
            return true;
        m_loot.open(m.lootId); // replaces the view of another bag (the server ended it silently)
        if (m_lootWindow != nullptr)
        {
            m_lootWindow->setVisible(true);
            m_lootWindow->setMessage("", false);
        }
        refreshWindow();
        return true;
    }
    case vn::Opcode::LootContents:
    {
        vn::LootContents m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        if (!m_loot.applyContents(m))
        {
            VO_LOG_DEBUG("world: LOOT_CONTENTS for bag {} ignored (not the open one)", m.lootId);
            return true;
        }
        VO_LOG_INFO("world: LOOT_CONTENTS {}: {} Akçe, {} entr(y/ies)", m.lootId, m.coins, m.entries.size());
        refreshWindow();
        return true;
    }
    case vn::Opcode::LootTakeRes:
    {
        vn::LootTakeRes m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        m_inventory.endTake(m.seq);
        const auto take = m_loot.finishTake(m.seq);
        ++m_lootSeen.takes;
        VO_LOG_INFO("world: LOOT_TAKE_RES seq {} bag {}: {}{}", m.seq, m.lootId, vn::lootResultName(m.result),
                    take ? "" : " (unknown seq)");
        if (m.result != vn::LootResult::Ok)
        {
            ++m_lootSeen.refused;
            showLootMessage(world::lootResultText(m.result), true);
            if (m_farmTemplate != 0 && m.lootId == m_loot.lootId())
            {
                m_farmSkip.push_back(m.lootId); // lootfarm: do not keep asking for it
                closeLootWindow(true, {});
            }
        }
        return true;
    }
    case vn::Opcode::LootClosed:
    {
        vn::LootClosed m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        VO_LOG_INFO("world: LOOT_CLOSED {} reason {}", m.lootId, static_cast<int>(m.reason));
        if (m.lootId == m_loot.lootId())
        {
            ++m_lootSeen.closedByServer;
            closeLootWindow(false, world::lootClosedText(m.reason));
        }
        return true;
    }
    case vn::Opcode::Coins:
    {
        vn::Coins m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        VO_LOG_INFO("world: COINS {} (+{})", m.coins, m.gained);
        m_coins = m.coins;
        m_coinsKnown = true;
        if (m_invWindow != nullptr)
            m_invWindow->setCoins(m_coins, true);
        if (m.gained > 0)
        {
            addPickup(world::coinPickupText(m.gained), kCoinRgb);
            ++m_lootSeen.coinGains;
        }
        return true;
    }
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------------------------
// Per frame
// ---------------------------------------------------------------------------------------------

void WorldScene::updateLoot(float dt)
{
    for (Pickup& p : m_pickups)
        p.age += dt;
    std::erase_if(m_pickups, [](const Pickup& p) { return p.age >= kPickupTime; });

    world::LocalMover& mover = m_model.local().mover();
    const Vec3 me = mover.position();

    // Hover: the bag under the cursor (not while the mouse is over a window or turning the camera).
    m_lootHover = 0;
    if (!m_dragging && !m_menuOpen && (ui().hover() == nullptr || ui().hover() == &ui().root() ||
                                       ui().hover() == m_combatHud))
    {
        const std::uint32_t hit = pickAt(ctx().input.mouseX(), ctx().input.mouseY());
        const world::RemoteEntity* e = hit != 0 ? m_model.find(hit) : nullptr;
        if (e != nullptr && e->isLoot())
            m_lootHover = hit;
    }

    // Walking to a bag to open it.
    if (m_lootWalk != 0)
    {
        const world::RemoteEntity* bag = m_model.find(m_lootWalk);
        if (bag == nullptr || m_self.dead)
        {
            m_lootWalk = 0;
        }
        else
        {
            const float d = world::horizontalDistance(bag->position, me);
            if (d <= world::kLootWalkStop)
            {
                const std::uint32_t id = m_lootWalk;
                m_lootWalk = 0;
                m_route.clear();
                m_routeNext = 0;
                mover.clearDestination();
                (void)sendLootOpen(id);
            }
            else
            {
                m_lootWalkReplan -= dt;
                if (m_lootWalkReplan <= 0.0f && !m_localAnim.seated())
                {
                    m_lootWalkReplan = 1.0f;
                    const Vec3 p = mover.position();
                    if (m_terrain == nullptr ||
                        game::isSegmentWalkable(*m_terrain, p.x, p.z, bag->position.x, bag->position.z))
                    {
                        m_route.clear();
                        m_routeNext = 0;
                        mover.setDestination(bag->position.x, bag->position.z);
                    }
                    else if (!routePending())
                    {
                        std::string ignored;
                        if (!setRouteNear(p, bag->position.x, bag->position.z, 2.0f, ignored))
                            mover.setDestination(bag->position.x, bag->position.z);
                    }
                }
            }
        }
    }

    // The open window follows its bag: gone (despawned / left the view) or walked away from.
    if (m_loot.isOpen())
    {
        const world::RemoteEntity* bag = m_model.find(m_loot.lootId());
        if (bag == nullptr)
            closeLootWindow(false, {});
        else if (world::horizontalDistance(bag->position, me) > world::kLootAutoClose)
            closeLootWindow(true, world::lootClosedText(vn::LootClosedReason::OutOfRange));
        else if (m_lootWindow != nullptr)
            layoutLootWindow();
    }
}

// ---------------------------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------------------------

void WorldScene::addLootDraw(const world::RemoteEntity& e)
{
    render::WorldRenderer& r = *ctx().worldRenderer;
    const world::LootLabelStyle st = world::lootLabelStyle(lootLabelOf(e));
    const bool hovered = e.id == m_lootHover;
    const bool open = e.id == m_loot.lootId();
    const float fadeIn = std::clamp(e.age / 0.35f, 0.0f, 1.0f);
    if (const render::GpuMesh* mesh = ctx().assets->mesh(kBagMesh))
    {
        // Settles onto the ground as it appears; turned by its id so neighbouring bags differ.
        const float drop = 0.25f * (1.0f - fadeIn) * (1.0f - fadeIn);
        const float yaw = static_cast<float>(e.id % 360u) * world::kPi / 180.0f;
        const world::Mat4 w = world::makeWorld(e.position + Vec3{0.0f, drop, 0.0f}, yaw, kBagScale);
        const float tint = st.meshTint * (hovered ? 1.35f : 1.0f);
        r.addEffectMesh(mesh, w, {tint, tint, tint});
    }
    if (m_terrain == nullptr)
        return;
    const Vec3 ground{e.position.x, m_terrain->heightAt(e.position.x, e.position.z), e.position.z};
    const Vec3 normal = m_terrain->normalAt(e.position.x, e.position.z);
    render::DecalDraw blob;
    blob.center = ground;
    blob.normal = normal;
    blob.radius = 0.42f * kBagScale;
    blob.color = {0.0f, 0.0f, 0.0f, 0.4f * fadeIn};
    r.addDecal(blob);
    float alpha = st.ringAlpha * fadeIn;
    float radius = 0.62f;
    if (st.ringPulse)
    {
        const float breathe = 0.5f + 0.5f * std::sin(static_cast<float>(m_time) * 3.2f + static_cast<float>(e.id));
        alpha *= 0.55f + 0.45f * breathe;
        radius += 0.06f * breathe;
    }
    if (hovered || open)
    {
        alpha = std::min(1.0f, alpha + 0.35f);
        radius += 0.08f;
    }
    if (alpha > 0.01f)
    {
        render::DecalDraw ring;
        ring.center = ground;
        ring.normal = normal;
        ring.radius = radius;
        ring.color = rgba(st.ringRgb, alpha);
        ring.ring = true;
        ring.phase = 0.0f;
        r.addDecal(ring);
    }
}

void WorldScene::drawLootHud()
{
    if (m_pickups.empty() || m_phase != Phase::Ready || m_menuOpen)
        return;
    auto& batch = ctx().batch;
    const float s = ui().scale();
    const float h = ctx().viewportHeight;
    const render::Font& f = ctx().fonts.get(render::FontWeight::Bold, 16.0f * s);
    // A short log at the bottom left, above the experience bar and the help lines (clear of the
    // skill button, the character and the windows' controls): newest at the bottom, each line slides
    // in from the left, holds and fades.
    const float left = std::round(18.0f * s);
    const float bottom = h - std::round(kPickupLogBottom * s);
    const float lineH = std::round(f.lineHeight() + 3.0f * s);
    const std::size_t n = m_pickups.size();
    for (std::size_t i = 0; i < n; ++i)
    {
        const Pickup& p = m_pickups[i];
        const float fadeIn = std::clamp(p.age / 0.2f, 0.0f, 1.0f);
        const float fadeOut = std::clamp((kPickupTime - p.age) / 0.8f, 0.0f, 1.0f);
        const float a = std::min(fadeIn, fadeOut);
        const float slide = std::round(14.0f * s * (1.0f - fadeIn));
        const float y = bottom - static_cast<float>(n - i) * lineH;
        const float tw = render::measureText(f, p.text);
        // A soft dark plate behind the line keeps it readable over grass and window frames.
        batch.fillRoundedRect({left - std::round(6.0f * s), y, tw + std::round(12.0f * s), lineH - std::round(2.0f * s)},
                              std::round(4.0f * s), Color::hex(0x05070A, 0.45f * a));
        render::TextStyle ts;
        ts.color = Color::hex(p.rgb, a);
        ts.outline = true;
        ts.outlineColor = Color::hex(0x000000, 0.85f * a);
        render::drawText(batch, f, p.text, {left - slide, y}, ts);
    }
}

// ---------------------------------------------------------------------------------------------
// Automation
// ---------------------------------------------------------------------------------------------

void WorldScene::lootStates(std::vector<std::string>& out) const
{
    std::size_t bags = 0;
    bool mine = false;
    bool locked = false;
    bool free = false;
    bool inRange = false;
    const Vec3 me = m_model.local().mover().position();
    for (const world::RemoteEntity& e : m_model.entities())
    {
        if (!e.active || !e.isLoot())
            continue;
        ++bags;
        switch (lootLabelOf(e))
        {
        case world::LootLabel::Mine: mine = true; break;
        case world::LootLabel::Locked: locked = true; break;
        case world::LootLabel::Free: free = true; break;
        }
        if (world::horizontalDistance(e.position, me) <= world::kLootRange)
            inRange = true;
    }
    out.emplace_back(bags > 0 ? "lootvisible" : "noloot");
    if (mine)
        out.emplace_back("lootmine");
    if (locked)
        out.emplace_back("lootlocked");
    if (free)
        out.emplace_back("lootfree");
    if (inRange)
        out.emplace_back("lootinrange");
    out.emplace_back(m_loot.isOpen() ? "lootwin" : "lootwinclosed");
    if (m_loot.isOpen() && m_loot.hasContents())
        out.emplace_back(m_loot.empty() ? "lootempty" : "lootcontents");
    out.emplace_back(m_loot.takesInFlight() > 0 || m_inventory.takesPending() > 0 ? "loottaking" : "lootidle");
    if (m_lootWalk != 0)
        out.emplace_back("lootwalking");
    if (m_lootPendingOpen != 0)
        out.emplace_back("lootopening");
    if (m_coinsKnown)
        out.emplace_back("coinsknown");
    if (!m_pickups.empty())
        out.emplace_back("pickupshown");
    if (m_farmTemplate != 0)
        out.emplace_back("lootfarming");
    if (m_farmDone)
        out.emplace_back("farmdone");
    const auto counter = [&out](const char* name, std::size_t n) {
        for (std::size_t i = 1; i <= std::min<std::size_t>(n, 12); ++i)
            out.emplace_back(name + std::to_string(i));
    };
    counter("lootbags", bags);
    counter("lootopened", m_lootSeen.opened);
    counter("loottakes", m_lootSeen.takes);
    counter("lootrefused", m_lootSeen.refused);
    counter("pickups", m_lootSeen.pickups);
    counter("coingains", m_lootSeen.coinGains);
    counter("lootclosed", m_lootSeen.closedByServer);
    counter("lootspawns", m_model.stats().lootSpawns);
    counter("lootdespawns", m_model.stats().lootDespawns);
}

bool WorldScene::lootAutomation(std::string_view command, bool& handled, std::string& error)
{
    handled = false;
    std::string parseError;
    const auto cmd = world::parseLootCommand(command, parseError);
    if (!cmd)
    {
        if (parseError.empty())
            return true; // not a loot command
        handled = true;
        error = parseError;
        return false;
    }
    handled = true;
    const auto fail = [&](std::string why) {
        error = std::move(why);
        return false;
    };
    const Vec3 me = m_model.local().mover().position();
    const auto unitsOf = [&](std::uint32_t templateId) {
        std::uint64_t n = 0;
        for (const game::StoredItem& s : m_inventory.view().items())
            if (s.templateId == templateId)
                n += s.quantity;
        return n;
    };
    switch (cmd->verb)
    {
    case world::LootVerb::Nearest:
        lootInteract(cmd->mode == "any");
        return true;
    case world::LootVerb::WalkTo:
    {
        const world::RemoteEntity* best = nullptr;
        float bestD = 1e9f;
        for (const world::RemoteEntity& e : m_model.entities())
        {
            if (!e.active || !e.isLoot())
                continue;
            if (cmd->mode != "any" && cmd->mode != world::lootLabelName(lootLabelOf(e)))
                continue;
            const float d = world::horizontalDistance(e.position, me);
            if (d < bestD)
            {
                bestD = d;
                best = &e;
            }
        }
        if (best == nullptr)
            return fail(std::format("no {} loot bag in view ({} bag(s))", cmd->mode, m_model.lootCount()));
        const float k = bestD > 1.6f ? (bestD - 1.6f) / bestD : 0.0f;
        VO_LOG_INFO("world: walking to bag {} ({:.1f} m away)", best->id, bestD);
        return setRouteNear(me, me.x + (best->position.x - me.x) * k, me.z + (best->position.z - me.z) * k, 2.0f, error);
    }
    case world::LootVerb::TakeAll:
        if (!m_loot.isOpen())
            return fail("takeall: no loot window open");
        requestLootTake(vn::kLootTakeAll);
        return true;
    case world::LootVerb::Take:
        if (!m_loot.isOpen())
            return fail("take: no loot window open");
        if (!m_loot.row(static_cast<std::uint8_t>(cmd->a)))
            return fail(std::format("take: the window lists no {}", cmd->a == vn::kLootTakeCoins
                                                                         ? std::string("Akçe")
                                                                         : "entry " + std::to_string(cmd->a)));
        requestLootTake(static_cast<std::uint8_t>(cmd->a));
        return true;
    case world::LootVerb::Close:
        if (!m_loot.isOpen())
            return fail("lootclose: no loot window open");
        closeLootWindow(true, {});
        return true;
    case world::LootVerb::ExpectCoins:
        VO_LOG_INFO("world: Akçe balance {}{}", m_coins, m_coinsKnown ? "" : " (no COINS yet)");
        if (!m_coinsKnown || !world::compare(cmd->op, m_coins, cmd->a))
            return fail(std::format("Akçe balance {}{}, expected {} {}", m_coins, m_coinsKnown ? "" : " (unknown)",
                                    world::compareOpText(cmd->op), cmd->a));
        return true;
    case world::LootVerb::ExpectItem:
    {
        const std::uint64_t n = unitsOf(cmd->a);
        if (!world::compare(cmd->op, n, cmd->b))
            return fail(std::format("{} unit(s) of template {}, expected {} {}", n, cmd->a, world::compareOpText(cmd->op),
                                    cmd->b));
        return true;
    }
    case world::LootVerb::ExpectStack:
    {
        const game::ItemStack& s = m_inventory.view().at(game::ItemPlace::bag(static_cast<std::uint8_t>(cmd->a)));
        if (s.empty() || s.templateId != cmd->b || s.quantity != cmd->c)
            return fail(std::format("bag slot {} holds {}, expected {} x {}", cmd->a,
                                    s.empty() ? std::string("nothing") : std::format("{} x {}", s.quantity, s.templateId),
                                    cmd->c, cmd->b));
        return true;
    }
    case world::LootVerb::ExpectRows:
        if (!m_loot.isOpen())
            return fail("expectlootrows: no loot window open");
        if (m_loot.rows().size() != cmd->a)
            return fail(std::format("the loot window lists {} row(s), expected {}", m_loot.rows().size(), cmd->a));
        return true;
    case world::LootVerb::ExpectMessage:
        if (m_lastLootMessage.find(cmd->text) == std::string::npos)
            return fail(std::format("last loot message is '{}', expected '{}'", m_lastLootMessage, cmd->text));
        return true;
    case world::LootVerb::Farm:
        if (cmd->mode == "stop")
        {
            m_farmTemplate = 0;
            m_huntLevel = 0;
            stopAttack(true);
            return true;
        }
        m_huntName = cmd->text;
        m_huntLevel = 11; // above the level cap: the hunt ends by the pickup count, not by a level
        m_huntNextTry = 0.0;
        m_farmTemplate = static_cast<std::uint16_t>(cmd->a);
        m_farmWant = cmd->b;
        m_farmSeen = 0;
        m_farmDone = false;
        m_farmSkip.clear();
        VO_LOG_INFO("world: farming '{}' until {} pickup(s) of template {}", m_huntName, m_farmWant, m_farmTemplate);
        return true;
    case world::LootVerb::MoveStack:
    {
        for (std::uint8_t i = 0; i < game::kBagSlots; ++i)
        {
            const game::ItemStack& s = m_inventory.view().at(game::ItemPlace::bag(i));
            if (s.empty() || s.templateId != cmd->a)
                continue;
            if (i != cmd->b)
                (void)requestItemMove(game::ItemPlace::bag(i), game::ItemPlace::bag(static_cast<std::uint8_t>(cmd->b)));
            return true;
        }
        return fail(std::format("movestack: no stack of template {} in the bag", cmd->a));
    }
    case world::LootVerb::Log:
    {
        VO_LOG_INFO("world: loot: {} bag(s) in view, window {} ({} row(s), {} take(s) in flight), Akçe {}, "
                    "opened {} takes {} refused {} pickups {} coin gains {}",
                    m_model.lootCount(), m_loot.lootId(), m_loot.rows().size(), m_loot.takesInFlight(), m_coins,
                    m_lootSeen.opened, m_lootSeen.takes, m_lootSeen.refused, m_lootSeen.pickups, m_lootSeen.coinGains);
        for (const world::RemoteEntity& e : m_model.entities())
            if (e.active && e.isLoot())
                VO_LOG_INFO("world:   bag {} from '{}' lvl {} at ({:.1f}, {:.1f}) {:.1f} m, {}", e.id, e.name, e.level,
                            e.position.x, e.position.z, world::horizontalDistance(e.position, me),
                            world::lootLabelName(lootLabelOf(e)));
        for (const world::LootRow& r : m_loot.rows())
            VO_LOG_INFO("world:   row {}: template {} x {}", r.coins ? std::string("Akçe") : std::to_string(r.index),
                        r.templateId, r.quantity);
        return true;
    }
    }
    return true;
}

} // namespace vo::client::scenes
