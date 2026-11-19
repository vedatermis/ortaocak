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
//   animate attack|death|revive|hit|cast|drink|stand        play a local animation action (cosmetic)
//   expectlocalarmor <n>                                    n armour pieces drawn on the local character
//   expectremotearmor <name> <head> <chest> <legs> <hands> <feet>
//                                                            a remote's armour (ENTITY_SPAWN / ENTITY_EQUIP)
//   armorlog                                                logs the local and remote armour
#include "client/scenes/WorldScene.h"

#include "client/scenes/SceneResources.h"
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

bool WorldScene::armorAutomation(std::string_view command, bool& handled, std::string& error)
{
    std::istringstream in{std::string(command)};
    std::string verb;
    in >> verb;
    static constexpr std::string_view kVerbs[] = {"wear", "wield", "animate", "expectlocalarmor", "expectremotearmor",
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
