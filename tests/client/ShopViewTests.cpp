// Merchant and potion logic of the client (client/world/ShopView.h, M14 "Pazar", docs/PROTOCOL_SHOP.md
// §8): the merchants in view, the shop session and its one request in flight, the shared potion
// cooldown, the quantity box, stock entries, the buy / sell decisions, every Turkish text of §8.3, the
// potion tooltip, the Esc order with the shop window, the automation parser and the merchant clips.
#include "client/world/Animator.h"
#include "client/world/ShopView.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace world = vo::client::world;
namespace vg = vo::game;
namespace vn = vo::net;
namespace assets = vo::assets;
using Catch::Approx;

namespace {

const vg::NpcCatalog& catalog()
{
    static const vg::NpcCatalog c = [] {
        vg::NpcCatalog out;
        std::string error;
        const char* text = R"([[npc]]
id = 1
name = "Sürmeli Bacı"
title = "Şerbetçi"
greeting = "Yolun açık olsun."
model = "npc_potion_esik"
zone = 1
x = 235.5
z = 250.5
yaw = 1.1
kind = "merchant"
stock = [16, 17]

[[npc]]
id = 2
name = "Demirci Ilgaz"
title = "Pazar Demircisi"
greeting = "Ne lazımsa söyle."
model = "npc_smith_esik"
zone = 1
x = 269.0
z = 239.0
yaw = -0.8
kind = "merchant"
stock = [1, 7]
)";
        if (!vg::parseNpcCatalog(text, "npcs.toml", nullptr, out, error))
            FAIL(error);
        return out;
    }();
    return c;
}

vn::EntitySpawn npcSpawn(std::uint32_t id, std::uint16_t npc, const char* name, float x, float z)
{
    vn::EntitySpawn s;
    s.entityId = id;
    s.entityType = vn::kEntityTypeNpc;
    s.name = name;
    s.templateId = npc;
    s.x = x;
    s.y = 3.0f;
    s.z = z;
    s.yaw = 0.5f;
    return s;
}

vg::ItemTemplate potion(vg::ItemTemplateId id, const char* name, std::uint32_t hp, std::uint32_t mp, std::uint32_t value)
{
    vg::ItemTemplate t;
    t.id = id;
    t.name = name;
    t.kind = vg::ItemKind::Consumable;
    t.slot = vg::SlotRule::None;
    t.maxStack = 20;
    t.value = value;
    t.restoreHp = hp;
    t.restoreMp = mp;
    return t;
}

vg::ItemTemplate weapon(vg::ItemTemplateId id, const char* name, std::uint8_t charClass, std::uint16_t level,
                        std::int32_t attack, std::uint32_t value)
{
    vg::ItemTemplate t;
    t.id = id;
    t.name = name;
    t.kind = vg::ItemKind::Weapon;
    t.slot = vg::SlotRule::MainHand;
    t.classMask = static_cast<std::uint8_t>(1u << charClass);
    t.level = level;
    t.attack = attack;
    t.value = value;
    return t;
}

constexpr std::uint8_t kWarrior = 1;
constexpr std::uint8_t kMage = 3;

} // namespace

