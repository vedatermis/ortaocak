# Protocol v11: quests (M17 "Görevler")

Extends docs/PROTOCOL.md (framing, encryption, sessions) and the item / loot / shop / upgrade contracts.
Protocol version **11** (HELLO.protocol_version; v10 clients get `VersionMismatch`). Mechanics follow the
classic two-nation MMORPG (quest givers in a shared town, kill / collect / talk / boss objectives, chains,
repeatables with a cooldown); every name, text, NPC and story is original (docs/LORE.md).

Data: `gamedata/quests.toml` (format in its header and §1), loaded and strictly validated by
`shared/game/Quests.h` on the server (refuses to start, exit 6), the client and the tools. Rules: §2.
Wire: §3-§5. Storage: §6. Client: §7.

## 1. Data

| Thing | Rule |
|---|---|
| Quest | `id` 1..999 (stable: stored and sent), `name`, `giver` / `turn_in` (NPC ids of npcs.toml; turn-in defaults to the giver), `level` (lowest level that may accept), `race` (optional: `aydor` / `kargan`), `after` (a quest earlier in the file that must be handed in first), `repeatable` + `cooldown_s` (60 s .. 7 days, from the hand-in), `story` (the offer), `progress_text` (optional), `done_text` |
| Objectives | 1..3: `kill` (`monsters` = template ids, `count` 1..50), `boss` (elite templates, 1..5), `collect` (`item`, a stackable item, 1..50), `talk` (`npc` = the turn-in NPC, which must differ from the giver). A quest handed in to another NPC needs a talk objective for it |
| Rewards | `xp` (0..100 000), `coins` (Akçe, 0..100 000), `items` (1..4 `[id, count]`, a count no larger than one stack), `class_items` (one equippable item per class, each wearable by its class: "the piece that fits my class"), `choice` (2..4 items, the player picks one at the hand-in). Something must be rewarded |
| Rules | `[rules] max_active` (1..25, shipped 10): quests a character may carry at once |
| NPCs | npcs.toml `kind = "quest"` (no stock; a quest giver only). Any NPC (merchants too) may give / take back quests. `tint` / `scale` let an NPC borrow another's model (two NPCs may share a model only if these differ) |

Collect items come from the **normal loot** of the monsters (Kül Tozu from Kül Çömezi, Cüruf Parçası from Cüruf
Yürüyen, Kavruk Başak, Körük Derisi); there are no quest-only drops. They are counted in the bag (equipment does
not count) and taken at the hand-in.

## 2. Rules (server-authoritative; `shared/game/Quests.h`)

### 2.1 Accept (QUEST_ACCEPT_REQ)

In this order; the first that fails answers QUEST_RES with that result, nothing changes:

1. `UnknownQuest` - not in quests.toml (the session answers without asking the world).
2. World (§2.2): `WrongNpc` (no NPC with that entity id in the player's view), `YouAreDead`, `OutOfRange` (farther
   than the talk range 4 m + 0.5 m).
3. `WrongNpc` - the NPC is not the quest's giver.
4. `game::checkQuestAccept`: `AlreadyActive`, `AlreadyDone` (a one-time quest handed in), `OnCooldown` (a repeatable
   one within `cooldown_s` of its last hand-in; the clock is Unix time, the stored `completed_at`), `WrongRace`,
   `LevelTooLow` (the live level), `NeedsQuest` (`after` not handed in), `TooManyActive` (the limit).
5. The write (§6): the row inserted (or a Done row of a repeatable set back to Active, counters 0). ServerError if
   it fails. Then QUEST_UPDATE (the new row), then QUEST_RES Ok.

### 2.2 The world's part

