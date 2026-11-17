# VedatOnline Database (PostgreSQL)

Status: M2. The server stores accounts and characters in PostgreSQL (tested against 16; the plan
targets 17). Without a configured database it falls back to the in-memory account service, which
loses everything on exit.

## 1. Configuration

`[db]` in `config/server.toml` (committed defaults) + `config/server.local.toml` (git-ignored, see
`config/server.local.toml.example`). Secrets only ever go in the local file or the environment.

| Key | Default | Meaning |
|---|---|---|
| `connection_string` | `""` | libpq key/value string or `postgresql://` URI of the game database. Empty = no database. Env `VEDAT_DB_URL` wins. |
| `admin_connection_string` | `""` | Maintenance DB (`.../postgres`). Used **only** to `CREATE DATABASE` the game database if it is missing. Env `VEDAT_DB_ADMIN_URL`. |
| `test_connection_string` | — | Read only by the tests (§6). Must name `vedatonline_test`. Env `VEDAT_TEST_DB_URL`. |
| `schema` | `"public"` | Schema holding all tables (`search_path` of every connection). |
| `pool_size` | `4` | Pooled connections. |
| `worker_threads` | `4` | DB worker threads; they also run the Argon2id hashing. |
| `statement_timeout_ms` | `5000` | Per statement (`0` = none). Migrations always run without a timeout. |
| `connect_timeout_s` | `5` | libpq `connect_timeout`. |
| `required` | `true` | Refuse to start when the database is unreachable or migrating fails (exit code 5). `false`: log an error and fall back to the in-memory service. |

Selection: `connection_string` non-empty → `PostgresAccountService`; empty → `MemoryAccountService`.
The startup log shows which one is used, the target as `user@host:port/db` (never the password),
the server version and whether the connection is encrypted.

**TLS.** libpq's default `sslmode=prefer` encrypts when the server offers TLS (the current dev
server does: TLSv1.3) but silently falls back to plaintext and does not verify the certificate.
For any server that is not on localhost use `sslmode=require` at least, ideally `verify-full` with
`sslrootcert=`. The server logs a warning when the connection is not encrypted.

## 2. Startup sequence

1. If `admin_connection_string` is set and the database named in `connection_string` does not
   exist, create it (`TEMPLATE template0 ENCODING 'UTF8' LC_COLLATE/LC_CTYPE 'C'`). Nothing else is
   ever done on the admin connection; only lower-case identifier names are accepted.
2. Connect, log server version and TLS state.
3. Apply pending migrations (§4), log each applied version.
4. `vo_server --migrate-only` exits here (0 = ok, 5 = failed).
5. Open the pool (one connection up front as a reachability check) and start serving.

## 3. Schema (0001_init, 0002_items, 0003_combat, 0004_loot, 0005_stats, 0006_skills)

All timestamps are `timestamptz`; every session runs with `TimeZone = 'UTC'`.

**accounts**

| Column | Type | Notes |
|---|---|---|
| `id` | `bigint` identity PK | CHECK 1..2³²−1 (u32 on the wire) |
| `username` | `text` | as entered; CHECK `^[A-Za-z0-9_]{4,20}$` |
| `username_key` | `text` | `vo::game::usernameKey()` (ASCII lower case); **unique index**; CHECK = `lower(username)` |
| `password_hash` | `text` | Argon2id encoded string (libsodium `crypto_pwhash_str`) |
| `created_at`, `last_login_at` | `timestamptz` | |
| `last_login_ip` | `inet` | NULL if unknown |
| `banned`, `ban_reason` | `boolean`, `text` | reason ≤ 200 characters |

**characters**

