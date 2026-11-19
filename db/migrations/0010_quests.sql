-- 0010_quests: the quest state of each character (M17, docs/PROTOCOL_QUESTS.md §6, docs/DATABASE.md).
-- Runs inside one transaction with search_path set to the target schema; never schema-qualify.
-- Immutable once applied anywhere: fix mistakes with a new migration.

-- One row per quest a character has accepted at least once. The quests themselves (givers, objectives,
-- rewards, prerequisites, cooldowns) live in gamedata/quests.toml; the server's shared code decides under
-- a lock on the characters row (the database cannot know the data file). "Available" and "ready to hand
-- in" are not stored: they follow from the data file, this row and the bag.
--   state 1 = active (progress counters running), 2 = done (handed in at least once).
--   progress1..3: the kill / boss counters of the quest's objectives in file order (collect objectives
--   count the bag at the hand-in, talk objectives nothing), 0..1000.
--   completions / completed_at: hand-ins so far and the last one (the cooldown of a repeatable quest).
-- Written by QUEST_ACCEPT / ABANDON / HANDIN_REQ before the answer (a hand-in in the same transaction as
-- its items, Akçe and experience), the counters with the world's save cadence and on leave; vo_admin
-- set-quest for tests. Existing characters have no rows: every quest of their level is available.
CREATE TABLE character_quests (
    char_id      bigint      NOT NULL REFERENCES characters (id) ON DELETE CASCADE,
    quest_id     smallint    NOT NULL,
    state        smallint    NOT NULL,
    progress1    smallint    NOT NULL DEFAULT 0,
    progress2    smallint    NOT NULL DEFAULT 0,
    progress3    smallint    NOT NULL DEFAULT 0,
    completions  integer     NOT NULL DEFAULT 0,
    accepted_at  timestamptz NOT NULL DEFAULT now(),
    completed_at timestamptz,
    CONSTRAINT character_quests_pk PRIMARY KEY (char_id, quest_id),
    CONSTRAINT character_quests_quest CHECK (quest_id BETWEEN 1 AND 999),
    CONSTRAINT character_quests_state CHECK (state IN (1, 2)),
    CONSTRAINT character_quests_progress CHECK (progress1 BETWEEN 0 AND 1000 AND progress2 BETWEEN 0 AND 1000 AND
                                                progress3 BETWEEN 0 AND 1000),
    CONSTRAINT character_quests_completions CHECK (completions >= 0),
    -- A done row was handed in at least once, and then has a time.
    CONSTRAINT character_quests_done CHECK (state = 1 OR (completions >= 1 AND completed_at IS NOT NULL))
);