TEST_CASE("Shop view: merchants in view come from ENTITY_SPAWN type 4 with their npcs.toml data", "[client][shop]")
{
    world::NpcBook book;
    CHECK(book.spawn(npcSpawn(500, 1, "Sürmeli Bacı", 235.5f, 250.5f), catalog(), nullptr));
    REQUIRE(book.size() == 1);
    const world::NpcEntity* n = book.find(500);
    REQUIRE(n != nullptr);
    REQUIRE(n->def != nullptr);
    CHECK(n->def->greeting == "Yolun açık olsun.");
    CHECK(n->title() == "Şerbetçi");
    CHECK(n->sellsPotions());
    CHECK(n->position.y == Approx(3.0f)); // no terrain: as sent
    // Other entity types are not merchants (players, monsters, loot bags go to the WorldModel).
    vn::EntitySpawn player = npcSpawn(501, 2, "Oyuncu", 240.0f, 250.0f);
    player.entityType = vn::kEntityTypePlayer;
    CHECK_FALSE(book.spawn(player, catalog(), nullptr));
    CHECK(book.size() == 1);
    // The smith; an unknown id is drawn without data (no model, the wire name).
    CHECK(book.spawn(npcSpawn(502, 2, "Demirci Ilgaz", 269.0f, 239.0f), catalog(), nullptr));
    CHECK_FALSE(book.find(502)->sellsPotions());
    CHECK(book.spawn(npcSpawn(503, 99, "Yabancı", 300.0f, 300.0f), catalog(), nullptr));
    CHECK(book.find(503)->def == nullptr);
    CHECK(book.find(503)->title().empty());
    // A spawn of a known id replaces it; despawn.
    CHECK(book.spawn(npcSpawn(500, 1, "Sürmeli Bacı", 236.0f, 251.0f), catalog(), nullptr));
    CHECK(book.size() == 3);
    CHECK(book.find(500)->position.x == Approx(236.0f));
    CHECK(book.spawns() == 4);
    // E: the nearest within reach; by name (automation).
    CHECK(book.nearest({237.0f, 0.0f, 252.0f}, 6.0f) == 500);
    CHECK(book.nearest({250.0f, 0.0f, 245.0f}, 6.0f) == 0);
    CHECK(book.nearest({262.0f, 0.0f, 241.0f}, 30.0f) == 502);
    CHECK(book.byName("Demirci Ilgaz") == 502);
    CHECK(book.byName("Yok") == 0);
    CHECK(book.despawn(503));
    CHECK_FALSE(book.despawn(503));
    CHECK(book.size() == 2);
    book.clear();
    CHECK(book.size() == 0);
}

TEST_CASE("Shop view: the session opens on the answer to its request and keeps one buy / sell in flight",
          "[client][shop]")
{
    world::ShopSession s;
    CHECK_FALSE(s.isOpen());
    CHECK_FALSE(s.beginBuy(16, 1)); // nothing open
    s.beginOpen(500);
    CHECK(s.opening() == 500);
    // A stale answer (another merchant) is ignored.
    vn::ShopOpenRes other{501, vn::ShopResult::Ok, {{16, 8}}};
    CHECK_FALSE(s.finishOpen(other));
    CHECK_FALSE(s.isOpen());
    // A refusal answers the request, nothing opens.
    vn::ShopOpenRes far{500, vn::ShopResult::OutOfRange, {}};
    CHECK(s.finishOpen(far));
    CHECK_FALSE(s.isOpen());
    CHECK(s.opening() == 0);
    s.beginOpen(500);
    vn::ShopOpenRes ok{500, vn::ShopResult::Ok, {{16, 8}, {17, 25}}};
    REQUIRE(s.finishOpen(ok));
    REQUIRE(s.isOpen());
    CHECK(s.npc() == 500);
    REQUIRE(s.offers().size() == 2);
    CHECK(s.offer(17)->price == 25);
    CHECK(s.offer(18) == nullptr);
    // One request at a time.
    const auto buy = s.beginBuy(16, 3);
    REQUIRE(buy);
    CHECK(buy->npcId == 500);
    CHECK(buy->templateId == 16);
    CHECK(buy->quantity == 3);
    CHECK(s.busy());
    CHECK_FALSE(s.beginBuy(16, 1));
    CHECK_FALSE(s.beginSell(0, 1, 1));
    // An answer with another seq is not ours; the right one ends it.
    CHECK_FALSE(s.finishBuy(vn::ShopBuyRes{buy->seq + 7, vn::ShopResult::Ok, 16, 3, 24}));
    CHECK_FALSE(s.finishSell(vn::ShopSellRes{buy->seq, vn::ShopResult::Ok, 16, 3, 24})); // a sell answer
    const auto r = s.finishBuy(vn::ShopBuyRes{buy->seq, vn::ShopResult::Ok, 16, 3, 24});
    REQUIRE(r);
    CHECK(r->op == world::ShopSession::Op::Buy);
    CHECK_FALSE(s.busy());
    // Sell: the request remembers the slot and what it held; closing keeps it in flight (its answer
    // still comes), a zone change forgets it.
    const auto sell = s.beginSell(4, 1, 1);
    REQUIRE(sell);
    CHECK(sell->bagSlot == 4);
    CHECK(sell->seq != buy->seq);
    s.close();
    CHECK_FALSE(s.isOpen());
    CHECK(s.busy());
    const auto sr = s.finishSell(vn::ShopSellRes{sell->seq, vn::ShopResult::Ok, 1, 1, 6});
    REQUIRE(sr);
    CHECK(sr->templateId == 1);
    CHECK(sr->bagSlot == 4);
    s.beginOpen(500);
    REQUIRE(s.finishOpen(ok));
    REQUIRE(s.beginBuy(17, 1));
    s.close(true);
    CHECK_FALSE(s.busy());
    CHECK(s.offers().empty());
}

