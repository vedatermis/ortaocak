-- 0008_skill_ranks_5: skill ranks 4 and 5 (M15, docs/PROTOCOL_LEVELS.md §2, docs/DATABASE.md).
-- Runs inside one transaction with search_path set to the target schema; never schema-qualify.
-- Immutable once applied anywhere: fix mistakes with a new migration.

-- The level cap rose from 10 to 20 and every skill got two more ranks; rank 4 needs level 11 and
-- rank 5 level 16 (gamedata/skills.toml [rules] rank_levels, checked by the server's shared code: the
-- database cannot know the data file). Only the CHECK widens: every stored rank (1..3) stays valid
-- and is not rewritten. The level itself was never limited below 65535 (0001) and the stat-point
-- CHECK (0005: 3 points per level above 1) holds for levels 11-20 unchanged.
ALTER TABLE character_skills DROP CONSTRAINT character_skills_rank;
ALTER TABLE character_skills ADD CONSTRAINT character_skills_rank CHECK (rank BETWEEN 1 AND 5);