QUEST_ACCEPT_REQ and QUEST_HANDIN_REQ go to the world first (a `QuestCommand`): the NPC must be an NPC entity the
player has in view, the player alive, within the talk range. The world answers a refusal itself or hands a
`QuestGrant` (with the NPC's npcs.toml id) to the session, which decides the rest against the **committed** state
(quest rows, bag, balance) in its item queue - in order with item moves, loot takes, purchases and upgrades. A
request sent while the player changes zone is dropped (like shop requests). QUEST_ABANDON_REQ needs no NPC.

### 2.3 Hand-in (QUEST_HANDIN_REQ)

1. `UnknownQuest`; the world (§2.2); `WrongNpc` (not the turn-in NPC).
2. `game::planQuestHandIn`: `NotActive`, `NotReady` (an objective not met: kill / boss counters, the units in the
   bag), `BadChoice` (a choice is offered and `choice` is not a valid index, or a choice is named for a quest
   without one), `BagFull` (after the collected units are taken, a reward unit does not fit - the client says
   "Çantan dolu: ödül sığmıyor"), `PurseFull` (the Akçe reward would pass 1 000 000 000).
3. **One transaction** (§6): the quest row Active -> Done (`completions + 1`, `completed_at = now()`, counters 0),
   the collected units removed (lowest bag slots first), the reward items placed (stacks first, then free slots -
   a slot the removal freed is used), the Akçe added, the experience added to the stored level / xp
   (`game::addExperience`). Nothing changes if any part does not hold.
4. After the commit: ITEM_UPDATE(s), COINS (if Akçe), QUEST_UPDATE (Done, with the cooldown), QUEST_RES Ok (with
   xp and coins); then the world adds the same experience to the live character (XP_GAIN, LEVEL_UP) and saves it
   at once. (A hand-in just before a zone change: the arrival carries it.)

### 2.4 Kill credit and counters

A monster's death counts for the player who gets its loot: the experience winner (most damage), else the
killing blow's player (`Zone::killMonster`, `WorldBatch::kills`). Every active quest of that player with an
unfinished kill / boss objective listing the template counts one; a QUEST_UPDATE with the new counters follows.
Counters are live in the session and stored with the world's save cadence (`world.save_interval_s`, 60 s) and when
the character leaves the world (logout, character select, disconnect, server shutdown): a crash loses at most the
last minute of counters, never an accept / abandon / hand-in.

### 2.5 Abandon (QUEST_ABANDON_REQ)

`UnknownQuest`, `NotActive`; else the row is deleted (a quest never handed in) or set back to Done keeping its
completions and `completed_at` (a repeat of a repeatable quest: its cooldown is not restarted). QUEST_UPDATE with
state None (or Done), QUEST_RES Ok.

### 2.6 Testing hooks (server config `[world]`, default off, a warning in the log when on)

| Key | Effect | Used by |
|---|---|---|
| `testing_quest_max_active` | the active limit instead of quests.toml's | `integration.quests` (TooManyActive) |
| `testing_quest_kill_credit` | every kill counts N times (0..50) for kill / boss objectives | `integration.quests`, `m17_*` flows |

## 3. Codes

| Opcode | Dir | Name |
|---|---|---|
| 0x0B00 | S->C | QUEST_LIST |
| 0x0B01 | C->S | QUEST_ACCEPT_REQ |
| 0x0B02 | C->S | QUEST_ABANDON_REQ |
| 0x0B03 | C->S | QUEST_HANDIN_REQ |
| 0x0B04 | S->C | QUEST_RES |
| 0x0B05 | S->C | QUEST_UPDATE |

QUEST_RES.action: 1 Accept, 2 Abandon, 3 HandIn. QuestEntry.state: 0 None (QUEST_UPDATE only: no row any more),
1 Active, 2 Done.

| result | Name | Meaning |
|---|---|---|
| 0 | Ok | |
| 1 | UnknownQuest | not in quests.toml |
| 2 | WrongNpc | not the giver / turn-in NPC, or no such NPC in view |
| 3 | OutOfRange | farther than 4 m + 0.5 m |
| 4 | YouAreDead | |
| 5 | LevelTooLow | |
| 6 | WrongRace | |
| 7 | NeedsQuest | the quest it follows is not handed in |
| 8 | AlreadyActive | |
| 9 | AlreadyDone | a one-time quest handed in |
| 10 | OnCooldown | a repeatable quest's cooldown runs |
| 11 | TooManyActive | the active limit |
| 12 | NotActive | abandon / hand-in of a quest that is not active |
| 13 | NotReady | an objective is not met |
| 14 | BagFull | the reward items do not fit |
| 15 | BadChoice | no / an invalid choice |
| 16 | PurseFull | the Akçe reward would pass the cap |
| 17 | ServerError | the write failed, or too many requests waiting |

## 4. Messages (little-endian; strings none)

    QuestEntry      u16 quest_id (1..999), u8 state, u16 progress[3], u32 cooldown_s            (13 bytes)
    QUEST_LIST      u8 count (0..250), QuestEntry[count]   (ascending id; state Active / Done only)
    QUEST_UPDATE    QuestEntry                               (state None = abandoned)
    QUEST_ACCEPT_REQ u32 seq, u32 npc_id (entity), u16 quest_id                                 (10 bytes)
    QUEST_ABANDON_REQ u32 seq, u16 quest_id                                                     (6 bytes)
    QUEST_HANDIN_REQ u32 seq, u32 npc_id (entity), u16 quest_id, u8 choice (0xFF = none)        (11 bytes)
    QUEST_RES       u32 seq, u8 action, u16 quest_id, u8 result, u32 xp, u32 coins               (16 bytes)

`progress` holds the kill / boss counters in the order of the quest's objectives (collect and talk objectives: 0 -
the client counts the bag itself, a talk objective is met while the quest is active). `cooldown_s`: a Done
repeatable quest's seconds until it may be taken again (0 otherwise). `xp` / `coins` of QUEST_RES: the hand-in's
rewards (0 otherwise). read() rejects an unknown state / action / result, a quest id 0, QUEST_LIST with state None
or more than 250 entries; any malformed request closes the connection (ProtocolViolation).

