-- 0007_bar_items: consumables on the skill bar (docs/PROTOCOL_SHOP.md §4.1, §7, docs/DATABASE.md).
-- Runs inside one transaction with search_path set to the target schema; never schema-qualify.
-- Immutable once applied anywhere: fix mistakes with a new migration.

-- A skill bar slot may now hold a consumable item as well as a skill. Stored elements: 0 = empty,
-- 1..63 = a skill id (as 0006), -1..-32767 = an item slot: minus the item template id (the wire's
-- 0x8000 | template id). Which templates are consumables lives in gamedata/items.toml and is checked
-- by the server's code. Only the CHECK widens: every existing bar (all elements 0..63) stays valid
-- and is not rewritten; NULL still means the default bar.
ALTER TABLE characters DROP CONSTRAINT characters_skill_bar;
ALTER TABLE characters ADD CONSTRAINT characters_skill_bar CHECK (
    skill_bar IS NULL OR (cardinality(skill_bar) = 8 AND array_ndims(skill_bar) = 1 AND
                          array_position(skill_bar, NULL) IS NULL AND
                          -32767 <= ALL (skill_bar) AND 63 >= ALL (skill_bar)));