TEST_CASE("Shop view: the shared potion cooldown sweeps from ITEM_COOLDOWN", "[client][shop]")
{
    world::ConsumableCooldown cd;
    CHECK_FALSE(cd.active(0.0));
    CHECK(cd.sweep(0.0) == 0.0f);
    cd.set(10'000, 10'000, 100.0);
    CHECK(cd.active(100.0));
    CHECK(cd.sweep(100.0) == Approx(1.0f));
    CHECK(cd.remaining(104.0) == Approx(6.0));
    CHECK(cd.sweep(105.0) == Approx(0.5f));
    CHECK_FALSE(cd.active(110.0));
    CHECK(cd.remaining(111.0) == 0.0);
    // After a relog the server sends what is left of the 10 s.
    cd.set(2'500, 10'000, 200.0);
    CHECK(cd.sweep(200.0) == Approx(0.25f));
    cd.clear();
    CHECK_FALSE(cd.active(200.0));
}

TEST_CASE("Shop view: the quantity box stays within 1..max", "[client][shop]")
{
    world::QuantityModel q(20, 1);
    CHECK(q.value == 1);
    q.down();
    CHECK(q.value == 1);
    q.up(5);
    CHECK(q.value == 6);
    CHECK(q.total(8) == 48);
    q.set(99);
    CHECK(q.value == 20);
    q.set(-3);
    CHECK(q.value == 1);
    world::QuantityModel whole(7, 7); // selling a stack: default the whole stack
    CHECK(whole.value == 7);
    CHECK(whole.total(4) == 28);
    world::QuantityModel zero(0, 0);
    CHECK(zero.max == 1);
    CHECK(zero.value == 1);
}

TEST_CASE("Shop view: stock entries show the price state and unmet requirements in red", "[client][shop]")
{
    const vg::ItemTemplate hp = potion(16, "Kızılca Şerbet", 70, 0, 8);
    const vg::ItemTemplate both = potion(30, "Karışık", 10, 20, 9);
    const vg::ItemTemplate keep = weapon(7, "Burç Kılıcı", kWarrior, 3, 9, 60);
    const vg::ItemUser warrior1{kWarrior, 1, {}};
    const vg::ItemUser warrior3{kWarrior, 3, {}};
    const vg::ItemUser mage5{kMage, 5, {}};
    auto v = world::shopEntryView(hp, 8, 8, warrior1);
    CHECK(v.affordable);
    CHECK(v.detail == "Can +70");
    CHECK_FALSE(v.detailUnmet);
    CHECK_FALSE(world::shopEntryView(hp, 8, 7, warrior1).affordable);
    CHECK(world::shopEntryView(both, 9, 0, warrior1).detail == "Can +10 · Mana +20");
    CHECK(world::shopEntryView(potion(18, "Şafak Çiyi", 0, 40, 8), 8, 0, warrior1).detail == "Mana +40");
    // Gear: too high a level, another class (it can still be bought); else what it gives.
    v = world::shopEntryView(keep, 60, 100, warrior1);
    CHECK(v.detail == "Gereken seviye: 3");
    CHECK(v.detailUnmet);
    v = world::shopEntryView(keep, 60, 100, mage5);
    CHECK(v.detail == "Yalnız Savaşçı");
    CHECK(v.detailUnmet);
    v = world::shopEntryView(keep, 60, 59, warrior3);
    CHECK(v.detail == "Saldırı +9 · Sv. 3");
    CHECK_FALSE(v.detailUnmet);
    CHECK_FALSE(v.affordable);
    CHECK(world::priceText(1234) == "1.234 Akçe");
    CHECK(world::balanceText(1234567) == "Akçe: 1.234.567");
}

