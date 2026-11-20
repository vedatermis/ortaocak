"""M18 "Kuşanma" content step (docs/DEVELOPMENT_PLAN.md §39): one-shot generator, kept for reference.

Applied once to gamedata/items.toml, loot.toml, npcs.toml and quests.toml (the files stay hand-editable;
ids are stable forever). Running it again refuses (item 169 exists). What it does:

* every existing weapon gets its family's speed_ms, reach and damage range (sword / pala / axe / mace /
  dagger / staff / rod; the mean stays `attack`, so the M15/M16 numbers keep their meaning);
* the higher armour tiers (Ayazlı, Yeminli, Korlu) get bonus lines (max_hp / max_mp / dodge / crit);
* new items 169-303: seven new weapon families (greatsword, war hammer, halberd, spiked mace, wavy dagger,
  crystal staff, buckler) in five tiers (Sağlam 5, Ayazlı 9, Yeminli 13, Korlu 17, Ant 19), the Ant tier
  (19) of the eight old families and of the three armour lines, 65 accessories (rings, earrings,
  necklaces, belts in five tiers), two large potions and ten boss uniques;
* their sources: shops (basic tiers only), loot tables, a few quest rewards.

Hand edits after the run (the balance tests of tests/game/EquipmentTests.cpp and HuntingTests.cpp): Ant Palası
Güç +2 / Çeviklik +5 (was +3 / +6), Son Kıvılcım (162) crit = 10, Bekçi Kalkancığı defense = 7 - so the Ocak
Efendisi's and the Kor Bekçisi's uniques stay stronger than any ordinary item of their slot and level.

Run with any Python 3.11+ (tomllib), e.g. Blender's bundled one:
    "C:\\Program Files\\Blender Foundation\\Blender 5.3\\5.3\\python\\bin\\python.exe" tools/content/m18_items.py
"""
import os
import re
import sys
import tomllib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
GD = os.path.join(ROOT, "gamedata")

W, R, M, P = "warrior", "rogue", "mage", "priest"


def sword_w(level):
    """The one-handed sword's attack at a level (the M15 ladder: 11 at 5, 16 at 11, 22 at 17)."""
    return round(6 + 0.95 * level)


def shield_d(level):
    return round(3.4 + 0.62 * level)


def value_at(level):
    table = {5: 140, 8: 225, 9: 280, 11: 380, 13: 480, 14: 530, 17: 895, 19: 1160}
    return table[level]


# ---- Weapon families: speed (ms), reach (m, None = the class range), damage spread (share of attack),
# attack factor over the sword ladder, value factor. ----
FAMILY = {
    "sword": dict(speed=1600, reach=2.5, spread=0.15),
    "pala": dict(speed=1450, reach=2.5, spread=0.18),
    "axe": dict(speed=2200, reach=2.7, spread=0.20),
    "mace": dict(speed=1700, reach=2.4, spread=0.25),
    "dagger": dict(speed=1200, reach=2.2, spread=0.10),
    "staff": dict(speed=1800, reach=None, spread=0.12),
    "rod": dict(speed=1900, reach=2.9, spread=0.15),
    # M18 families
    "greatsword": dict(speed=2000, reach=2.8, spread=0.15),
    "hammer": dict(speed=2400, reach=2.6, spread=0.30),
    "halberd": dict(speed=2300, reach=3.3, spread=0.20),
    "spiked": dict(speed=1700, reach=2.4, spread=0.28),
    "wavy": dict(speed=1200, reach=2.2, spread=0.10),
    "crystal": dict(speed=1500, reach=None, spread=0.12),
}


def damage_range(attack, spread):
    lo = round(attack * (1.0 - spread))
    if lo >= attack:
        return None
    return [lo, 2 * attack - lo]


def family_of(item):
    """Family of an existing weapon template (by grip, slot and classes)."""
    g, slot, cls = item["grip"], item["slot"], item["classes"]
    if g == "dagger":
        return "dagger"
    if g == "axe2h":
        return "axe"
    if g == "mace":
        return "mace"
    if g == "staff":
        return "rod" if P in cls else "staff"
    if g == "sword":
        return "pala" if R in cls else "sword"
    return None


# ---- New weapon families (M18) ----
NEW_FAMILIES = [
    # key, display noun, slot, classes, model/icon, grip, attack factor, primary / secondary attribute
    dict(key="greatsword", noun="Büyük Kılıç", slot="two_hand", classes=[W], model="sword_broad", grip="axe2h",
         factor=1.6, attr=("strength", "vitality"), vfac=1.3,
         desc="İki elle savrulan uzun ağız; kalkan tutmayan elin hakkını verir."),
    dict(key="hammer", noun="Savaş Çekici", slot="two_hand", classes=[W], model="hammer_war", grip="axe2h",
         factor=1.6, attr=("strength", "vitality"), vfac=1.3,
         desc="Ağır başlı çekiç; yavaş iner, indiği yerde zırh bırakmaz."),
    dict(key="halberd", noun="Teber", slot="two_hand", classes=[W], model="halberd", grip="axe2h",
         factor=1.48, attr=("strength", "dexterity"), vfac=1.3,
         desc="Uzun saplı teber; düşmana herkesten önce erişir."),
    dict(key="spiked", noun="Çivili Topuz", slot="main_hand", classes=[W, P], model="mace_spiked", grip="mace",
         factor=0.85, attr=("vitality", "spirit"), vfac=0.93,
         desc="Başı çivi dolu topuz; isabeti dalgalı, vuruşu sert."),
    dict(key="wavy", noun="Dalgalı Hançer", slot="either_hand", classes=[R], model="dagger_twin", grip="dagger",
         factor=0.64, attr=("dexterity", "strength"), vfac=0.79,
         desc="Dalgalı ağzı yarayı açık tutan hafif hançer."),
    dict(key="crystal", noun="Billur Asa", slot="two_hand", classes=[M], model="staff_crystal", grip="staff",
         factor=1.12, attr=("intelligence", "spirit"), vfac=1.0,
         desc="Billur uçlu ince asa; büyüyü sık ama hafif savurur."),
    dict(key="buckler", noun="Kalkancık", slot="off_hand", classes=[W, P], model="shield_buckler", grip="shield",
         factor=None, attr=("dexterity", "vitality"), vfac=0.86,
         desc="Yumruk büyüklüğünde kalkan; darbeyi karşılamaktan çok savuşturur."),
]

