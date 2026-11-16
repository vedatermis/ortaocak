-- 0004_loot: the Akçe (coin) balance of characters (docs/PROTOCOL_LOOT.md §6, docs/DATABASE.md).
-- Runs inside one transaction with search_path set to the target schema; never schema-qualify.
-- Immutable once applied anywhere: fix mistakes with a new migration.

-- Coins of a character (u32 on the wire, capped at 1 000 000 000). Existing characters start with 0.
-- Written by loot takes (one transaction with the item rows) and vo_admin set-coins.
ALTER TABLE characters ADD COLUMN coins bigint NOT NULL DEFAULT 0;
ALTER TABLE characters ADD CONSTRAINT characters_coins CHECK (coins BETWEEN 0 AND 1000000000);

-- Stacks need no schema change: character_items.quantity (0002, 1..65535) holds the stack size.
