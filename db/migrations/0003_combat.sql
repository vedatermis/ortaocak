-- 0003_combat: experience, hit points and mana of characters (docs/PROTOCOL_COMBAT.md §3.5,
-- docs/DATABASE.md). Runs inside one transaction with search_path set to the target schema; never
-- schema-qualify. Immutable once applied anywhere: fix mistakes with a new migration.

-- Experience towards the next level (the level itself has been stored since 0001). Existing
-- characters start with 0.
ALTER TABLE characters ADD COLUMN xp bigint NOT NULL DEFAULT 0;
ALTER TABLE characters ADD CONSTRAINT characters_xp CHECK (xp BETWEEN 0 AND 4294967295);

-- Stored hit points and mana. NULL means "full": new characters and every character from before
-- this migration start at full hp/mp (max_hp/max_mp are derived from class, level and equipment by
-- the server, not stored). 0 hp = the character left the world dead; it re-enters at its race spawn
-- with 50 % hp/mp. Written by the world together with the position, and on level-up.
ALTER TABLE characters ADD COLUMN hp integer;
ALTER TABLE characters ADD COLUMN mp integer;
ALTER TABLE characters ADD CONSTRAINT characters_hp CHECK (hp IS NULL OR hp >= 0);
ALTER TABLE characters ADD CONSTRAINT characters_mp CHECK (mp IS NULL OR mp >= 0);