TIERS = [
    # name, level, rarity, tint, attribute bonus (primary, secondary), description
    dict(name="Sağlam", level=5, rarity="common", tint=[214, 214, 222], attrs=(0, 0),
         desc="Yurt demircilerinin sağlam işi."),
    dict(name="Ayazlı", level=9, rarity="uncommon", tint=[172, 202, 236], attrs=(2, 0),
         desc="Ayaz suyunda su verilmiş."),
    dict(name="Yeminli", level=13, rarity="common", tint=[236, 214, 158], attrs=(0, 0),
         desc="Eşik ustalarının yeminli damgasını taşır."),
    dict(name="Korlu", level=17, rarity="rare", tint=[242, 146, 96], attrs=(5, 2),
         desc="Kor ocağında dövülmüş; hâlâ ılık."),
    dict(name="Ant", level=19, rarity="rare", tint=[238, 232, 208], attrs=(6, 3),
         desc="Ant Taşı'nın önünde yemin edilerek dövülmüş; eşi az bulunur."),
]

# Requirements of the two top tiers of the new items (base attributes: class start + spent points).
REQ = {
    # family key -> (attribute, value at Korlu, value at Ant)
    "greatsword": ("strength", 22, 25), "hammer": ("strength", 22, 25), "halberd": ("strength", 22, 25),
    "spiked": ("vitality", 16, 18), "wavy": ("dexterity", 24, 27), "crystal": ("intelligence", 24, 27),
    "buckler": ("vitality", 16, 18),
    # the Ant tier of the old families
    "sword": ("strength", 22, 25), "axe": ("strength", 22, 25), "mace": ("vitality", 16, 18),
    "dagger": ("dexterity", 24, 27), "pala": ("dexterity", 24, 27), "staff": ("intelligence", 24, 27),
    "rod": ("spirit", 18, 20), "shield": ("vitality", 16, 18),
}

ANT_OLD = [
    # family, noun, slot, classes, model, icon, grip, factor, attrs, value factor
    dict(key="sword", noun="Kılıcı", slot="main_hand", classes=[W], model="sword_ridge", icon="sword_ridge",
         grip="sword", factor=1.0, attr=("strength", "vitality"), vfac=1.0,
         desc="Sırtı yemin sözleriyle işlenmiş tek elli kılıç."),
    dict(key="axe", noun="Baltası", slot="two_hand", classes=[W], model="axe_bearded", icon="axe_bearded",
         grip="axe2h", factor=1.6, attr=("strength", "vitality"), vfac=1.3,
         desc="Sakallı ağzı Ant Taşı'nın tozuyla bilenmiş iki elli balta."),
    dict(key="mace", noun="Gürzü", slot="main_hand", classes=[W, P], model="mace_flanged", icon="mace_flanged",
         grip="mace", factor=0.85, attr=("vitality", "spirit"), vfac=0.93,
         desc="Kanatlı başı yemin halkalarıyla sarılmış gürz."),
    dict(key="dagger", noun="Hançeri", slot="either_hand", classes=[R], model="dagger_fang", icon="dagger_fang",
         grip="dagger", factor=0.64, attr=("dexterity", "strength"), vfac=0.79,
         desc="Kavisli, kısa ve sessiz; yemin bozanların korkusu."),
    dict(key="pala", noun="Palası", slot="either_hand", classes=[R], model="sword_curved", icon="sword_curved",
         grip="sword", factor=0.73, attr=("dexterity", "strength"), vfac=0.86,
         desc="Ay gibi eğri pala; iki elde ayrı ayrı şarkı söyler."),
    dict(key="staff", noun="Asası", slot="two_hand", classes=[M], model="staff_ritual", icon="staff_ritual",
         grip="staff", factor=1.12, attr=("intelligence", "spirit"), vfac=1.0,
         desc="Ucunda Ant Taşı'ndan bir kıymık ışıyan ayin asası."),
    dict(key="rod", noun="Değneği", slot="two_hand", classes=[P, M], model="staff_frost", icon="staff_frost",
         grip="staff", factor=1.0, attr=("spirit", "intelligence"), vfac=0.93,
         desc="Şifacıların yeminini taşıyan soluk değnek."),
    dict(key="shield", noun="Siperi", slot="off_hand", classes=[W, P], model="shield_tower", icon="shield_tower",
         grip="shield", factor=None, attr=("vitality", "strength"), vfac=0.86,
         desc="Ant Taşı gibi dik duran kule kalkan."),
]