### 4.1 Order on entry

ENTER_WORLD_RES, INVENTORY, COINS, SKILL_STATE, SKILL_BAR, **QUEST_LIST**, then the world (CHAR_STATS,
ENTITY_SPAWN...). The same after ZONE_CHANGE.

## 5. Limits

| Limit | Value |
|---|---|
| QUEST_ACCEPT / ABANDON / HANDIN_REQ | 5 a second combined (more: ERROR(Flood), the connection closes); the client sends at most 4 and one at a time |
| Active quests | 10 (`[rules] max_active`) |
| Requests waiting behind a write | 16 (shared with the item queue; more: ServerError) |
| Quests in quests.toml | 200; QUEST_LIST 250 entries |

## 6. Storage (PostgreSQL, `db/migrations/0010_quests.sql`)

`character_quests(char_id, quest_id, state 1|2, progress1..3 0..1000, completions >= 0, accepted_at,
completed_at)`, primary key (char_id, quest_id), `ON DELETE CASCADE` with the character; a Done row has
completions >= 1 and a time. Written by `IAccountService::applyQuestWrite` (one transaction; the character row is
locked first and its coins changed as `applyItemWrite` does, then the quest rows - strict changes must find their
row in the expected state, progress saves skip rows that are no longer active - then the items, then the level /
xp). Loaded by `loadQuests` on world entry (after the items, before the world). The in-memory service mirrors it
(`tests/server/AccountServiceTests.cpp` style; `MemoryAccountService::questsOf / setQuest`).

### 6.1 vo_admin

- `show-character <character>`: a "quests" section - every row with its state, kill counters ("Kül Çömezi 3/6"),
  collect objectives ("(in the bag)"), `done xN` and a repeatable's cooldown; "quests: N row(s), M active (limit
  10)".
- `set-quest <character> <quest> none|active|done [p1 p2 p3 | <seconds ago>]` (testing): deletes / sets one row
  (`done` with completions >= 1 and `completed_at = now - seconds`). A character in the world sees it on its
  next entry.

## 7. Client

### 7.1 Markers (procedural, `client/scenes/QuestWindows.cpp drawQuestMarker`)

A faceted köz gem on a short ember stem over the NPC's head (over a merchant's shop marker when it has one), at
least 26 logical pixels high at any distance up to 60 m:

| State | Look | When |
|---|---|---|
| available | solid amber gem, bright core, faint glow | the NPC gives a quest the character may accept now |
| progress | hollow ash-grey gem with a dim heart | a quest of the NPC (given or taken back) is active and not ready |
| ready | green-gold gem with a tick and a pulsing halo | a quest is ready to hand in to this NPC |

Strongest wins (ready > available > progress). Automation states `marker<npcId>_<state>`.

### 7.2 Dialog, log, tracker, notices

