-- 0002_items: item instances and the starter-kit flag (docs/PROTOCOL_ITEMS.md §5, docs/DATABASE.md).
-- Runs inside one transaction with search_path set to the target schema; never schema-qualify.
-- Immutable once applied anywhere: fix mistakes with a new migration.

-- Set (in the same transaction as the kit's rows) the first time the character enters the world.
-- Existing characters start with false and get their kit on their next entry.
ALTER TABLE characters ADD COLUMN starter_kit_granted boolean NOT NULL DEFAULT false;

-- One row per item instance. Templates (template_id) live in gamedata/items.toml, not here: a row
-- whose template the server does not know is kept, logged and hidden from the client.
CREATE TABLE character_items (
    item_id     bigserial   PRIMARY KEY,        -- unique forever (u64 on the wire)
    char_id     bigint      NOT NULL REFERENCES characters (id) ON DELETE CASCADE,
    container   smallint    NOT NULL,           -- 0 Equipment, 1 Bag
    slot        smallint    NOT NULL,           -- Equipment 0..7, Bag 0..27
    template_id integer     NOT NULL,
    quantity    integer     NOT NULL DEFAULT 1,
    created_at  timestamptz NOT NULL DEFAULT now(),
    CONSTRAINT character_items_container CHECK (container IN (0, 1)),
    CONSTRAINT character_items_slot CHECK (
        (container = 0 AND slot BETWEEN 0 AND 7) OR (container = 1 AND slot BETWEEN 0 AND 27)),
    CONSTRAINT character_items_template CHECK (template_id BETWEEN 1 AND 65535),
    CONSTRAINT character_items_quantity CHECK (quantity BETWEEN 1 AND 65535),
    -- One item per place. Deferrable: a swap updates both rows and is checked at commit.
    CONSTRAINT character_items_place_uq UNIQUE (char_id, container, slot) DEFERRABLE INITIALLY IMMEDIATE
);