| Column | Type | Notes |
|---|---|---|
| `id` | `bigint` identity PK | CHECK u32 range |
| `account_id` | `bigint` FK → accounts | `ON DELETE RESTRICT` |
| `name` | `text` | as entered; CHECK 3–16 letters (ASCII + `çğıöşüÇĞİÖŞÜ`) / digits, starting with a letter |
| `name_key` | `text` | `vo::game::characterNameKey()` computed in C++ (all i-like letters fold to one key); **unique across all rows, soft-deleted included** |
| `race`, `class`, `face`, `hair` | `smallint` | CHECK race 1–2, class 1–4, face/hair 0–3 |
| `level` | `integer` | CHECK 1..65535 |
| `zone_id`, `x`, `y`, `z`, `yaw` | `integer`, `real` | CHECK finite (no NaN/∞). New characters get the race spawn of their **home zone** (M13, docs/PROTOCOL_ZONES.md §4: Aydor `spawn_aydor` of zone 2 Tanyeri, Kargan `spawn_kargan` of zone 3 Demirağız; `world.start_zone_override` for tests), taken from the zone terrains and passed to the account service at startup. The world also writes the zone on a zone change, before the player appears in the new zone (the arrival is stored first; docs/PROTOCOL_ZONES.md §3.5); written by the world on leave/logout/disconnect/shutdown and every 60 s while in the world if changed (docs/SERVER_WORLD.md §8) |
| `created_at`, `deleted_at`, `last_played_at` | `timestamptz` | `deleted_at` set = soft-deleted; the name stays reserved |

Indexes: `accounts_username_key_uq`, `characters_name_key_uq`, and the partial index
`characters_account_live_idx (account_id, id) WHERE deleted_at IS NULL` for listing/counting.

### 3.1 Items (0002_items, M8 "Kuşan", docs/PROTOCOL_ITEMS.md §5)

`characters.starter_kit_granted boolean NOT NULL DEFAULT false`: set in the same transaction that
inserts the kit's rows, the first time the character enters the world (characters created before
M8 get their kit on their next entry).

**character_items** (one row per item instance; templates live in `gamedata/items.toml`)

