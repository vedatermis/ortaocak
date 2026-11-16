-- 0006_skills: skill ranks and the skill bar (docs/PROTOCOL_SKILLS.md §6, docs/DATABASE.md).
-- Runs inside one transaction with search_path set to the target schema; never schema-qualify.
-- Immutable once applied anywhere: fix mistakes with a new migration.

-- One row per skill rank the player paid for with skill points. Skills (ids, classes, branches,
-- tiers) live in gamedata/skills.toml, not here; a class's free skill at rank 1 has no row (at rank
-- 2 or 3 its row holds that rank). Class membership, tier gates and the point budget
-- (1 point per level above 1) are checked by the server's shared code under a lock on the
-- characters row: the database cannot know the data file. Existing characters have no rows, so
-- every point of their level is unspent. Written by SKILL_LEARN_REQ (before the answer), deleted by
-- vo_admin reset-skills and by vo_admin set-level when the ranks no longer fit a lower level.
CREATE TABLE character_skills (
    char_id  bigint   NOT NULL REFERENCES characters (id) ON DELETE CASCADE,
    skill_id smallint NOT NULL,
    rank     smallint NOT NULL,
    CONSTRAINT character_skills_pk PRIMARY KEY (char_id, skill_id),
    CONSTRAINT character_skills_skill CHECK (skill_id BETWEEN 1 AND 63),
    CONSTRAINT character_skills_rank CHECK (rank BETWEEN 1 AND 3)
);

-- The 8-slot skill bar (keys 1-8): skill ids, 0 = empty slot. NULL = the default bar (the class's
-- free skill on key 1), for new characters, characters from before M12 and after reset-skills.
ALTER TABLE characters ADD COLUMN skill_bar smallint[];
ALTER TABLE characters ADD CONSTRAINT characters_skill_bar CHECK (
    skill_bar IS NULL OR (cardinality(skill_bar) = 8 AND array_ndims(skill_bar) = 1 AND
                          array_position(skill_bar, NULL) IS NULL AND
                          0 <= ALL (skill_bar) AND 63 >= ALL (skill_bar)));