ARMOR_LINES = [
    # line, classes, defence factor, primary attr, bonus line key, models prefix, icon prefix, names per slot,
    # requirement
    dict(line="heavy", classes=[W, P], factor=1.0, attr="vitality", second="strength", pool="max_hp",
         names=["Ant Tolgası", "Ant Göğüs Zırhı", "Ant Dizliği", "Ant Demir Eldiveni", "Ant Demir Çizmesi"],
         req=("vitality", 20), tint=[232, 226, 206]),
    dict(line="leather", classes=[R, W, M], factor=0.7, attr="dexterity", second="vitality", pool="dodge",
         names=["Ant Deri Başlığı", "Ant Deri Yeleği", "Ant Tozluğu", "Ant Deri Eldiveni", "Ant Çarığı"],
         req=("dexterity", 20), tint=[176, 142, 104]),
    dict(line="cloth", classes=[M, P, R], factor=0.45, attr="intelligence", second="spirit", pool="max_mp",
         names=["Ant Sarığı", "Ant Cübbesi", "Ant Şalvarı", "Ant Bez Eldiveni", "Ant Pabucu"],
         req=("spirit", 18), tint=[236, 222, 184]),
]
ARMOR_SLOTS = ["head", "chest", "legs", "hands", "feet"]
ARMOR_SHARE = {"head": 0.15, "chest": 0.35, "legs": 0.25, "hands": 0.125, "feet": 0.125}

# ---- Accessories ----
ACC_TIERS = [
    dict(name="Eski", level=3, rarity="common", t=1, tint=(176, 166, 150), value=1.0,
         desc="Pazarda bulunur, çoğu bir dededen kalmadır."),
    dict(name="Sağlam", level=7, rarity="common", t=2, tint=(208, 210, 216), value=2.8,
         desc="Yurt kuyumcularının sağlam işi."),
    dict(name="Ayazlı", level=11, rarity="uncommon", t=3, tint=(172, 202, 236), value=6.0,
         desc="Ayaz suyunda soğutulmuş; teni serin tutar."),
    dict(name="Yeminli", level=15, rarity="uncommon", t=4, tint=(236, 214, 158), value=10.0,
         desc="Üstünde Eşik'in yemin damgası var."),
    dict(name="Korlu", level=19, rarity="rare", t=5, tint=(242, 146, 96), value=16.0,
         desc="Kor ocağının ışığını içinde saklar."),
]


def half(t):
    return (t + 1) // 2


# flavour key, slot, noun, icon, flavour colour, bonus lines as a function of the tier t (1..5), base value
ACC_KINDS = [
    dict(key="ring_str", slot="ring", noun="Kaya Yüzüğü", icon="acc_ring_band", color=(232, 150, 140),
         lines=lambda t: {"strength": t}, value=34, desc="Kaya gibi ağır bir halka; kolu güçlendirir."),
    dict(key="ring_dex", slot="ring", noun="Rüzgâr Yüzüğü", icon="acc_ring_band", color=(160, 220, 170),
         lines=lambda t: {"dexterity": t}, value=34, desc="İnce, hafif bir halka; parmakları çevikleştirir."),
    dict(key="ring_int", slot="ring", noun="Köz Yüzüğü", icon="acc_ring_signet", color=(250, 180, 120),
         lines=lambda t: {"intelligence": t}, value=34, desc="Taşında küçük bir köz parlar; zihni keskinleştirir."),
    dict(key="ring_spi", slot="ring", noun="Şafak Yüzüğü", icon="acc_ring_signet", color=(176, 210, 250),
         lines=lambda t: {"spirit": t}, value=34, desc="Şafak renkli taşlı halka; iradeyi sağlamlaştırır."),
    dict(key="ring_vit", slot="ring", noun="Meşe Yüzüğü", icon="acc_ring_signet", color=(196, 160, 120),
         lines=lambda t: {"vitality": t, "max_hp": 5 * t}, value=36,
         desc="Meşe özüyle sertleştirilmiş halka; bedeni dayanıklı kılar."),
    dict(key="ear_crit", slot="earring", noun="Kıvılcım Küpesi", icon="acc_earring_drop", color=(250, 200, 120),
         lines=lambda t: {"dexterity": half(t), "crit": 3 * t}, value=40,
         desc="Damla taşı kıvılcım gibi titrer; vuruşu yerine oturtur."),
    dict(key="ear_mp", slot="earring", noun="Çiy Küpesi", icon="acc_earring_hoop", color=(170, 220, 250),
         lines=lambda t: {"spirit": half(t), "max_mp": 10 * t}, value=40,
         desc="Halkasında sabah çiyi birikir; manayı besler."),
    dict(key="ear_dodge", slot="earring", noun="Yankı Küpesi", icon="acc_earring_hoop", color=(210, 200, 240),
         lines=lambda t: {"vitality": half(t), "dodge": 3 * t}, value=40,
         desc="Takanın kulağına darbeden önce bir yankı gelir."),
    dict(key="neck_hp", slot="necklace", noun="Kurt Dişi Kolye", icon="acc_necklace_chain", color=(222, 214, 196),
         lines=lambda t: {"vitality": half(t), "max_hp": 15 * t}, value=44,
         desc="Ucunda bir kurt dişi sallanır; yorgunluğu uzak tutar."),
    dict(key="neck_mp", slot="necklace", noun="Ay Taşı Kolye", icon="acc_necklace_chain", color=(190, 206, 250),
         lines=lambda t: {"intelligence": half(t), "max_mp": 15 * t}, value=44,
         desc="Ay taşı gece bile ışır; düşünceyi berrak tutar."),
    dict(key="neck_crit", slot="necklace", noun="Köz Muskası", icon="acc_necklace_torc", color=(244, 170, 120),
         lines=lambda t: {"strength": half(t), "crit": 2 * t}, value=44,
         desc="Boyuna dolanan bükme gerdanlık; içinde bir köz kırığı saklı."),
    dict(key="belt_def", slot="belt", noun="Örme Kemer", icon="acc_belt_plated", color=(210, 214, 224),
         lines=lambda t: {"vitality": half(t), "max_hp": 5 * t}, defense=lambda t: t, value=40,
         desc="Demir levhalı örme kemer; beli ve böğrü korur."),
    dict(key="belt_dodge", slot="belt", noun="Deri Kuşak", icon="acc_belt_leather", color=(200, 160, 120),
         lines=lambda t: {"dexterity": half(t), "dodge": 3 * t}, defense=lambda t: half(t), value=40,
         desc="Yumuşak deri kuşak; dönüşleri hafifletir."),
]