TEST_CASE("Shop view: when buying and selling ask first, and the texts they ask with", "[client][shop]")
{
    const vg::ItemTemplate hp = potion(16, "Kızılca Şerbet", 70, 0, 8);
    const vg::ItemTemplate keep = weapon(7, "Burç Kılıcı", kWarrior, 3, 9, 60);
    const vg::ItemTemplate axe = weapon(6, "Ocak Baltası", kWarrior, 1, 10, 20);
    vg::ItemTemplate core;
    core.id = 11;
    core.name = "Kor Çekirdeği";
    core.kind = vg::ItemKind::Material;
    core.maxStack = 5;
    core.value = 40;
    core.rarity = vg::Rarity::Rare;
    vg::ItemTemplate mace = weapon(14, "Cüruf Topuzu", 4, 4, 7, 39);
    mace.rarity = vg::Rarity::Uncommon;
    // Buying: stacks ask for a quantity, gear above 50 Akçe for a confirmation.
    CHECK(world::buyNeedsQuantity(hp));
    CHECK_FALSE(world::buyNeedsQuantity(keep));
    CHECK(world::buyNeedsConfirm(keep, 60));
    CHECK_FALSE(world::buyNeedsConfirm(axe, 20));
    CHECK_FALSE(world::buyNeedsConfirm(keep, 50));
    CHECK_FALSE(world::buyNeedsConfirm(hp, 160)); // a stack: its quantity box shows the total
    CHECK(world::buyConfirmText("Burç Kılıcı", 60) == "Burç Kılıcı için 60 Akçe ödensin mi?");
    // Selling: a stack of more than one asks how many; rarer or dearer items ask.
    CHECK(world::sellNeedsQuantity(2));
    CHECK_FALSE(world::sellNeedsQuantity(1));
    CHECK(world::sellNeedsConfirm(keep));     // worth 60
    CHECK(world::sellNeedsConfirm(core));     // rare (and worth 40)
    CHECK(world::sellNeedsConfirm(mace));     // uncommon
    CHECK_FALSE(world::sellNeedsConfirm(axe)); // common, worth 20
    CHECK_FALSE(world::sellNeedsConfirm(hp));
    CHECK(world::sellConfirmText("Burç Kılıcı", 1, 30) == "Burç Kılıcı 30 Akçe'ye satılsın mı?");
    CHECK(world::sellConfirmText("Kor Çekirdeği", 3, 60) == "3 × Kor Çekirdeği 60 Akçe'ye satılsın mı?");
    CHECK(world::kBuyQuantityPrompt == "Kaç tane?");
    CHECK(world::kSellQuantityPrompt == "Kaç tane satılsın?");
    CHECK(world::buyTotalText(24) == "Toplam: 24 Akçe");
    CHECK(world::sellTotalText(1500) == "Alınacak: 1.500 Akçe");
    CHECK(world::kUnequipFirstText == "Önce çıkarmalısın.");
    // The bag tooltip while a shop is open.
    CHECK(world::sellTipText(keep, 1) == "Satış: 30 Akçe");
    CHECK(world::sellTipText(hp, 5) == "Satış: 4 Akçe (yığın 20 Akçe)");
    vg::ItemTemplate cheap = axe;
    cheap.value = 1;
    CHECK(world::sellTipText(cheap, 1) == "Satılamaz");
}