| Column | Type | Notes |
|---|---|---|
| `item_id` | `bigserial` PK | unique forever; u64 on the wire |
| `char_id` | `bigint` FK → characters | `ON DELETE CASCADE`: purging a character (or deleting its account) removes its items; a soft delete keeps them |
| `container`, `slot` | `smallint` | CHECK container 0 (Equipment, slot 0–7) or 1 (Bag, slot 0–27) |
| `template_id` | `integer` | CHECK 1..65535. Not a foreign key: a row whose template the server does not know is kept, logged once and hidden from the client |
| `quantity` | `integer` | CHECK 1..65535 (1 for weapons and shields; since M10 the stack size of materials, ≤ the template's `max_stack`) |
| `created_at` | `timestamptz` | |

`character_items_place_uq UNIQUE (char_id, container, slot) DEFERRABLE INITIALLY IMMEDIATE` (also
the index for loading a character's items). A move runs `SET CONSTRAINTS character_items_place_uq
DEFERRED`, so the rows of a swap may pass each other and the check happens at commit.

- *Kit exactly once*: the load locks the character row (`SELECT starter_kit_granted ... FOR
  UPDATE`), inserts the kit (around any rows already there, e.g. from `vo_admin give-item`) and
  sets the flag in one transaction; a concurrent load waits and then sees the flag.
- *Moves*: one transaction per move. The character row is locked; every relocated row is updated
  only if it is still at its expected old place (or already at the new one, so a job re-run after a
  lost connection whose commit went through still succeeds); any mismatch or a place collision at
  commit rolls the whole move back. The server answers ITEM_MOVE_RES only after the commit.

### 3.2 Combat (0003_combat, M9 "İlk Kan", docs/PROTOCOL_COMBAT.md §3.5)

Three columns on **characters** (the level has been stored since 0001):

| Column | Type | Notes |
|---|---|---|
| `xp` | `bigint NOT NULL DEFAULT 0` | experience towards the next level; CHECK 0..2³²−1 (u32 on the wire) |
| `hp` | `integer` | CHECK NULL or ≥ 0. **NULL = full** (new characters and every character from before 0003: max_hp is derived from class, level and equipment by the server, not stored); **0 = left the world dead** (the next entry is at the race spawn with 50 % hp/mp) |
| `mp` | `integer` | CHECK NULL or ≥ 0; NULL = full |

Written by the world through `IAccountService::saveCharacterState` (position, level, xp, hp, mp in
one UPDATE) at the position cadence (docs/SERVER_WORLD.md §8: leave, logout, disconnect, shutdown,
and every 60 s if anything changed) and at once on a level-up. Values above the current maxima
(e.g. after unequipping) are clamped on entry. `vo_admin set-level` sets the level (clamped to the
cap, 10), `xp = 0` and `hp = mp = NULL`.

### 3.3 Loot (0004_loot, M10 "Ganimet", docs/PROTOCOL_LOOT.md §6)

One column on **characters**:

| Column | Type | Notes |
|---|---|---|
| `coins` | `bigint NOT NULL DEFAULT 0` | the Akçe balance; CHECK 0..1 000 000 000 (u32 on the wire). Existing characters start with 0 |

Stacks need no schema change (`character_items.quantity`). Loot containers are world state only:
they are not stored, a server restart loses the unlooted ones.

- *Loot takes* and *merge moves* use `IAccountService::applyItemWrite`: one transaction that first
  runs `UPDATE characters SET coins = coins + $n ... RETURNING coins` (locks the character row,
  checks it is a live character of the account; the CHECK refuses a balance above the cap), then
  updates each grown stack only `WHERE quantity = <expected>` (or deletes an emptied one with the
  same condition), then inserts the new stacks (the place uniqueness refuses an occupied slot). Any
  mismatch rolls the whole write back: the units return to the container and the take is answered
  `ServerError`. The returned balance is what `COINS` reports, so a `vo_admin set-coins` made while
  the character is online is never overwritten by a stale value.
- Known limit: a commit "in doubt" (the connection dropped during `COMMIT`, never retried) is
  reported as a failure, and its units return to the container although the commit may have
  happened; the next entry loads the stored truth.

### 3.4 Stats (0005_stats, M11 "Künye", docs/PROTOCOL_STATS.md §6)

Five columns on **characters**, the stat points the player **spent** per attribute (the class's
starting spread comes from `shared/game/Combat.cpp`, not from the database):

| Column | Type | Notes |
|---|---|---|
| `stat_strength`, `stat_dexterity`, `stat_vitality`, `stat_intelligence`, `stat_spirit` | `smallint NOT NULL DEFAULT 0` | CHECK `characters_stat_range`: each 0..255 |

CHECK `characters_stat_budget`: the sum is at most `3 × (level − 1)` (the points the stored level
grants). Existing characters get 0 everywhere: after 0005 every character from before M11 has all
the points of its level unspent (a level-7 character: 18), with nothing else to do.

- *Allocations* (`STAT_ALLOCATE_REQ`) use `IAccountService::allocateStats`: one `UPDATE characters
  SET stat_x = stat_x + n, ... WHERE id = .. AND account_id = .. AND deleted_at IS NULL AND <sum
  after> <= <budget of the live level> AND <class start + each value after> <= 255 RETURNING
  stat_*`. No row = refused (nothing changed); a CHECK violation (the stored level is lower than the
  world's, e.g. a lost level-up save) is refused the same way. The returned values are what the
  session and the world adopt, so a `vo_admin reset-stats` made while the character is online is
  picked up by its next allocation. Requests of one account run in submission order, so a level-up
  save of the world commits before an allocation that uses the new level. The answer
  (`STAT_ALLOCATE_RES`) is sent only after the commit. Known limit as for loot: a commit "in doubt" is
  answered `ServerError`.
- The world never writes these columns (its saves carry position and progress only).

### 3.5 Skills (0006_skills, M12 "Yetenek Yolları", docs/PROTOCOL_SKILLS.md §6)

**character_skills** — one row per skill rank the player paid for with skill points:

| Column | Type | Notes |
|---|---|---|
| `char_id` | `bigint NOT NULL` | `REFERENCES characters (id) ON DELETE CASCADE` |
| `skill_id` | `smallint NOT NULL` | CHECK `character_skills_skill`: 1..63 (ids of `gamedata/skills.toml`) |
| `rank` | `smallint NOT NULL` | CHECK `character_skills_rank`: 1..3 |

Primary key `(char_id, skill_id)`. A class's free skill at rank 1 has no row (rank 2 or 3 is stored
as that rank). Skills, classes, branches, tiers and the point budget (1 point per level above 1)
live in the data file and the shared code (`shared/game/Skills.h`), not in the database: no CHECK can
see them. Instead the learn job locks the `characters` row (`SELECT class, level ... FOR UPDATE`),
reads the rows, runs the shared `game::checkSkillLearn` on the **stored** ranks with the lower of the
live and the stored level, and upserts one row (`INSERT ... ON CONFLICT (char_id, skill_id) DO UPDATE
SET rank = EXCLUDED.rank`) in the same transaction. The answer carries the stored ranks, so a
`vo_admin reset-skills` made while the character is online is picked up by its next learn. Rows of
unknown or foreign skills (data changed) are ignored on load and logged nowhere (they cost nothing).

**characters.skill_bar** — `smallint[]`, NULL = the default bar (the class's free skill on key 1).
CHECK `characters_skill_bar`: NULL, or exactly 8 values, one dimension, no NULL element, each 0..63.
`SKILL_BAR_SET_REQ` is one `UPDATE characters SET skill_bar = $slots::smallint[]` after checking every
slot against the stored ranks under the same row lock. Loads read it with `array_to_string(skill_bar,
',')`; slots holding a skill that is no longer learned are shown empty.

Existing characters: no rows and a NULL bar, i.e. every skill point of their level unspent and the
default bar — nothing to backfill. The world never writes these (skill cooldowns are kept in memory
only, docs/SERVER_WORLD.md §8e).

**Race-freedom.**
- *Auto-registration*: `INSERT ... ON CONFLICT (username_key) DO NOTHING RETURNING id`. The loser of
  a race verifies its password against the winner's row like a normal login.
- *Name uniqueness*: only the unique index decides; its violation maps to `NameTaken`.
- *3 live characters*: character creation locks the account row (`SELECT ... FOR UPDATE`) before
  counting, so creates for one account are serialised. The trigger `characters_slot_limit`
  enforces the same invariant for every writer (inserts and un-deletes, `vo_admin` included).
  Both rely on READ COMMITTED (the default).

## 4. Migrations

Files: `db/migrations/NNNN_name.sql`, applied in version order. Rules:

- **Forward-only and immutable.** `schema_migrations(version, name, checksum, applied_at)` stores
  the SHA-256 of each applied file (CRLF→LF normalised, BOM stripped). A changed checksum, an
  applied version without a file, or a new file numbered below the newest applied version is a
  hard error: the server does not start (exit 5) and nothing is applied.
- Each file runs in **one transaction** together with its `schema_migrations` row; a failing file
  leaves no trace. Do not write `BEGIN`/`COMMIT` or schema-qualified names.
- Concurrent runners (two servers, server + `vo_admin`) are serialised by
  `pg_advisory_lock(0x564F4D47, hashtext(<schema>))`; the second one waits, then finds nothing to do.
- The database must be UTF8.

Adding a change: create `db/migrations/0002_what_it_does.sql`, run `vo_admin migrate` (or just
start the server) against your local database, run `build.ps1 -Test` (the tests migrate fresh
schemas from scratch), commit.

## 5. vo_admin

Same config loading as the server (`--config <path>`, `server.local.toml`, `VEDAT_DB_URL`).

```
vo_admin migrate                              # create DB if missing + apply migrations
vo_admin status                               # connection, TLS, schema version, counts
vo_admin account create <user>                # prompts for the password twice (no echo)
vo_admin account passwd <user>
vo_admin account ban <user> [reason...]
vo_admin account unban <user>
vo_admin account delete <user> --yes          # remove the account and all its characters for good
vo_admin character list <user>                # all characters incl. soft-deleted
vo_admin character rename <user> <id> <new_name>
vo_admin character restore <user> <id>        # undo a soft delete (needs a free slot)
vo_admin character purge <user> <id> --yes    # delete a soft-deleted character for good (frees the name)
vo_admin give-item <character> <template_id> [count]  # new item(s) in the bag (template from gamedata/items.toml);
                                              # stackables fill existing stacks first, all or nothing
vo_admin list-items <character>               # every item of a live character, with the kit flag and its Akçe
vo_admin set-coins <character> <amount>       # the Akçe balance, 0..1000000000
vo_admin set-level <character> <level>        # testing: level clamped to 1..10, xp 0, hp/mp full on the next entry;
                                              # spent stat / skill points that no longer fit a lower level are reset
vo_admin reset-stats <character>              # every spent stat point back to unspent (M11)
vo_admin reset-skills <character>             # every skill back to rank 0 (the free skill to 1), default bar (M12)
vo_admin show-character <character>           # level, xp, hp/mp, Akçe, attributes (start + spent), unspent points,
                                              # skill points, every learned rank and the skill bar, the zone with its name
vo_admin move-character <character> <zone> [marker]  # M13: an offline character to a marker (default spawn_<race>)
vo_admin testdb drop-schema <vo_test_*|vo_it_*>   # only in vedatonline_test
vo_admin testdb sweep                         # drop test schemas older than 2 h
```

Passwords are never command-line arguments (they would end up in shell history and process
lists). For scripts, pipe the password from a secret store into `--password-stdin`, which reads
the first line of stdin:

```
Get-Secret vo-admin-pw -AsPlainText | vo_admin account create Vedat --password-stdin
```

`give-item` / `list-items` / `set-coins` find live characters by name (Turkish-aware, like the
uniqueness key; arguments are read as UTF-8). `give-item` refuses unknown templates, a count above
`max_stack × 28` and a bag the units do not all fit into (exit 3); the count defaults to 1 and
stackable units go first into the bag's stacks of that template that are not full, then into the
lowest free slots (the same rule as a loot pickup). `set-coins` refuses amounts above the cap (exit
3); a character in the world sees the new balance on its next entry, and a loot gain before that
reports and keeps the admin value plus the gain. The
server keeps a character's inventory in memory while it is in the world: an item given to a
character that is online shows up after it leaves and enters again (until then a move onto that bag
slot is refused with ServerError and nothing changes), so give items to offline characters.
`set-level` is for testing; like `give-item` it is meant for offline characters: a character that
is in the world keeps its live level and overwrites the stored one with its next save. Raising the
level of a character that left the world a moment ago works (a stored level above the world's
recently-left cache wins); lowering it within a minute of a logout does not. Lowering the level
below what the spent stat points need resets them to 0 in the same statement (the
`characters_stat_budget` CHECK) and says so; `set-level` prints the unspent points afterwards.
`reset-stats` (M11) sets the five `stat_*` columns to 0, so every point of the character's level is
unspent again (the only way back from a spent point); a character in the world keeps its live
allocation until its next entry or its next allocation (which returns the stored values).
`show-character` prints id, race/class, level, xp, hp/mp ("full" for NULL), Akçe, zone and
position, a table of the five attributes (class start, spent, base) and "stat points: S of P
spent, U unspent"; since M12 also "skill points: S of P spent, U unspent", one line per learned skill
(id, name, branch, tier, rank, "(free)") and "skill bar: [1] ... [8] ..." ("(default)" for NULL).
`reset-skills` (M12) deletes the character's `character_skills` rows and sets `skill_bar` to NULL
in one transaction; a character in the world keeps its live ranks until its next entry or learn.
`set-level` to a level whose points no longer cover the spent skill points does the same and prints
"skills were reset". Both read `gamedata/skills.toml` like the server (searched upwards from the
working directory and the executable's directory) for names and the point budget.
`move-character` (M13, docs/PROTOCOL_ZONES.md §6.1) reads `gamedata/zones.toml` (zone id or key) and
the zone's terrain like the server (`world.assets_dir`, else `assets/<terrain>` upwards), refuses a
zone the character's race may not enter, an unknown zone and a missing or unwalkable marker (exit
3), and writes `zone_id`, `x`, `y` (on the terrain), `z`, `yaw` in one `UPDATE`. Like `set-level` it
is for offline characters (the database does not know who is online; a character in the world
overwrites it with its next save). `show-character` prints the zone as `zone 2 (Tanyeri)`.

**Zones (M13).** No migration: `characters.zone_id` (0001, CHECK 0..65535) already holds the zone.
A zone change writes the arrival with the normal state save (`UPDATE characters SET zone_id, x, y, z,
yaw, level, xp, hp, mp`) *before* the player appears in the new zone; one row per character means a
character is always in exactly one zone, whatever crashes when. Existing characters (all in zone 1)
keep their place; a stored zone that is unknown or forbids the race puts the character at the race
spawn of its home zone on its next entry (nothing is rewritten in the database until then).

Exit codes: 0 ok, 1 usage, 2 config, 3 not found / invalid / refused, 4 database error.

## 6. Test database

`vedatonline_test` is **disposable**: tests may create and drop anything inside it, never touch it
for real data. It is created on demand (through `admin_connection_string`) if missing.

- URL: `VEDAT_TEST_DB_URL`, else `db.test_connection_string` in `config/server.local.toml`. The
  tests refuse any URL whose database is not `vedatonline_test` (checked on the URL and with
  `current_database()`).
- Without a URL, or with `VEDAT_TEST_DB_URL=skip`, every PostgreSQL test is **skipped** (ctest
  shows "Skipped"), so `build.ps1 -Test` passes on machines without PostgreSQL.
- Every unit test runs in its own schema `vo_test_<unix time>_<random>`, every DB integration
  scenario (`integration.db.*`) in `vo_it_<unix time>_<random>`; they are dropped afterwards, and
  leftovers of crashed runs older than two hours are swept automatically.
- The pool-recovery test terminates (`pg_terminate_backend`) only its own pooled connections,
  identified by backend PID, database `vedatonline_test` and `application_name = vo_tests`.

## 7. Operations

- **Backups.** `pg_dump -Fc -d vedatonline -f vedatonline_YYYYMMDD.dump` (restore with
  `pg_restore -d <empty db>`). Take one before every deployment that adds a migration; there are
  no down-migrations.
- **What is logged.** Connection targets as `user@host:port/db`, server version, TLS state,
  applied migrations, and DB errors as their first message line + SQLSTATE. Never passwords,
  password hashes (DETAIL lines that could quote row contents are dropped) or full connection strings.
- **Failure behaviour.** A failed request is logged and answered with the protocol's
  `ServerError` result (`LOGIN_RES 5`, `CHAR_CREATE_RES 6`, ...); the session stays open, except
  for `CHAR_LIST_REQ`, whose response has no result field (`ERROR(InternalError)` + close).
  Lost connections are re-opened transparently (a job interrupted by a dead connection is retried
  once); while the database is down, reconnects back off from 100 ms to 5 s and requests fail fast.
- **Ordering.** Requests of one account run in submission order; different accounts run in parallel
  on the worker threads. Position saves are submitted by the world thread; a leave's save is always
  submitted before the session can send the next ENTER_WORLD_REQ, so a re-enter loads the saved
  position (and the world bridges the remaining gap for duplicate logins, docs/SERVER_WORLD.md §8).
- **Load.** Periodic saves are spread over the save interval (one slot per character), i.e. about
  `players / 60` UPDATEs per second, and skipped for players whose position and progress did not
  change; level-ups add one UPDATE each.