def mix(a, b, k=0.35):
    return [round(a[i] * (1 - k) + b[i] * k) for i in range(3)]


# ---- Armour bonus lines for the existing tiers (by template id) ----
def armour_lines():
    out = {}
    tiers = {100: 1, 115: 2, 130: 3}  # first id of Ayazlı (9), Yeminli (13), Korlu (17); 15 ids each
    for first, k in tiers.items():
        for line_i, line in enumerate(["heavy", "leather", "cloth"]):
            for slot_i, slot in enumerate(ARMOR_SLOTS):
                tid = first + line_i * 5 + slot_i
                pool = {"heavy": "max_hp", "cloth": "max_mp"}.get(line)
                weights = {"chest": 1.0, "legs": 0.6, "head": 0.4, "hands": 0.25, "feet": 0.25}
                lines = {}
                if pool:
                    base = {1: 15, 2: 25, 3: 40}[k]
                    v = round(base * weights[slot] / 5) * 5
                    if v > 0 and (k > 1 or slot in ("chest", "legs")):
                        lines[pool] = v
                else:
                    dodge = {1: 5, 2: 8, 3: 10}[k]
                    if slot == "chest":
                        lines["dodge"] = dodge
                    elif slot == "legs" and k >= 2:
                        lines["dodge"] = dodge - 3
                    elif slot == "hands" and k >= 2:
                        lines["crit"] = 2 + 2 * k - 2
                    elif slot in ("head", "feet") and k == 3:
                        lines["dodge" if slot == "feet" else "crit"] = 4
                if k == 3 and line == "leather" and slot == "chest":
                    lines["crit"] = 5
                if lines:
                    out[tid] = lines
    return out


# ---- Existing items: speed / reach / damage ----
def existing_weapon_keys(items):
    keys = {}
    for it in items:
        if it["kind"] != "weapon":
            continue
        fam = family_of(it)
        f = FAMILY[fam]
        lines = [f"speed_ms = {f['speed']}"]
        if f["reach"] is not None:
            lines.append(f"reach = {f['reach']}")
        dr = damage_range(it["attack"], f["spread"])
        if dr:
            lines.insert(0, f"damage = [{dr[0]}, {dr[1]}]")
        keys[it["id"]] = lines
    return keys


# ---- Emitting new items ----
def toml_list(v):
    return "[" + ", ".join(f'"{x}"' if isinstance(x, str) else str(x) for x in v) + "]"


def emit(item):
    order = ["id", "name", "description", "kind", "slot", "classes", "level", "attack", "damage", "speed_ms", "reach",
             "defense", "model", "icon", "grip", "max_stack", "value", "rarity", "strength", "dexterity", "vitality",
             "intelligence", "spirit", "max_hp", "max_mp", "crit", "dodge", "req_strength", "req_dexterity",
             "req_vitality", "req_intelligence", "req_spirit", "restore_hp", "restore_mp", "tint"]
    assert set(item) <= set(order), set(item) - set(order)
    out = ["[[item]]"]
    for k in order:
        if k not in item:
            continue
        v = item[k]
        if isinstance(v, str):
            out.append(f'{k} = "{v}"')
        elif isinstance(v, list):
            out.append(f"{k} = {toml_list(v)}")
        else:
            out.append(f"{k} = {v}")
    return "\n".join(out) + "\n"


def weapon_item(tid, name, desc, fam_key, slot, classes, level, attack, model, icon, grip, value, rarity, tint,
                attrs=None, lines=None, req=None):
    it = dict(id=tid, name=name, description=desc, kind="weapon", slot=slot, classes=classes, level=level,
              attack=attack, defense=0, model=model, icon=icon, grip=grip, value=value, rarity=rarity, tint=tint)
    f = FAMILY[fam_key]
    dr = damage_range(attack, f["spread"])
    if dr:
        it["damage"] = dr
    it["speed_ms"] = f["speed"]
    if f["reach"] is not None:
        it["reach"] = f["reach"]
    for k, v in (attrs or {}).items():
        if v:
            it[k] = v
    for k, v in (lines or {}).items():
        if v:
            it[k] = v
    if req:
        it["req_" + req[0]] = req[1]
    return it


def shield_item(tid, name, desc, classes, level, defense, model, value, rarity, tint, attrs=None, lines=None, req=None):
    it = dict(id=tid, name=name, description=desc, kind="shield", slot="off_hand", classes=classes, level=level,
              attack=0, defense=defense, model=model, icon=model, grip="shield", value=value, rarity=rarity,
              tint=tint)
    for k, v in (attrs or {}).items():
        if v:
            it[k] = v
    for k, v in (lines or {}).items():
        if v:
            it[k] = v
    if req:
        it["req_" + req[0]] = req[1]
    return it