- A click / E on a quest NPC walks into range and opens its **dialog** (left side, placed by the window arranger);
  a merchant opens its dialog first only when it has a quest ready to hand in there or one to give now (the dialog
  then has "Alışveriş"); otherwise the shop opens as before and shows a "Görevler" button in its footer while the
  merchant has a quest of ours in any state. One quest and no shop: the quest at once; else a list (status gem,
  name, "Yeni görev" / "Teslim edilebilir" / "Devam ediyor" / "3. seviyede" greyed for quests up to two levels
  above).
- A quest: name, status, the NPC's words (offer / progress / hand-in text), "Hedefler" (with progress while
  active), "Ödüller" (experience, Akçe, item icons with the item tooltip on hover, the class item marked "(sınıfına
  göre)", a choice row picked by a click), buttons Kabul et / Teslim et and Vazgeç / Kapat, "Geri" to the list.
  The body scrolls (wheel) when the screen is small. After an accept / hand-in the dialog returns to the list, or
  closes when the NPC has nothing more to give or take back. It closes beyond 9.5 m, on death and on a zone
  change.
- **Görevler** window (L or the HUD button "Görevler (L)" between Yetenekler and Tuşlar; right side): the active
  quests ("Etkin görevler 3 / 10"), the selected one's objectives, text and "Teslim: <NPC> (<bölge>)", "Bırak"
  (asks "<Görev> bırakılsın mı? ..."; Vazgeç is the default button).
- **Tracker** (HUD, under the zone name): up to five active quests, their objectives ("Kül Çömezi 3/6"; met ones
  green), a ready quest as "<NPC> ile konuş"; hidden while a window covers it, in the menu and the gate screen.
- **Notices** (centre, a quarter down the screen, 4 s): "Yeni görev: ...", "Tamam: <objective>" (one objective of
  several), "Görev tamamlandı: <Görev> — <NPC> ile konuş", "Görev teslim edildi: <Görev> (+70 deneyim, +20 Akçe)".
- Refusals (one line, also above the bar): "Bu görev için 3. seviyeye ulaşmalısın.", "Önce \"Külden Kalkanlar\"
  görevini bitirmelisin.", "Çantan dolu: ödül sığmıyor. Yer açıp yeniden dene.", "En çok 10 görev
  taşıyabilirsin: ...", "Çok uzaktasın: biraz yaklaş." ... (`client/world/QuestView.cpp questResultText`).

### 7.3 Automation (WorldScene `scene` commands)

`questselect <id>`, `questaccept`, `questhandin [choice]`, `questdecline`, `questacceptforce <npc> <id>`,
`questhandinforce <npc> <id> [choice]`, `questabandon <id>` (asks), `questabandonforce <id>`, `questlog open|close`,
`questlogselect <id>`, `expectquest <id> none|active|ready|done|cooldown [p1 p2 p3]`, `expectmarker <npc>
none|available|progress|ready`, `expecttracker <text>|none`, `expectquestmessage <text>`, `expectquestnotice
<text>`, `expectquestdialog <npc>|none`, `questdump` (NPC names with `_` for spaces). States: `questsknown`,
`questdialog` / `noquestdialog`, `questdetail` / `questlist`, `questlog` / `noquestlog`, `questbusy` / `questidle`,
`questtracker` / `noquesttracker`, `questnotice`, `questabandonask`, counters `questactive<n>`, `questaccepted<n>`,
`questhandedin<n>`, `questabandoned<n>`, `questrefused<n>`, `questupdates<n>`. Widgets: `quest.dialog`,
`quest.accept`, `quest.handin`, `quest.decline`, `quest.back`, `quest.shop`, `quest.close`, `quest.text`,
`quest.log`, `quest.log.abandon`, `quest.log.text`, `quest.tracker`, `shop.quests`, `shop.note`, `world.quests`.

## 8. The upgrade hub (M17)

Only the Eşik smith (Demirci Ilgaz, `upgrades = true`) upgrades: UPGRADE_REQ at a homeland smith is
`NotSmith`. The homeland smiths still sell the three stones; their shop window has no "Yükselt" tab and instead
the line "Tav işi Eşik'te yapılır: Demirci Ilgaz'a git." (`NpcDef::sendsToSmith`: a merchant that sells stones
and does not upgrade).
