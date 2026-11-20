-- 0011_accessory_slots: the accessory equipment slots (M18, docs/PROTOCOL_EQUIPMENT.md §5,
-- docs/DATABASE.md).
-- Runs inside one transaction with search_path set to the target schema; never schema-qualify.
-- Immutable once applied anywhere: fix mistakes with a new migration.

-- The Equipment container grows from 8 slots (0..7, 7 reserved and always empty) to 13:
-- 7 necklace, 8 belt, 9 / 10 rings, 11 / 12 earrings. Existing rows are untouched (no row could be in
-- slot 7: the server never put anything there). Which template fits which slot is the server's shared
-- code (gamedata/items.toml); the database only keeps the place in range.
ALTER TABLE character_items DROP CONSTRAINT character_items_slot;
ALTER TABLE character_items ADD CONSTRAINT character_items_slot CHECK (
    (container = 0 AND slot BETWEEN 0 AND 12) OR (container = 1 AND slot BETWEEN 0 AND 27));
