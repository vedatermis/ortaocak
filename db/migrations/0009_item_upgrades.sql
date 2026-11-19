-- 0009_item_upgrades: the upgrade level of an item instance (M16, docs/PROTOCOL_UPGRADE.md §6,
-- docs/DATABASE.md).
-- Runs inside one transaction with search_path set to the target schema; never schema-qualify.
-- Immutable once applied anywhere: fix mistakes with a new migration.

-- Weapons, shields and armour are upgraded at the smiths, +0..+10, per item instance. Every existing
-- row starts at 0 (nothing was ever upgraded before M16); nothing else is rewritten. Which templates
-- may be upgraded (not stackables, materials or potions) is the server's shared code: the database
-- cannot know gamedata/items.toml. It only keeps an upgraded row a single unit.
ALTER TABLE character_items ADD COLUMN upgrade smallint NOT NULL DEFAULT 0;
ALTER TABLE character_items ADD CONSTRAINT character_items_upgrade CHECK (upgrade BETWEEN 0 AND 10);
ALTER TABLE character_items ADD CONSTRAINT character_items_upgrade_single CHECK (upgrade = 0 OR quantity = 1);
