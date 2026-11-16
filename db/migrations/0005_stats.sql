-- 0005_stats: stat points spent on the five primary attributes (docs/PROTOCOL_STATS.md §6,
-- docs/DATABASE.md). Runs inside one transaction with search_path set to the target schema; never
-- schema-qualify. Immutable once applied anywhere: fix mistakes with a new migration.

-- Points the player allocated per attribute (not the totals: the class starting spread comes from
-- the server's code, shared/game/Combat.cpp). Existing characters start with 0 everywhere, so every
-- point of their level (3 per level above 1) is unspent and can be spent at their next entry.
-- Written by STAT_ALLOCATE_REQ (one UPDATE, before the answer), vo_admin reset-stats and vo_admin
-- set-level (which resets them when they no longer fit a lower level).
ALTER TABLE characters ADD COLUMN stat_strength smallint NOT NULL DEFAULT 0;
ALTER TABLE characters ADD COLUMN stat_dexterity smallint NOT NULL DEFAULT 0;
ALTER TABLE characters ADD COLUMN stat_vitality smallint NOT NULL DEFAULT 0;
ALTER TABLE characters ADD COLUMN stat_intelligence smallint NOT NULL DEFAULT 0;
ALTER TABLE characters ADD COLUMN stat_spirit smallint NOT NULL DEFAULT 0;
ALTER TABLE characters ADD CONSTRAINT characters_stat_range CHECK (
    stat_strength BETWEEN 0 AND 255 AND stat_dexterity BETWEEN 0 AND 255 AND stat_vitality BETWEEN 0 AND 255 AND
    stat_intelligence BETWEEN 0 AND 255 AND stat_spirit BETWEEN 0 AND 255);
-- Never more than the level grants: 3 x (level - 1).
ALTER TABLE characters ADD CONSTRAINT characters_stat_budget CHECK (
    stat_strength + stat_dexterity + stat_vitality + stat_intelligence + stat_spirit <= 3 * (level - 1));
