// WorldScene, M17 armour (docs/CLIENT_RENDERING.md §18): the automation of the worn armour and the
// cosmetic dress-up the screenshot flows use. Drawing itself is in WorldScene::addCharacterDraw
// (attachArmor -> render::CharacterDraw::worn); the remotes' armour arrives with ENTITY_SPAWN /
// ENTITY_EQUIP (world::RemoteEntity::armor).
//
// `scene` verbs:
//   wear <head> <chest> <legs> <hands> <feet> | wear off   draw the local character in these armour
//                                                            templates (0 = none); never sent to the
//                                                            server (screenshots only)
//   wield <main> <off> | wield off                          the same for the weapons in the hands
//   wieldmodel <model> <grip> [<offModel> <offGrip>]        weapon meshes by model id (no template needed)
//   animate attack|death|revive|hit|cast|drink|stand        play a local animation action (cosmetic)
//   expectlocalarmor <n>                                    n armour pieces drawn on the local character
//   expectremotearmor <name> <head> <chest> <legs> <hands> <feet>
//                                                            a remote's armour (ENTITY_SPAWN / ENTITY_EQUIP)
//   armorlog                                                logs the local and remote armour
#include "client/scenes/WorldScene.h"

#include "client/render/AssetCache.h"
#include "client/render/WorldRenderer.h"
#include "client/scenes/CharacterPreview.h"
#include "client/scenes/SceneResources.h"
#include "client/world/WeaponPose.h"
#include "client/world/ItemData.h"
#include "shared/core/Log.h"

#include <algorithm>
#include <format>
#include <sstream>