TEST_CASE("Shop view: the result texts of PROTOCOL_SHOP.md 8.3", "[client][shop]")
{
    using R = vn::ShopResult;
    CHECK(world::shopResultText(R::Ok).empty());
    CHECK(world::shopResultText(R::NotFound) == "Burada satıcı yok.");
    CHECK(world::shopResultText(R::YouAreDead) == "Ölüyken alışveriş yapamazsın.");
    CHECK(world::shopResultText(R::OutOfRange) == "Satıcıya daha yakın olmalısın.");
    CHECK(world::shopResultText(R::NotOpen) == "Önce satıcıyla konuş.");
    CHECK(world::shopResultText(R::NotInStock) == "Bu satıcı bunu satmıyor.");
    CHECK(world::shopResultText(R::BadQuantity) == "Bu miktar olmaz.");
    CHECK(world::shopResultText(R::NotEnoughCoins) == "Yeterli Akçen yok.");
    CHECK(world::shopResultText(R::BagFull) == "Çantanda yer yok.");
    CHECK(world::shopResultText(R::NoItem) == "Satılacak bir şey yok.");
    CHECK(world::shopResultText(R::CannotSell) == "Bu satılamaz.");
    CHECK(world::shopResultText(R::PurseFull) == "Kesen dolu; daha fazla Akçe taşıyamazsın.");
    CHECK(world::shopResultText(R::ServerError) == "Alışveriş kaydedilemedi, tekrar dene.");
    CHECK(world::buyOkText("Burç Kılıcı", 1, 60) == "Burç Kılıcı alındı (60 Akçe).");
    CHECK(world::buyOkText("Kızılca Şerbet", 3, 24) == "3 × Kızılca Şerbet alındı (24 Akçe).");
    CHECK(world::sellOkText("Er Kılıcı", 1, 6) == "Er Kılıcı satıldı (+6 Akçe).");
    CHECK(world::sellOkText("Kül Tozu", 7, 7) == "7 × Kül Tozu satıldı (+7 Akçe).");
    CHECK(world::shopClosedText(vn::ShopClosedReason::OutOfRange, "Sürmeli Bacı") == "Sürmeli Bacı çok uzakta kaldı.");
    CHECK(world::shopClosedText(vn::ShopClosedReason::YouDied, "Sürmeli Bacı").empty());
    const auto tip = world::npcTipLines("Sürmeli Bacı", "Şerbetçi");
    REQUIRE(tip.size() == 2);
    CHECK(tip[0] == "Sürmeli Bacı — Şerbetçi");
    CHECK(tip[1] == "Konuşmak için tıkla");
    CHECK(world::npcTitlePlate("Şerbetçi") == "‹Şerbetçi›");
    CHECK(world::npcTitlePlate("").empty());
}

TEST_CASE("Shop view: potions - bag count, tooltip, bar key line", "[client][shop]")
{
    const vg::ItemTemplate hp = [] {
        vg::ItemTemplate t = potion(16, "Kızılca Şerbet", 70, 0, 8);
        t.description = "Kırmızı bir şerbet.";
        return t;
    }();
    vg::Inventory inv;
    std::string error;
    REQUIRE(vg::Inventory::fromItems({{1, vg::ItemPlace::bag(0), 16, 5},
                                      {2, vg::ItemPlace::bag(3), 16, 20},
                                      {3, vg::ItemPlace::bag(4), 18, 2},
                                      {4, vg::ItemPlace::equipment(vg::EquipSlot::MainHand), 1, 1}},
                                     inv, error));
    CHECK(world::bagCount(inv, 16) == 25);
    CHECK(world::bagCount(inv, 18) == 2);
    CHECK(world::bagCount(inv, 17) == 0);
    CHECK(world::bagCount(inv, 0) == 0);
    const auto lines = world::consumableTipLines(hp, 5);
    std::vector<std::string> text;
    for (const auto& l : lines)
        text.push_back(l.text);
    REQUIRE(text.size() == 6);
    CHECK(lines[0].tone == world::TipTone::Title);
    CHECK(text[0] == "Kızılca Şerbet");
    CHECK(text[1] == "Tüketilebilir");
    CHECK(text[2] == "Can: +70");
    CHECK(text[3] == "Bekleme: 10 sn (bütün iksirler)");
    CHECK(text[4] == "Kırmızı bir şerbet.");
    CHECK(text[5] == "5 / 20");
    // On an empty bar slot the count shows 0 (red); in a loot row without a count it is left out.
    const auto zero = world::consumableTipLines(hp, 0, true);
    CHECK(zero.back().text == "0 / 20");
    CHECK(zero.back().tone == world::TipTone::Bad);
    CHECK(world::consumableTipLines(hp, 0).size() == 5);
    const auto mana = world::consumableTipLines(potion(18, "Şafak Çiyi", 0, 40, 8), 1);
    CHECK(mana[2].text == "Mana: +40");
    // The inventory / loot tooltip uses it for consumables.
    CHECK(world::itemTipLines(hp, {kWarrior, 1, {}}, 5)[1].text == "Tüketilebilir");
    CHECK(world::barUseText("2") == "Çubuktan kullan: 2");
    CHECK(world::restoreText(70) == "+70");
}