def build_new_items():
    items = []  # (id, dict, sources-key)
    groups = {}  # name -> list of ids
    tid = 169

    def add(group, it):
        items.append(it)
        groups.setdefault(group, []).append(it["id"])

    # 169-203: new families x tiers (tier-major: all families of a tier together)
    for ti, tier in enumerate(TIERS):
        for fam in NEW_FAMILIES:
            lv = tier["level"]
            name = f"{tier['name']} {fam['noun']}"
            desc = f"{fam['desc']} {tier['desc']}"
            p_attr, s_attr = fam["attr"]
            attrs = {p_attr: tier["attrs"][0], s_attr: tier["attrs"][1]}
            lines = {}
            req = None
            if tier["name"] in ("Korlu", "Ant"):
                r = REQ[fam["key"]]
                req = (r[0], r[1] if tier["name"] == "Korlu" else r[2])
            if tier["name"] == "Ant":
                lines = {"max_hp": 20} if W in fam["classes"] else ({"max_mp": 25} if M in fam["classes"] else {"crit": 8})
            if fam["key"] == "wavy" and lv >= 9:
                lines["crit"] = lines.get("crit", 0) + {9: 5, 13: 6, 17: 8, 19: 4}[lv]
            value = round(value_at(lv) * fam["vfac"] / 5) * 5
            group = f"newfam_{tier['name']}"
            if fam["key"] == "buckler":
                d = max(1, round(0.6 * shield_d(lv)))
                lines["dodge"] = 6 + lv // 2
                add(group, shield_item(tid, name, desc, fam["classes"], lv, d, "shield_buckler", value, tier["rarity"],
                                       tier["tint"], attrs, lines, req))
            else:
                attack = round(sword_w(lv) * fam["factor"])
                add(group, weapon_item(tid, name, desc, fam["key"], fam["slot"], fam["classes"], lv, attack,
                                       fam["model"], fam["model"], fam["grip"], value, tier["rarity"], tier["tint"],
                                       attrs, lines, req))
            tid += 1
    assert tid == 204, tid
    # 204-211: the Ant tier of the old families
    ant = TIERS[4]
    for fam in ANT_OLD:
        lv = 19
        name = f"Ant {fam['noun']}"
        p_attr, s_attr = fam["attr"]
        attrs = {p_attr: 6, s_attr: 3}
        r = REQ[fam["key"]]
        lines = {"max_hp": 20} if W in fam["classes"] else ({"max_mp": 25} if M in fam["classes"] else {"crit": 8})
        if fam["key"] == "dagger":
            lines["crit"] = 12
        value = round(value_at(lv) * fam["vfac"] / 5) * 5
        if fam["key"] == "shield":
            add("ant_old", shield_item(tid, name, fam["desc"] + " " + ant["desc"], fam["classes"], lv, shield_d(lv) + 1,
                                       fam["model"], value, "rare", ant["tint"], attrs, {"max_hp": 25}, (r[0], r[2])))
        else:
            attack = round(sword_w(lv) * fam["factor"])
            add("ant_old", weapon_item(tid, name, fam["desc"] + " " + ant["desc"], fam["key"], fam["slot"],
                                       fam["classes"], lv, attack, fam["model"], fam["icon"], fam["grip"], value,
                                       "rare", ant["tint"], attrs, lines, (r[0], r[2])))
        tid += 1
    assert tid == 212, tid
    # 212-226: Ant armour (heavy target set defence 24 at 19)
    for line in ARMOR_LINES:
        for si, slot in enumerate(ARMOR_SLOTS):
            d = max(1, round(24 * ARMOR_SHARE[slot] * line["factor"]))
            share = {"chest": 1.0, "legs": 0.6, "head": 0.4, "hands": 0.3, "feet": 0.3}[slot]
            attrs = {line["attr"]: max(1, round(5 * share)), line["second"]: 2 if slot == "chest" else 0}
            if line["pool"] == "dodge":
                lines = {"dodge": max(3, round(12 * share))}
                if slot in ("hands", "head"):
                    lines["crit"] = 6
            else:
                lines = {line["pool"]: round(55 * share / 5) * 5}
            value = round({"chest": 520, "legs": 370, "head": 300, "hands": 220, "feet": 220}[slot] *
                          {"heavy": 1.0, "leather": 0.85, "cloth": 0.75}[line["line"]] / 5) * 5
            it = dict(id=tid, name=line["names"][si],
                      description=f"{ {'heavy': 'Kalın levhalar', 'leather': 'Sertleştirilmiş deri', 'cloth': 'Ağır dokuma'}[line['line']] }; "
                                  f"{ant['desc'][0].lower() + ant['desc'][1:]}",
                      kind="armor", slot=slot, classes=line["classes"], level=19, attack=0, defense=d,
                      model=f"{line['line']}_b_{slot}", icon=f"armor_{line['line']}_{slot}", value=value,
                      rarity="rare", tint=line["tint"])
            for k, v in attrs.items():
                if v:
                    it[k] = v
            for k, v in lines.items():
                if v:
                    it[k] = v
            it["req_" + line["req"][0]] = line["req"][1]
            add("ant_armor", it)
            tid += 1
    assert tid == 227, tid
    # 227-291: accessories (tier-major)
    for tier in ACC_TIERS:
        t = tier["t"]
        for kind in ACC_KINDS:
            lines = kind["lines"](t)
            it = dict(id=tid, name=f"{tier['name']} {kind['noun']}", description=f"{kind['desc']} {tier['desc']}",
                      kind="accessory", slot=kind["slot"], classes=[W, R, M, P], level=tier["level"], attack=0,
                      defense=kind["defense"](t) if "defense" in kind else 0, icon=kind["icon"],
                      value=round(kind["value"] * tier["value"] / 5) * 5 if tier["value"] > 1 else kind["value"],
                      rarity=tier["rarity"], tint=mix(tier["tint"], kind["color"]))
            it.update(lines)
            add(f"acc_{tier['name']}", it)
            tid += 1
    assert tid == 292, tid
    # 292-293: large potions
    add("potions", dict(id=292, name="Kor Şerbeti",
                        description="Ocak şerbetinin közde koyulaşmış hâli. İçen, kan kaybını bir solukta unutur.",
                        kind="consumable", icon="potion_hp_medium", max_stack=20, value=110, restore_hp=600,
                        tint=[220, 96, 80]))
    add("potions", dict(id=293, name="Kırağı Damlası",
                        description="Demirağız'ın en yüksek kayalarından toplanan kırağı. Manayı tepeden tırnağa doldurur.",
                        kind="consumable", icon="potion_mp_medium", max_stack=20, value=110, restore_mp=340,
                        tint=[130, 200, 255]))
    # 294-303: uniques
    U = [255, 214, 140]
    uniq = []
    uniq.append(("u_bekci", dict(id=294, name="Bekçinin Kor Halkası",
                                 description="Kor Bekçisinin parmağında yıllarca ısınmış halka.",
                                 kind="accessory", slot="ring", classes=[W, R, M, P], level=6, attack=0, defense=0,
                                 icon="acc_ring_signet", value=0, rarity="unique", tint=U, strength=2, dexterity=2,
                                 intelligence=2, spirit=2, max_hp=20)))
    uniq.append(("u_bekci", shield_item(295, "Bekçi Kalkancığı",
                                        "Bekçinin sol elinde taşıdığı küçük kalkan; kor kıvılcımlarını savuşturur.",
                                        [W, P], 6, 6, "shield_buckler", 0, "unique", U, {"vitality": 4},
                                        {"dodge": 15})))
    uniq.append(("u_aga", dict(id=296, name="Ağanın Başak Kuşağı",
                               description="Anız Ağası'nın belinden çözülen, başak örgülü kuşak.",
                               kind="accessory", slot="belt", classes=[W, R, M, P], level=10, attack=0, defense=3,
                               icon="acc_belt_leather", value=0, rarity="unique", tint=U, vitality=3, max_hp=40)))
    uniq.append(("u_aga", weapon_item(297, "Ağanın Dalgalı Hançeri",
                                      "Ağanın tırpanından kopan dalgalı ağızdan dövülmüş hançer.", "wavy",
                                      "either_hand", [R], 10, 10, "dagger_twin", "dagger_twin", "dagger", 0,
                                      "unique", U, {"dexterity": 5, "strength": 1}, {"crit": 10})))
    uniq.append(("u_han", dict(id=298, name="Hanın Buz Küpesi",
                               description="Körük Hanı'nın kulağında donmuş bir damla; hiç erimez.",
                               kind="accessory", slot="earring", classes=[W, R, M, P], level=10, attack=0, defense=0,
                               icon="acc_earring_drop", value=0, rarity="unique", tint=U, intelligence=3, spirit=3,
                               max_mp=40)))
    uniq.append(("u_han", weapon_item(299, "Han Billuru", "Hanın körüğünün dibinde büyüyen billurdan asa.",
                                      "crystal", "two_hand", [M], 10, 16, "staff_crystal", "staff_crystal", "staff",
                                      0, "unique", U, {"intelligence": 5, "spirit": 3}, {"max_mp": 30})))
    uniq.append(("u_ana", dict(id=300, name="Ananın Cüruf Gerdanlığı",
                               description="Cüruf Anası'nın boynundan sökülen ağır gerdanlık.",
                               kind="accessory", slot="necklace", classes=[W, R, M, P], level=15, attack=0, defense=0,
                               icon="acc_necklace_torc", value=0, rarity="unique", tint=U, vitality=4, max_hp=80)))
    uniq.append(("u_ana", weapon_item(301, "Döküm Çekici", "Cüruf Gölü'nün dökümünden çıkan, ağırlığı insafsız çekiç.",
                                      "hammer", "two_hand", [W], 15, 33, "hammer_war", "hammer_war", "axe2h", 0,
                                      "unique", U, {"strength": 6, "vitality": 3}, {"max_hp": 30})))
    uniq.append(("u_ocak", dict(id=302, name="Ocak Mührü",
                                description="Ocak Efendisinin buyruklarını mühürlediği yüzük.",
                                kind="accessory", slot="ring", classes=[W, R, M, P], level=20, attack=0, defense=0,
                                icon="acc_ring_signet", value=0, rarity="unique", tint=U, strength=3, dexterity=3,
                                intelligence=3, spirit=3, crit=10)))
    uniq.append(("u_ocak", weapon_item(303, "Efendinin Teberi", "Ocak Tahtı'nın önünde nöbet tutan teber.", "halberd",
                                       "two_hand", [W], 20, 37, "halberd", "halberd", "axe2h", 0, "unique", U,
                                       {"strength": 9, "vitality": 4}, {"max_hp": 40})))
    for g, it in uniq:
        add(g, it)
    return items, groups


