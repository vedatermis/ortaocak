// M13 "Üç Yurt" presentation of the world scene (docs/PROTOCOL_ZONES.md §7; logic in
// client/world/ZoneView.h): the zone change (gate screen from ZONE_CHANGE_BEGIN, a fresh scene for
// ZONE_CHANGE(Ok), staying put after ZONE_CHANGE(Failed)), gate refusals, the gate labels / hints /
// veils of the zone, and the zone automation commands.
#include "client/net/GameClient.h"
#include "client/render/AssetCache.h"
#include "client/render/TextRenderer.h"
#include "client/render/WorldRenderer.h"
#include "client/scenes/WorldScene.h"
#include "client/ui/Ui.h"
#include "client/world/TerrainQuery.h"
#include "shared/core/Log.h"
#include "shared/game/ZoneData.h"
#include "shared/net/Messages.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <sstream>

namespace vo::client::scenes {

namespace vn = vo::net;
using world::Vec3;

namespace {

// Automation `zonehold <gateSeconds> <loadSeconds>`: the gate screen and the loading screen of the next
// zone changes stay up at least this long (screenshots). Process-wide: the loading screen belongs to
// the next world scene. 0 = off (the default; players never wait for it).
struct ZoneHold
{
    float gate = 0.0f;
    float load = 0.0f;
};
ZoneHold g_zoneHold;

constexpr float kVeilOpacity = 0.5f; // docs/WORLD_ZONES_ART.md §3
constexpr float kGateLabelLift = 0.7f; // metres above the gate prop's top

} // namespace

bool WorldScene::holdNetEvents() const
{
    return m_zoneChanging && m_changeTime < g_zoneHold.gate;
}

bool WorldScene::loadingHeld() const
{
    if (g_zoneHold.load <= 0.0f)
        return false;
    const auto& entry = ctx().net.account().world;
    return entry && entry->zoneChange && m_loadingTime < g_zoneHold.load;
}

// ---------------------------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------------------------

bool WorldScene::handleZoneMessage(const net::NetEvent& e)
{
    if (e.is(vn::Opcode::ZoneChangeBegin))
    {
        vn::ZoneChangeBegin b;
        if (!vn::decodeMessage(e.payload, b))
        {
            returnToLogin(net::connectFailureMessage(vn::ConnectResult::ProtocolError));
            return true;
        }
        VO_LOG_INFO("world: zone change {} -> {} begins", b.fromZone, b.toZone);
        ++m_zonesSeen.begins;
        beginZoneChange(b.toZone);
        return true;
    }
    if (e.is(vn::Opcode::ZoneChange))
    {
        vn::ZoneChange zc;
        auto& account = ctx().net.account();
        if (!vn::decodeMessage(e.payload, zc) || !account.world)
        {
            returnToLogin(net::connectFailureMessage(vn::ConnectResult::ProtocolError));
            return true;
        }
        const bool failed = zc.result != vn::ZoneChangeResult::Ok;
        VO_LOG_INFO("world: zone change {}: now in zone {} as entity {}{}", failed ? "FAILED" : "ok", zc.enter.zoneId,
                    zc.enter.entityId, m_simulatedChange ? " (automation simulation)" : "");
        account.world->enter = zc.enter;
        account.world->character.zoneId = zc.enter.zoneId;
        account.world->character.level = zc.enter.level;
        account.world->zoneChange = true;
        account.world->zoneChangeFailed = failed;
        if (failed)
        {
            ++m_zonesSeen.failed;
            // §3.5 / §7.4: back where we stood in the old zone. A ready scene of that zone keeps its
            // terrain and resumes at once; otherwise (still loading) the scene is built again.
            if (m_phase == Phase::Ready && m_terrain != nullptr && zc.enter.zoneId == m_zoneId)
            {
                resumeAfterFailedChange();
                return true;
            }
        }
        m_model.dropRemotes();
        changeScene(std::make_unique<WorldScene>()); // the packets after ZONE_CHANGE go to the new scene
        return true;
    }
    if (e.is(vn::Opcode::ZoneGateRefused))
    {
        vn::ZoneGateRefused r;
        if (!vn::decodeMessage(e.payload, r))
        {
            returnToLogin(net::connectFailureMessage(vn::ConnectResult::ProtocolError));
            return true;
        }
        ++m_zonesSeen.refusals;
        showZoneMessage(world::gateClosedText(game::zones(), r.toZone));
        return true;
    }
    return false;
}

void WorldScene::beginZoneChange(std::uint16_t toZone)
{
    // §7.1: stop at once (GameClient also refuses MOVE_REQ until ZONE_LOADED_REQ), close the loot
    // window, drop target / auto-attack / cast, forget everything of the old zone (§3.3 step 2: no
    // ENTITY_DESPAWNs follow).
    m_zoneChanging = true;
    m_changeTo = toZone;
    m_changeTime = 0.0f;
    world::LocalMover& walker = m_model.local().mover();
    walker.clearDestination();
    m_route.clear();
    m_routeNext = 0;
    m_pendingDestination.reset();
    m_holdMove = false;
    stopDrag();
    m_rightDown = false;
    m_skillDrag = {};
    m_target = 0;
    m_targetSent = true;
    m_autoAttack = false;
    m_chasing = false;
    m_queuedSkill = 0;
    m_attackOnArrive = false;
    m_skillTimers.cancelCast();
    m_lootWalk = 0;
    m_lootHover = 0;
    closeLootWindow(false, {});
    forgetShop(); // M14 (PROTOCOL_SHOP.md §8.2): the shop window closes silently, requests in flight are forgotten
    m_model.dropRemotes();
    m_effects.clearAll(); // EFFECT_LIST about ourselves follows ZONE_CHANGE
    m_bolts.clear();
    m_art.clear();
    m_leaps.clear();
    m_numbers.clear();
    m_gateHint.clear();
    m_gateLabelsShown.clear();
    if (m_menuOpen)
        closeMenu();
    // The HUD goes with the world: the loading screen is drawn alone (message boxes stay on top).
    ui().root().setVisible(false);
    // Start reading the target zone's terrain now (worker thread): the next scene finds it cached.
    if (ctx().assets != nullptr && ctx().assets->hasRoot())
    {
        const game::ZoneDef* z = game::zones().find(toZone);
        const std::string path =
            z != nullptr ? z->terrain : std::format("terrain/zone{0}/zone{0}.vterrain", toZone);
        ctx().assets->request(render::AssetCache::Kind::Terrain, path);
    }
}

void WorldScene::endZoneChangeScreen()
{
    m_zoneChanging = false;
    m_changeTo = 0;
    m_changeTime = 0.0f;
    ui().root().setVisible(true);
}

void WorldScene::resumeAfterFailedChange()
{
    auto& entry = ctx().net.account().world;
    const vn::EnterWorldRes enter = entry->enter;
    // A new entity id, the place we stood (inside the gate, disarmed until we step out), everything
    // else as before; INVENTORY, SKILL_*, CHAR_STATS, EFFECT_LIST and the spawns follow (§3.5).
    m_model.enter(enter, m_terrain, m_time, m_runDefault);
    world::LocalMover& mover = m_model.local().mover();
    mover.setPosition(mover.position(), mover.yaw(), m_terrain);
    initCombat(enter, m_charClass);
    m_effects.clearAll();
    m_shownPlayers = static_cast<std::size_t>(-1);
    entry->zoneChange = false;
    entry->zoneChangeFailed = false;
    if (!m_simulatedChange)
        (void)ctx().net.send(vn::ZoneLoadedReq{enter.zoneId});
    m_simulatedChange = false;
    endZoneChangeScreen();
    m_hudTimer = 0.0f;
    updateHud(0.0f);
    showZoneMessage(std::string(world::kZoneChangeFailedText));
    VO_LOG_INFO("world: zone change failed; staying in zone {} at ({:.1f}, {:.1f})", m_zoneId, mover.position().x,
                mover.position().z);
}

void WorldScene::showZoneMessage(std::string text)
{
    showCombatMessage(text);
    m_lastZoneMessage = std::move(text);
}

// ---------------------------------------------------------------------------------------------
// Gates
// ---------------------------------------------------------------------------------------------

void WorldScene::buildGateViews()
{
    m_gates.clear();
    if (m_terrain == nullptr)
        return;
    render::AssetCache& cache = *ctx().assets;
    const assets::TerrainData& terrain = *m_terrain;
    m_gates = world::buildGateViews(terrain, game::zones(), m_zoneId, m_race, [&](int i) {
        const assets::ObjectPlacement& o = terrain.objects[static_cast<std::size_t>(i)];
        const render::GpuMesh* mesh = cache.mesh(o.mesh);
        if (mesh == nullptr || !mesh->cpu)
            return 0.0f;
        return mesh->cpu->boundsMax.y * o.scale * o.stretch.y;
    });
    for (const world::GateView& g : m_gates)
        VO_LOG_INFO("world: gate {} at ({:.1f}, {:.1f}) -> zone {} '{}' ({}), prop {}, veil {}{}", g.marker,
                    g.position.x, g.position.z, g.toZone, g.label, g.open ? "open" : "closed to us", g.placement,
                    g.veilMesh.empty() ? "-" : g.veilMesh,
                    !g.veilMesh.empty() && cache.mesh(g.veilMesh) == nullptr ? " (NOT LOADED)" : "");
}

void WorldScene::updateZones(float dt)
{
    if (m_zoneChanging)
    {
        m_changeTime += dt;
        if (ctx().assets != nullptr)
            ctx().assets->pump(0.004); // the target terrain loads in the background
        return;
    }
    m_gateHint.clear();
    const Vec3 p = m_model.local().mover().position();
    if (const world::GateView* g = world::nearestGate(m_gates, p.x, p.z, world::kGateHintRange); g != nullptr)
        m_gateHint = g->hint;
}

void WorldScene::addZoneDraws()
{
    m_veilsDrawn = 0;
    if (m_terrain == nullptr)
        return;
    render::WorldRenderer& r = *ctx().worldRenderer;
    render::AssetCache& cache = *ctx().assets;
    for (std::size_t i = 0; i < m_gates.size(); ++i)
    {
        const world::GateView& g = m_gates[i];
        if (g.placement < 0 || g.veilMesh.empty())
            continue;
        const render::GpuMesh* mesh = cache.mesh(g.veilMesh);
        if (mesh == nullptr)
            continue;
        // At the gate placement's own transform (docs/WORLD_ZONES_ART.md §3), the opacity breathing a
        // little around 0.5.
        const assets::ObjectPlacement& o = m_terrain->objects[static_cast<std::size_t>(g.placement)];
        const float t = static_cast<float>(m_time) * 1.6f + static_cast<float>(i) * 1.3f;
        r.addEffectMesh(mesh, o.worldMatrix(), {1.0f, 1.0f, 1.0f}, kVeilOpacity + 0.08f * std::sin(t));
        ++m_veilsDrawn;
    }
}

void WorldScene::drawGateOverlay(const world::Mat4& vp)
{
    m_gateLabelsShown.clear();
    if (m_terrain == nullptr || m_gates.empty())
        return;
    auto& batch = ctx().batch;
    const float scale = ui().scale();
    const float w = ctx().viewportWidth;
    const float h = ctx().viewportHeight;
    const Vec3 eye = m_camera.eye();
    const Vec3 me = m_model.local().mover().position();
    const auto px = [scale](float v) { return std::round(v * scale); };

    // §7.3: "<Bölge> Geçidi" above every gate within 40 m (hidden behind hills, not behind houses: it
    // is a signpost seen over the roofs).
    const render::Font& font = ctx().fonts.get(render::FontWeight::Bold, 18.0f * scale);
    m_gateLabelState.assign(m_gates.size(), "drawn");
    for (std::size_t i = 0; i < m_gates.size(); ++i)
    {
        const world::GateView& g = m_gates[i];
        const float d = std::hypot(g.position.x - me.x, g.position.z - me.z);
        const float alpha = world::gateLabelAlpha(d);
        const char*& state = m_gateLabelState[i];
        if (alpha <= 0.0f)
        {
            state = "beyond 40 m";
            continue;
        }
        const Vec3 anchor{g.position.x, g.top + kGateLabelLift, g.position.z};
        world::Vec2 s{};
        if (!world::projectToScreen(vp, anchor, w, h, s) || s.x < -200.0f || s.x > w + 200.0f || s.y < -50.0f ||
            s.y > h + 50.0f)
        {
            state = "off screen";
            continue;
        }
        if (!world::segmentClearOfTerrain(*m_terrain, eye, anchor, 0.1f, 2.0f))
        {
            state = "behind terrain";
            continue;
        }
        render::TextStyle style;
        style.color = Color::hex(g.open ? 0xF2D27A : 0xD8A08A, alpha);
        style.outline = true;
        style.outlineColor = Color::hex(0x000000, 0.85f * alpha);
        const float tw = render::measureText(font, g.label);
        render::drawText(batch, font, g.label, {std::round(s.x - tw * 0.5f), std::round(s.y - font.lineHeight())}, style);
        m_gateLabelsShown.push_back(g.label);
    }

    // §7.3: the hint near the bottom within 8 m (not while the same words are the combat message:
    // the refusal line says it already).
    if (m_gateHint.empty() || m_self.dead || (m_combatMessage == m_gateHint && m_combatMessageAge < 2.6f))
        return;
    const world::GateView* g = world::nearestGate(m_gates, me.x, me.z, world::kGateHintRange);
    const bool open = g == nullptr || g->open;
    const render::Font& hf = ctx().fonts.get(render::FontWeight::Bold, 16.0f * scale);
    const float tw = render::measureText(hf, m_gateHint);
    const float padX = px(16.0f);
    const float boxH = std::round(hf.lineHeight() + px(10.0f));
    // Above the combat message line, above the HUD bar (HudView.h hudBottomLines).
    const Rect box{std::round((w - tw) * 0.5f - padX), std::round(bottomLines().gateHint), std::round(tw + 2.0f * padX), boxH};
    const ui::Theme& th = theme();
    batch.fillRoundedRect(box, boxH * 0.5f, Color::hex(0x0B0E12, 0.72f));
    batch.strokeRoundedRect(box, boxH * 0.5f, std::max(1.0f, scale),
                            open ? th.ornament.withAlpha(0.8f) : Color::hex(0xB06A50, 0.8f));
    render::TextStyle st;
    st.color = open ? Color::hex(0xF4E6C0) : Color::hex(0xF0B9A0);
    st.shadow = true;
    render::drawTextAligned(batch, hf, m_gateHint, box, render::HAlign::Center, render::VAlign::Middle, st);
}

// ---------------------------------------------------------------------------------------------
// Automation
// ---------------------------------------------------------------------------------------------

void WorldScene::zonesStates(std::vector<std::string>& out) const
{
    if (m_zoneChanging)
        out.emplace_back("gatescreen");
    out.emplace_back(m_gateHint.empty() ? "nogatehint" : "gatehint");
    if (!m_gateLabelsShown.empty())
        out.emplace_back("gatelabel");
    if (m_bannerTime < 0.0f)
        out.emplace_back("nobanner");
}

bool WorldScene::zonesAutomation(std::string_view command, bool& handled, std::string& error)
{
    std::istringstream in{std::string(command)};
    std::string verb;
    in >> verb;
    handled = true;
    auto fail = [&](std::string why) {
        error = std::move(why);
        return false;
    };
    auto rest = [&]() {
        std::string text;
        std::getline(in >> std::ws, text);
        return text;
    };
    const Vec3 p = m_model.local().mover().position();
    const auto findGate = [&](const std::string& key) -> const world::GateView* {
        int zone = 0;
        const auto [ptr, ec] = std::from_chars(key.data(), key.data() + key.size(), zone);
        const bool numeric = ec == std::errc{} && ptr == key.data() + key.size();
        for (const world::GateView& g : m_gates)
            if ((numeric && g.toZone == zone) || g.marker == key)
                return &g;
        return nullptr;
    };

    if (verb == "gates" || verb == "expectgates")
    {
        for (const world::GateView& g : m_gates)
            VO_LOG_INFO("world: gate {} -> zone {} '{}' at ({:.1f}, {:.1f}), {:.1f} m away, {}, hint '{}'", g.marker,
                        g.toZone, g.label, g.position.x, g.position.z, std::hypot(g.position.x - p.x, g.position.z - p.z),
                        g.open ? "open" : "closed", g.hint);
        if (verb == "gates")
            return true;
        std::size_t n = 0;
        if (!(in >> n))
            return fail("usage: expectgates <n>");
        if (m_gates.size() != n)
            return fail(std::format("{} gate(s) in zone {}, expected {}", m_gates.size(), m_zoneId, n));
        return true;
    }
    if (verb == "walktogate")
    {
        // walktogate <toZone|marker> [metres in front]: 0 walks into the trigger.
        std::string key;
        float front = 0.0f;
        if (!(in >> key))
            return fail("usage: walktogate <toZone|marker> [metresInFront]");
        in >> front;
        const world::GateView* g = findGate(key);
        if (g == nullptr)
            return fail(std::format("no gate '{}' in zone {}", key, m_zoneId));
        const float x = g->position.x + std::sin(g->yaw) * front;
        const float z = g->position.z + std::cos(g->yaw) * front;
        return setRouteNear(p, x, z, 1.5f, error);
    }
    if (verb == "facegate")
    {
        // facegate <toZone|marker> <pitchDeg> <distance>: the camera behind the player, looking at the gate.
        std::string key;
        float pitch = 0.0f;
        float dist = 0.0f;
        if (!(in >> key >> pitch >> dist))
            return fail("usage: facegate <toZone|marker> <pitchDeg> <distance>");
        const world::GateView* g = findGate(key);
        if (g == nullptr)
            return fail(std::format("no gate '{}' in zone {}", key, m_zoneId));
        m_camera.setPose(std::atan2(g->position.x - p.x, g->position.z - p.z), pitch * world::kPi / 180.0f, dist);
        return true;
    }
    if (verb == "expectgatelabel" || verb == "expectnogatelabel")
    {
        const std::string text = rest();
        const bool shown = std::any_of(m_gateLabelsShown.begin(), m_gateLabelsShown.end(),
                                       [&](const std::string& l) { return l.find(text) != std::string::npos; });
        if (shown != (verb == "expectgatelabel"))
        {
            std::string all;
            for (std::size_t i = 0; i < m_gates.size(); ++i)
                all += std::format("{}{} at {:.1f} m (label top y {:.1f}): {}", all.empty() ? "" : "; ", m_gates[i].label,
                                   std::hypot(m_gates[i].position.x - p.x, m_gates[i].position.z - p.z),
                                   m_gates[i].top, i < m_gateLabelState.size() ? m_gateLabelState[i] : "?");
            return fail(std::format("gate label '{}' {} ({})", text, shown ? "is drawn" : "is not drawn", all));
        }
        return true;
    }
    if (verb == "expectgatehint" || verb == "expectnogatehint")
    {
        const std::string text = rest();
        if (verb == "expectnogatehint")
        {
            if (!m_gateHint.empty())
                return fail(std::format("gate hint '{}' is shown", m_gateHint));
            return true;
        }
        if (m_gateHint.empty() || m_gateHint.find(text) == std::string::npos)
            return fail(std::format("gate hint is '{}', expected '{}'", m_gateHint, text));
        return true;
    }
    if (verb == "expectzonemessage")
    {
        const std::string text = rest();
        if (m_lastZoneMessage.find(text) == std::string::npos)
            return fail(std::format("last zone message is '{}', expected '{}'", m_lastZoneMessage, text));
        return true;
    }
    if (verb == "expectveils")
    {
        std::uint32_t n = 0;
        if (!(in >> n))
            return fail("usage: expectveils <n>");
        if (m_veilsDrawn != n)
            return fail(std::format("{} gate veil(s) drawn, expected {}", m_veilsDrawn, n));
        return true;
    }
    if (verb == "expectzoneseen")
    {
        // expectzoneseen <begins> <failed> <refusals>
        std::uint32_t b = 0;
        std::uint32_t f = 0;
        std::uint32_t r = 0;
        if (!(in >> b >> f >> r))
            return fail("usage: expectzoneseen <begins> <failed> <refusals>");
        if (m_zonesSeen.begins != b || m_zonesSeen.failed != f || m_zonesSeen.refusals != r)
            return fail(std::format("seen {} ZONE_CHANGE_BEGIN, {} failed ZONE_CHANGE, {} ZONE_GATE_REFUSED; expected {} {} {}",
                                    m_zonesSeen.begins, m_zonesSeen.failed, m_zonesSeen.refusals, b, f, r));
        return true;
    }
    if (verb == "zonehold")
    {
        float gate = 0.0f;
        float load = 0.0f;
        if (!(in >> gate >> load) || gate < 0.0f || load < 0.0f || gate > 10.0f || load > 10.0f)
            return fail("usage: zonehold <gateSeconds 0..10> <loadSeconds 0..10>");
        g_zoneHold = {gate, load};
        VO_LOG_INFO("world: automation: zone change screens held {:.1f} s / {:.1f} s", gate, load);
        return true;
    }
    if (verb == "zonefail")
    {
        // A LOCAL simulation of ZONE_CHANGE(Failed) (§3.5) through the gate `toZone`: the client gets
        // ZONE_CHANGE_BEGIN and ZONE_CHANGE(Failed) with its current entity and place, injected into
        // its own event queue (the server is not involved: no ZONE_LOADED_REQ is sent afterwards).
        std::uint16_t to = 0;
        if (!(in >> to))
            return fail("usage: zonefail <toZone>");
        const auto& entry = ctx().net.account().world;
        if (!entry || m_zoneChanging)
            return fail("not in a zone");
        vn::ZoneChangeBegin b{m_zoneId, to};
        vn::ZoneChange zc;
        zc.result = vn::ZoneChangeResult::Failed;
        zc.fromZone = m_zoneId;
        zc.enter = entry->enter;
        zc.enter.entityId = m_model.localEntityId();
        zc.enter.zoneId = m_zoneId;
        zc.enter.x = p.x;
        zc.enter.y = p.y;
        zc.enter.z = p.z;
        zc.enter.yaw = m_model.local().mover().yaw();
        zc.enter.level = m_self.level;
        zc.enter.xp = m_self.xp;
        zc.enter.xpToNext = m_self.xpToNext;
        zc.enter.hp = m_self.hp;
        zc.enter.maxHp = m_self.maxHp;
        zc.enter.mp = m_self.mp;
        zc.enter.maxMp = m_self.maxMp;
        zc.enter.attack = m_self.attack;
        zc.enter.defense = m_self.defense;
        zc.enter.isDead = 0;
        const auto beginPayload = vn::encodeMessage(b);
        const auto changePayload = vn::encodeMessage(zc);
        if (!beginPayload || !changePayload)
            return fail("cannot encode the simulated zone change");
        m_simulatedChange = true;
        ctx().net.injectMessage(static_cast<std::uint16_t>(vn::Opcode::ZoneChangeBegin), *beginPayload);
        ctx().net.injectMessage(static_cast<std::uint16_t>(vn::Opcode::ZoneChange), *changePayload);
        VO_LOG_INFO("world: automation: simulated ZONE_CHANGE(Failed) through the gate to zone {}", to);
        return true;
    }
    handled = false;
    return false;
}

} // namespace vo::client::scenes