TEST_CASE("Shop view: Esc closes the loot window, then the shop, then the HUD order", "[client][shop]")
{
    using A = world::EscapeAction;
    CHECK(world::escapeActionShop(true, true, true, true, true, true, true, false) == A::CloseLoot);
    CHECK(world::escapeActionShop(false, true, true, true, true, true, true, false) == A::CloseShop);
    CHECK(world::escapeActionShop(false, false, true, true, true, true, true, false) == A::CloseControls);
    CHECK(world::escapeActionShop(false, false, false, true, true, true, true, false) == A::CloseSkills);
    CHECK(world::escapeActionShop(false, false, false, false, true, true, true, false) == A::CloseKunye);
    CHECK(world::escapeActionShop(false, false, false, false, false, true, true, false) == A::CloseInventory);
    CHECK(world::escapeActionShop(false, false, false, false, false, false, true, false) == A::ClearTarget);
    CHECK(world::escapeActionShop(false, false, false, false, false, false, false, false) == A::OpenMenu);
    CHECK(world::escapeActionShop(true, true, true, true, true, true, true, true) == A::None);
}

TEST_CASE("Shop view: the shop automation commands", "[client][shop]")
{
    std::string error;
    auto c = world::parseShopCommand("talk Sürmeli Bacı", error);
    REQUIRE(c);
    CHECK(c->verb == world::ShopVerb::Talk);
    CHECK(c->text == "Sürmeli Bacı");
    c = world::parseShopCommand("walktonpc Demirci Ilgaz 3.5", error);
    REQUIRE(c);
    CHECK(c->text == "Demirci Ilgaz");
    CHECK(c->lo == Approx(3.5));
    c = world::parseShopCommand("walktonpc Demirci Ilgaz", error);
    REQUIRE(c);
    CHECK(c->text == "Demirci Ilgaz");
    CHECK(c->lo == Approx(2.5));
    c = world::parseShopCommand("shopbuy 16 3", error);
    REQUIRE(c);
    CHECK(c->verb == world::ShopVerb::Buy);
    CHECK(c->a == 16);
    CHECK(c->hasB);
    CHECK(c->b == 3);
    c = world::parseShopCommand("shopbuy 7", error);
    REQUIRE(c);
    CHECK_FALSE(c->hasB);
    c = world::parseShopCommand("shopsell 2", error);
    REQUIRE(c);
    CHECK(c->verb == world::ShopVerb::Sell);
    CHECK(c->a == 2);
    c = world::parseShopCommand("expectshopmessage Yeterli Akçen yok.", error);
    REQUIRE(c);
    CHECK(c->text == "Yeterli Akçen yok.");
    c = world::parseShopCommand("expectbarcount 2 0", error);
    REQUIRE(c);
    CHECK(c->a == 2);
    CHECK(c->b == 0);
    c = world::parseShopCommand("expectpotioncd 8.5 10", error);
    REQUIRE(c);
    CHECK(c->hi == Approx(10.0));
    c = world::parseShopCommand("baritem 3 none", error);
    REQUIRE(c);
    CHECK(c->b == 0);
    c = world::parseShopCommand("usepotion 16 4", error);
    REQUIRE(c);
    CHECK(c->hasB);
    CHECK(c->b == 4);
    CHECK(world::parseShopCommand("shoplog", error));
    CHECK(world::parseShopCommand("expectshop none", error)->text == "none");
    // Not a shop verb: no error (the scene tries its other commands).
    CHECK_FALSE(world::parseShopCommand("camera behind 20 8", error));
    CHECK(error.empty());
    // Malformed: the problem is named.
    CHECK_FALSE(world::parseShopCommand("shopbuy", error));
    CHECK(error.find("shopbuy") != std::string::npos);
    CHECK_FALSE(world::parseShopCommand("shopsell 28", error));
    CHECK_FALSE(world::parseShopCommand("shopbuyforce 16", error));
    CHECK_FALSE(world::parseShopCommand("expectbarcount 9 1", error));
    CHECK_FALSE(world::parseShopCommand("expectpotioncd 5 2", error));
    CHECK_FALSE(world::parseShopCommand("baritem 1 potion", error));
    CHECK_FALSE(world::parseShopCommand("talk", error));
    CHECK_FALSE(world::parseShopCommand("shopclose now", error));
}