# ---- File edits ----
def edit_items(text, items, new_items):
    if re.search(r"^id = 169$", text, re.M):
        sys.exit("items.toml already has item 169: the M18 step was applied")
    weapon_keys = existing_weapon_keys(items)
    armour = armour_lines()
    out = []
    current = None
    for line in text.split("\n"):
        m = re.match(r"^id = (\d+)$", line)
        if m:
            current = int(m.group(1))
        out.append(line)
        if current in weapon_keys and line.startswith("attack = "):
            out.extend(weapon_keys[current])
        if current in armour and line.startswith("defense = "):
            for k, v in armour[current].items():
                out.append(f"{k} = {v}")
    text = "\n".join(out)
    marker = "# Starter kits: granted once per character"
    assert marker in text
    block = ["# ---- M18 \"Kuşanma\" (docs/DEVELOPMENT_PLAN.md §39, docs/PROTOCOL_EQUIPMENT.md): seven new weapon",
             "# families in five tiers (169-203), the Ant tier (19) of the old families (204-211) and of the armour",
             "# (212-226), accessories (227-291: kind \"accessory\", never drawn on the character), two large potions",
             "# (292-293) and ten boss uniques (294-303). Generated once by tools/content/m18_items.py. ----", ""]
    for it in new_items:
        block.append(emit(it))
    text = text.replace(marker, "\n".join(block) + marker)
    return text