namespace vo::client::scenes {

game::VisibleEquipment WorldScene::drawnLocalEquipment() const
{
    game::VisibleEquipment v = localEquipment();
    if (m_wearOverride)
        v.armor = *m_wearOverride;
    if (m_wieldOverride)
    {
        v.mainHand = (*m_wieldOverride)[0];
        v.offHand = (*m_wieldOverride)[1];
    }
    return v;
}

std::size_t WorldScene::attachWieldModels(render::CharacterDraw& d, std::uint8_t race, int look,
                                          const world::Animator& animator)
{
    d.attached = {};
    const world::ItemData* items = ctx().resources.itemData();
    const assets::MeshData* mesh = animator.mesh();
    if (items == nullptr || mesh == nullptr)
        return 0;
    render::AssetCache& cache = *ctx().assets;
    const std::string lookKey = world::attachmentLookKey(race, cache.looks().effectiveLook(race, look));
    const auto models = animator.modelMatrices();
    std::size_t n = 0;
    for (std::size_t h = 0; h < m_wieldModels.size(); ++h)
    {
        const auto& [model, grip] = m_wieldModels[h];
        if (model.empty())
            continue;
        const game::WeaponAttachment* a =
            items->attachments.find(grip, lookKey, h == 0 ? game::WeaponHand::Main : game::WeaponHand::Off);
        const int bone = a != nullptr ? mesh->skeleton.findBone(a->bone) : -1;
        const std::string path = "weapons/" + model + ".vmesh";
        const render::GpuMesh* m = cache.mesh(path);
        if (m == nullptr && !cache.failed(path))
            cache.request(render::AssetCache::Kind::Mesh, path);
        if (m == nullptr || bone < 0 || static_cast<std::size_t>(bone) >= models.size())
            continue;
        d.attached[n].mesh = m;
        d.attached[n].world = game::weaponWorldMatrix(d.world, models[static_cast<std::size_t>(bone)], *a);
        ++n;
    }
    return n;
}

bool WorldScene::armorAutomation(std::string_view command, bool& handled, std::string& error)
{
    std::istringstream in{std::string(command)};
    std::string verb;
    in >> verb;
    static constexpr std::string_view kVerbs[] = {"wear", "wield", "wieldmodel", "focusdrop", "dressremotes", "animate", "expectlocalarmor", "expectremotearmor",
                                                  "armorlog"};
    handled = std::find(std::begin(kVerbs), std::end(kVerbs), verb) != std::end(kVerbs);
    if (!handled)
        return true;
    const auto fail = [&](std::string why) {
        error = std::move(why);
        return false;
    };
    const auto readArmor = [&](game::ArmorAppearance& out) {
        for (auto& id : out)
        {
            int v = -1;
            if (!(in >> v) || v < 0 || v > 65535)
                return false;
            id = static_cast<std::uint16_t>(v);
        }
        return true;
    };
    const world::ItemData* items = ctx().resources.itemData();
    if (verb == "wear")
    {
        std::string first;
        if (!(in >> first))
            return fail("usage: wear <head> <chest> <legs> <hands> <feet> | wear off");
        if (first == "off")
        {
            m_wearOverride.reset();
            return true;
        }
        in.clear();
        in.str(std::string(command));
        in >> verb;
        game::ArmorAppearance a{};
        if (!readArmor(a))
            return fail("usage: wear <head> <chest> <legs> <hands> <feet> (template ids, 0 = none)");
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            if (a[i] == 0)
                continue;
            const game::ItemTemplate* t = items != nullptr ? items->catalog.find(a[i]) : nullptr;
            if (t == nullptr || !t->isArmor() || t->slot != static_cast<game::SlotRule>(static_cast<int>(game::SlotRule::Head) + static_cast<int>(i)))
                return fail(std::format("wear: template {} is not armour for slot {}", a[i], i));
        }
        m_wearOverride = a;
        VO_LOG_INFO("world: wearing (cosmetic) {} {} {} {} {}", a[0], a[1], a[2], a[3], a[4]);
        return true;
    }
    if (verb == "wield")
    {
        std::string first;
        if (!(in >> first))
            return fail("usage: wield <main> <off> | wield off");
        if (first == "off")
        {
            m_wieldOverride.reset();
            return true;
        }
        int off = -1;
        int main = -1;
        try
        {
            main = std::stoi(first);
        }
        catch (...)
        {
            return fail("wield: bad main-hand id");
        }
        if (!(in >> off) || main < 0 || off < 0 || main > 65535 || off > 65535)
            return fail("usage: wield <main> <off>");
        m_wieldOverride = std::array<std::uint16_t, 2>{static_cast<std::uint16_t>(main), static_cast<std::uint16_t>(off)};
        VO_LOG_INFO("world: wielding (cosmetic) {} / {}", main, off);
        return true;
    }
    if (verb == "wieldmodel")
    {
        std::string model;
        std::string grip;
        if (!(in >> model))
            return fail("usage: wieldmodel <model> <grip> [<offModel> <offGrip>] | wieldmodel off");
        if (model == "off")
        {
            m_wieldModelsOn = false;
            m_wieldModels = {};
            return true;
        }
        const auto parseGrip = [](const std::string& s) -> std::optional<game::Grip> {
            for (int g = 0; g <= static_cast<int>(game::Grip::Shield); ++g)
                if (game::gripName(static_cast<game::Grip>(g)) == s)
                    return static_cast<game::Grip>(g);
            return std::nullopt;
        };
        m_wieldModels = {};
        for (std::size_t h = 0; h < 2; ++h)
        {
            if (h == 1 && !(in >> model))
                break;
            if (!(in >> grip))
                return fail("wieldmodel: missing grip");
            const auto g = parseGrip(grip);
            if (!g)
                return fail("wieldmodel: unknown grip '" + grip + "'");
            m_wieldModels[h] = {model == "-" ? std::string{} : model, *g};
        }
        m_wieldModelsOn = true;
        VO_LOG_INFO("world: wielding models (cosmetic) '{}' / '{}'", m_wieldModels[0].first, m_wieldModels[1].first);
        return true;
    }
    if (verb == "focusdrop")
    {
        float drop = 0.0f;
        if (!(in >> drop) || drop < 0.0f || drop > 1.5f)
            return fail("usage: focusdrop <metres 0..1.5>");
        m_focusDrop = drop;
        return true;
    }
    if (verb == "dressremotes")
    {
        // Every remote player (e.g. --debug-ghosts) gets a full set: line and tier by its position in the
        // list (cosmetic, client-side only; the performance flow m17_perf).
        int k = 0;
        for (world::RemoteEntity& r : m_model.entities())
        {
            if (!r.active || r.isMonster() || r.isLoot())
                continue;
            const int tier = k % 5;
            const int line = (k / 5 + k) % 3;
            for (std::size_t s = 0; s < r.armor.size(); ++s)
                r.armor[s] = static_cast<std::uint16_t>(70 + 15 * tier + 5 * line + static_cast<int>(s));
            ++k;
        }
        VO_LOG_INFO("world: dressed {} remote player(s) in armour (cosmetic)", k);
        return true;
    }
    if (verb == "animate")
    {
        std::string what;
        in >> what;
        world::Animator& a = m_localAnim;
        bool ok = true;
        if (what == "attack")
            ok = a.playAttack(world::AnimState::SwordAttack, 1.6f);
        else if (what == "death")
            a.die();
        else if (what == "revive")
            a.revive();
        else if (what == "hit")
            ok = a.playHit();
        else if (what == "cast")
            ok = a.beginCast();
        else if (what == "drink")
            ok = a.playDrink();
        else if (what == "stand")
            a.standUp();
        else
            return fail("usage: animate attack|death|revive|hit|cast|drink|stand");
        if (!ok)
            return fail(std::format("animate {}: the animator refused (busy or no clip)", what));
        return true;
    }
    if (verb == "expectlocalarmor")
    {
        int n = -1;
        if (!(in >> n))
            return fail("usage: expectlocalarmor <n>");
        VO_LOG_INFO("world: local character wears {} armour piece(s) (drawn last frame)", m_localArmorDrawn);
        if (static_cast<int>(m_localArmorDrawn) != n)
            return fail(std::format("local character draws {} armour piece(s), expected {}", m_localArmorDrawn, n));
        return true;
    }
    if (verb == "expectremotearmor")
    {
        std::string who;
        game::ArmorAppearance want{};
        if (!(in >> who) || !readArmor(want))
            return fail("usage: expectremotearmor <name> <head> <chest> <legs> <hands> <feet>");
        std::replace(who.begin(), who.end(), '_', ' ');
        for (const world::RemoteEntity& r : m_model.entities())
        {
            if (!r.active || r.name != who)
                continue;
            VO_LOG_INFO("world: remote '{}' wears {} {} {} {} {}", who, r.armor[0], r.armor[1], r.armor[2], r.armor[3],
                        r.armor[4]);
            if (r.armor != want)
                return fail(std::format("remote '{}' wears {} {} {} {} {}, expected {} {} {} {} {}", who, r.armor[0],
                                        r.armor[1], r.armor[2], r.armor[3], r.armor[4], want[0], want[1], want[2], want[3],
                                        want[4]));
            return true;
        }
        return fail("no remote player '" + who + "'");
    }
    // armorlog
    const game::VisibleEquipment v = drawnLocalEquipment();
    VO_LOG_INFO("world: armour: local {} {} {} {} {} ({} drawn{})", v.armor[0], v.armor[1], v.armor[2], v.armor[3],
                v.armor[4], m_localArmorDrawn, m_wearOverride ? ", cosmetic" : "");
    for (const world::RemoteEntity& r : m_model.entities())
        if (r.active && !r.isMonster() && !r.isLoot())
            VO_LOG_INFO("world: armour: remote '{}' {} {} {} {} {}", r.name, r.armor[0], r.armor[1], r.armor[2], r.armor[3],
                        r.armor[4]);
    return true;
}

} // namespace vo::client::scenes