TEST_CASE("Animator: a merchant talks while its shop is open and reaches on a sale (M14)", "[client][shop][anim]")
{
    assets::MeshData mesh;
    assets::Bone root;
    root.name = "root";
    mesh.skeleton.bones = {root};
    mesh.vertexFormat = assets::kVertexNormal | assets::kVertexSkin;
    const auto clip = [](bool loop) {
        assets::AnimationClip c;
        c.sampleRate = 30.0f;
        c.frameCount = 31;
        c.duration = 1.0f;
        c.loop = loop;
        assets::AnimationTrack t;
        t.bone = "root";
        t.keys.assign(c.frameCount, assets::Transform{});
        c.tracks.push_back(t);
        return c;
    };
    std::vector<assets::AnimationClip> store = {clip(true), clip(true), clip(false)};
    world::CharacterClips clips;
    clips.set(world::AnimState::Idle, &store[0]);
    clips.set(world::AnimState::IdleTalking, &store[1]);
    clips.set(world::AnimState::Interact, &store[2]);
    clips.bind(mesh.skeleton);
    world::Animator a;
    a.bind(&mesh, &clips);
    const auto run = [&](float s) {
        for (float t = 0.0f; t < s; t += 1.0f / 60.0f)
            a.update(1.0f / 60.0f, 0.0f, false);
    };
    run(0.2f);
    CHECK(a.state() == world::AnimState::Idle);
    a.setTalking(true);
    run(0.1f);
    CHECK(a.state() == world::AnimState::IdleTalking);
    CHECK(std::string(world::animStateName(a.state())) == "idle_talking");
    // A sale: interact once, then back to the talking idle.
    REQUIRE(a.playInteract());
    CHECK(a.action() == world::AnimAction::Interact);
    CHECK(a.state() == world::AnimState::Interact);
    run(1.2f);
    CHECK(a.action() == world::AnimAction::None);
    run(0.1f);
    CHECK(a.state() == world::AnimState::IdleTalking);
    a.setTalking(false);
    run(0.1f);
    CHECK(a.state() == world::AnimState::Idle);
    // Without the clips (a player look): nothing happens.
    world::CharacterClips plain;
    plain.set(world::AnimState::Idle, &store[0]);
    plain.bind(mesh.skeleton);
    world::Animator b;
    b.bind(&mesh, &plain);
    CHECK_FALSE(b.playInteract());
    b.setTalking(true);
    b.update(0.1f, 0.0f, false);
    CHECK(b.state() == world::AnimState::Idle);
}

TEST_CASE("Animator: drink plays once when the look has the clip, never holds the walk (fix1)", "[client][shop][anim]")
{
    assets::MeshData mesh;
    assets::Bone root;
    root.name = "root";
    mesh.skeleton.bones = {root};
    mesh.vertexFormat = assets::kVertexNormal | assets::kVertexSkin;
    const auto clip = [](bool loop) {
        assets::AnimationClip c;
        c.sampleRate = 30.0f;
        c.frameCount = 31;
        c.duration = 1.0f;
        c.loop = loop;
        assets::AnimationTrack t;
        t.bone = "root";
        t.keys.assign(c.frameCount, assets::Transform{});
        c.tracks.push_back(t);
        return c;
    };
    std::vector<assets::AnimationClip> store = {clip(true), clip(true), clip(false)};
    world::CharacterClips clips;
    clips.set(world::AnimState::Idle, &store[0]);
    clips.set(world::AnimState::Walk, &store[1]);
    clips.set(world::AnimState::Drink, &store[2]);
    clips.bind(mesh.skeleton);
    CHECK(std::string(world::animStateName(world::AnimState::Drink)) == "drink");
    CHECK(world::characterClipName(world::CharacterClip::Drink) == "drink");
    world::Animator a;
    a.bind(&mesh, &clips);
    const auto run = [&](float s, float speed) {
        for (float t = 0.0f; t < s; t += 1.0f / 60.0f)
            a.update(1.0f / 60.0f, speed, false);
    };
    run(0.2f, 0.0f);
    REQUIRE(a.playDrink());
    CHECK(a.action() == world::AnimAction::Drink);
    CHECK(a.state() == world::AnimState::Drink);
    run(1.2f, 0.0f);
    CHECK(a.action() == world::AnimAction::None); // once, then the idle again
    // Moving during the gesture cuts it short at once: the walk takes over.
    REQUIRE(a.playDrink());
    run(0.3f, 1.5f);
    CHECK(a.action() == world::AnimAction::None);
    CHECK(a.state() == world::AnimState::Walk);
    // On the move no gesture starts.
    CHECK_FALSE(a.playDrink());
    // Without the clip (the art has not delivered it): silently nothing.
    world::CharacterClips plain;
    plain.set(world::AnimState::Idle, &store[0]);
    plain.bind(mesh.skeleton);
    world::Animator b;
    b.bind(&mesh, &plain);
    CHECK_FALSE(b.playDrink());
    CHECK(b.action() == world::AnimAction::None);
}