def add_drops(text, monsters_key, drops, names):
    """Appends [[table.drop]] blocks to the table whose `monsters = ...` line is `monsters_key`."""
    lines = text.split("\n")
    start = lines.index(monsters_key)
    end = len(lines)
    for i in range(start + 1, len(lines)):
        if lines[i] == "[[table]]":
            end = i
            while end > 0 and (lines[end - 1].startswith("#") or lines[end - 1] == ""):
                end -= 1
            break
    else:
        while end > 0 and lines[end - 1] == "":
            end -= 1
    block = [""]
    for item, chance, count in drops:
        block.append("[[table.drop]]")
        block.append(f"item = {item:<7d}# {names[item]} (M18)")
        block.append(f"chance = {chance:g}")
        if count:
            block.append(f"count = [{count[0]}, {count[1]}]")
        block.append("")
    block = block[:-1]
    lines[end:end] = block
    return "\n".join(lines)


def edit_loot(text, groups, names, existing_uniques):
    g = groups
    N = lambda tier: g[f"newfam_{tier}"]
    acc = lambda tier: g[f"acc_{tier}"]
    plan = {
        # Kül Çömezi / Anız Yanığı / Kırağı Kayası (1-2), Cüruf Yürüyen / Yanık Nöbetçi / Donuk Ocakçı (3-4)
        "monsters = [1, 2]": [(i, 0.002, None) for i in acc("Eski")],
        "monsters = [6, 7]": [(i, 0.002, None) for i in acc("Eski")],
        "monsters = [10, 11]": [(i, 0.002, None) for i in acc("Eski")],
        "monsters = [3, 4]": [(i, 0.003, None) for i in acc("Eski")],
        "monsters = [8, 9]": [(i, 0.003, None) for i in acc("Eski")],
        "monsters = [12, 13]": [(i, 0.003, None) for i in acc("Eski")],
        # Kavruk Tırpancı / Buzul Körükçü (5-7)
        "monsters = [14, 15, 16]": [(i, 0.003, None) for i in N("Sağlam")] + [(i, 0.003, None) for i in acc("Sağlam")],
        "monsters = [20, 21, 22]": [(i, 0.003, None) for i in N("Sağlam")] + [(i, 0.003, None) for i in acc("Sağlam")],
        # Harman Devi / Örs Kıran (8-10)
        "monsters = [17, 18, 19]": [(i, 0.004, None) for i in N("Ayazlı")],
        "monsters = [23, 24, 25]": [(i, 0.004, None) for i in N("Ayazlı")],
        # İs Avcısı (11-13), Cüruf Zırhlısı (14-16), Kor Büyücüsü (17-19)
        "monsters = [26, 27, 28]": [(i, 0.003, None) for i in N("Yeminli")] + [(i, 0.003, None) for i in acc("Ayazlı")],
        "monsters = [29, 30, 31]": [(i, 0.003, None) for i in acc("Yeminli")] + [(292, 0.03, None), (293, 0.02, None)],
        "monsters = [32, 33, 34]": [(i, 0.002, None) for i in N("Korlu")] + [(i, 0.0015, None) for i in acc("Korlu")] +
                                   [(i, 0.0008, None) for i in N("Ant") + g["ant_old"]] +
                                   [(i, 0.0008, None) for i in g["ant_armor"]] + [(292, 0.04, None), (293, 0.03, None)],
        # bosses
        "monsters = [5]": [(i, 0.04, None) for i in g["u_bekci"]],
        "monsters = [36]": [(i, 0.04, None) for i in g["u_aga"]] + [(i, 0.08, None) for i in N("Ayazlı")] +
                           [(i, 0.05, None) for i in acc("Ayazlı")],
        "monsters = [37]": [(i, 0.04, None) for i in g["u_han"]] + [(i, 0.08, None) for i in N("Ayazlı")] +
                           [(i, 0.05, None) for i in acc("Ayazlı")],
        "monsters = [38]": [(i, 0.04, None) for i in g["u_ana"]] + [(i, 0.06, None) for i in N("Yeminli")] +
                           [(i, 0.05, None) for i in acc("Yeminli")] + [(292, 1.0, (1, 2))],
        "monsters = [35]": [(i, 0.03, None) for i in g["u_ocak"]] + [(i, 0.05, None) for i in N("Korlu")] +
                           [(i, 0.03, None) for i in N("Ant") + g["ant_old"]] +
                           [(i, 0.025, None) for i in g["ant_armor"]] + [(i, 0.04, None) for i in acc("Korlu")],
    }
    for key, drops in plan.items():
        text = add_drops(text, key, drops, names)
    # The old uniques' chances: a boss now has 6-8 uniques, so each line drops 4-5 % (one or more per kill
    # stays ~20-25 %, docs/DEVELOPMENT_PLAN.md §39).
    new_chance = {5: 0.04, 36: 0.04, 37: 0.035, 38: 0.04, 35: 0.03}
    lines = text.split("\n")
    table_monster = None
    for i, line in enumerate(lines):
        m = re.match(r"^monsters = \[(\d+)", line)
        if m:
            table_monster = int(m.group(1))
        m = re.match(r"^item = (\d+)", line)
        if m and int(m.group(1)) in existing_uniques and table_monster in new_chance:
            j = i + 1
            assert lines[j].startswith("chance = ")
            lines[j] = f"chance = {new_chance[table_monster]:g}"
    text = "\n".join(lines)
    head = "# M16 \"Tav\": every table ends with its upgrade stone lines"
    assert head in text
    text = text.replace(head, "# M18 \"Kuşanma\": the new weapons, the Ant tier, the accessories and the ten new uniques are appended\n"
                              "# to the end of the tables (tools/content/m18_items.py; at most 80 lines per table now). Boss uniques\n"
                              "# 3-4 % per line (6-8 per boss), so a kill holds at least one in ~20-25 %.\n" + head)
    text = text.replace("# At most 32 drop lines per table (M16; 24 in M15, 8 before)",
                        "# At most 80 drop lines per table (M18; 32 in M16, 24 in M15, 8 before)")
    return text