TEST_CASE("Shop stock list: a long stock scrolls by whole rows (fix1)", "[client][shop]")
{
    // 4 rows in plenty of room: no scrolling.
    world::ShopListLayout l = world::shopListLayout(4, 400.0f, 34.0f, 2.0f, 0);
    CHECK_FALSE(l.scrolls);
    CHECK(l.visible == 4);
    CHECK(l.first == 0);
    CHECK(l.height == 4.0f * 34.0f + 3.0f * 2.0f);
    // No limit: everything.
    CHECK(world::shopListLayout(24, 0.0f, 34.0f, 2.0f, 5).visible == 24);
    CHECK(world::shopListLayout(24, 0.0f, 34.0f, 2.0f, 5).first == 0);
    // 24 rows in 300 logical pixels: 8 rows (8 * 36 - 2 = 286), scrolls 0..16.
    l = world::shopListLayout(24, 300.0f, 34.0f, 2.0f, 0);
    CHECK(l.scrolls);
    CHECK(l.visible == 8);
    CHECK(l.maxFirst == 16);
    CHECK(l.height == 286.0f);
    CHECK(l.height <= 300.0f);
    CHECK(l.thumbTop == 0.0f);
    CHECK(l.thumbHeight >= world::kShopMinThumb);
    CHECK(l.thumbHeight < l.height);
    // Scrolled to the end (and clamped past it): the thumb at the bottom.
    l = world::shopListLayout(24, 300.0f, 34.0f, 2.0f, 99);
    CHECK(l.first == 16);
    CHECK(l.thumbTop + l.thumbHeight == l.height);
    // Halfway.
    l = world::shopListLayout(24, 300.0f, 34.0f, 2.0f, 8);
    CHECK(l.first == 8);
    CHECK(l.thumbTop > 0.0f);
    CHECK(l.thumbTop + l.thumbHeight < l.height);
    // The thumb dragged: back to a row index (round trip), clamped at both ends.
    for (std::size_t first : {std::size_t{0}, std::size_t{3}, std::size_t{8}, std::size_t{16}})
    {
        const world::ShopListLayout at = world::shopListLayout(24, 300.0f, 34.0f, 2.0f, first);
        CHECK(world::shopScrollForThumb(at, at.thumbTop) == first);
    }
    CHECK(world::shopScrollForThumb(l, -50.0f) == 0);
    CHECK(world::shopScrollForThumb(l, 5000.0f) == 16);
    // Very little room: still three rows (the window grows past its limit rather than show none).
    l = world::shopListLayout(10, 20.0f, 34.0f, 2.0f, 0);
    CHECK(l.visible == world::kShopMinVisibleRows);
    CHECK(l.scrolls);
    // An empty stock keeps one row's room; nothing scrolls.
    l = world::shopListLayout(0, 300.0f, 34.0f, 2.0f, 0);
    CHECK_FALSE(l.scrolls);
    CHECK(l.height == 34.0f);
}

TEST_CASE("Shop session: test entries only pad an open shop, without duplicates (fix1)", "[client][shop]")
{
    world::ShopSession s;
    const world::ShopOffer extra[] = {{3, 10}, {4, 12}};
    s.appendOffersForTesting(extra);
    CHECK(s.offers().empty()); // nothing open
    s.beginOpen(77);
    vn::ShopOpenRes res;
    res.npcId = 77;
    res.result = vn::ShopResult::Ok;
    res.entries = {{16, 8}, {3, 10}};
    REQUIRE(s.finishOpen(res));
    s.appendOffersForTesting(extra);
    REQUIRE(s.offers().size() == 3);
    CHECK(s.offers()[2].templateId == 4);
}