def edit_npcs(text, groups):
    g = groups

    def extend(stock_head, ids):
        nonlocal text
        i = text.index(stock_head)
        j = text.index("]", i)
        text = text[:j] + ",\n         " + ", ".join(str(x) for x in ids) + text[j:]

    # Sürmeli Bacı (Eşik): potions + the Eski accessories; the two homeland healers: + the Sağlam ones.
    stocks = [m.start() for m in re.finditer(r"^stock = \[16, 17, 18, 19, 28, 29\]", text, re.M)]
    assert len(stocks) == 3
    for idx, pos in reversed(list(enumerate(stocks))):
        acc = g["acc_Eski"] if idx == 0 else g["acc_Sağlam"]
        end = text.index("]", pos)
        text = (text[:end] + ", 292, 293,   # M18: large potions; " + ("Eski" if idx == 0 else "Sağlam") +
                " accessories\n         " + ", ".join(str(x) for x in acc) + text[end:])
    # Demirci Ilgaz: + the Yeminli new families; Usta Berkay and Örsbaşı Gürşad: + the Sağlam ones.
    extend("stock = [1, 2, 3, 4, 5, 6, 12, 14,", g["newfam_Yeminli"])
    extend("stock = [1, 2, 3, 4, 5, 7, 12,", g["newfam_Sağlam"])
    extend("stock = [1, 2, 3, 4, 5, 6, 8, 12,", g["newfam_Sağlam"])
    return text


def edit_quests(text, groups):
    g = groups
    sag = dict(zip(["greatsword", "hammer", "halberd", "spiked", "wavy", "crystal", "buckler"], g["newfam_Sağlam"]))
    acc = dict(zip([k["key"] for k in ACC_KINDS], g["acc_Sağlam"]))
    class_line = (f"class_items = {{ warrior = {sag['halberd']}, rogue = {sag['wavy']}, mage = {sag['crystal']}, "
                  f"priest = {sag['spiked']} }}   # M18: Sağlam Teber / Dalgalı Hançer / Billur Asa / Çivili Topuz")
    choice_line = (f"choice = [{acc['neck_hp']}, {acc['belt_def']}, {acc['ear_crit']}]   "
                   f"# M18: Sağlam Kurt Dişi Kolye / Örme Kemer / Kıvılcım Küpesi")

    def after_reward(qid, line):
        nonlocal text
        i = text.index(f"\n[[quest]]\nid = {qid}\n")
        j = text.index("[quest.reward]", i)
        k = text.index("coins = ", j)
        e = text.index("\n", k)
        text = text[:e + 1] + line + "\n" + text[e + 1:]

    for q in (21, 31):
        after_reward(q, class_line)
    for q in (23, 33):
        after_reward(q, choice_line)
    return text


def main():
    with open(os.path.join(GD, "items.toml"), "rb") as f:
        items = tomllib.load(f)["item"]
    new_items, groups = build_new_items()
    names = {it["id"]: it["name"] for it in items + new_items}
    existing_uniques = {it["id"] for it in items if it.get("rarity") == "unique"}

    def rw(name, fn):
        p = os.path.join(GD, name)
        with open(p, encoding="utf-8") as f:
            text = f.read()
        text = fn(text)
        with open(p, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)

    rw("items.toml", lambda t: edit_items(t, items, new_items))
    rw("loot.toml", lambda t: edit_loot(t, groups, names, existing_uniques))
    rw("npcs.toml", lambda t: edit_npcs(t, groups))
    rw("quests.toml", lambda t: edit_quests(t, groups))
    print(f"{len(items)} old + {len(new_items)} new = {len(items) + len(new_items)} templates")


if __name__ == "__main__":
    main()
