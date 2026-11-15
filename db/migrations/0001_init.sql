-- 0001_init: accounts and characters (docs/DATABASE.md, docs/PROTOCOL.md §5.2-5.3).
-- Runs inside one transaction with search_path set to the target schema; never schema-qualify.
-- Immutable once applied anywhere: fix mistakes with a new migration.

CREATE TABLE accounts (
    id            bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    username      text        NOT NULL,            -- as entered
    username_key  text        NOT NULL,            -- vo::game::usernameKey(): ASCII lower case
    password_hash text        NOT NULL,            -- Argon2id encoded string (libsodium)
    created_at    timestamptz NOT NULL DEFAULT now(),
    last_login_at timestamptz,
    last_login_ip inet,
    banned        boolean     NOT NULL DEFAULT false,
    ban_reason    text,
    -- The protocol carries account ids as u32.
    CONSTRAINT accounts_id_u32 CHECK (id BETWEEN 1 AND 4294967295),
    CONSTRAINT accounts_username_format CHECK (username ~ '^[A-Za-z0-9_]{4,20}$'),
    CONSTRAINT accounts_username_key_format CHECK (username_key = lower(username COLLATE "C")),
    CONSTRAINT accounts_password_hash_format
        CHECK (password_hash LIKE '$argon2id$%' AND octet_length(password_hash) <= 255),
    CONSTRAINT accounts_ban_reason_length CHECK (ban_reason IS NULL OR char_length(ban_reason) <= 200)
);

CREATE UNIQUE INDEX accounts_username_key_uq ON accounts (username_key);

CREATE TABLE characters (
    id             bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    account_id     bigint      NOT NULL REFERENCES accounts (id) ON DELETE RESTRICT,
    name           text        NOT NULL,           -- as entered (UTF-8)
    name_key       text        NOT NULL,           -- vo::game::characterNameKey(), computed in C++
    race           smallint    NOT NULL,
    class          smallint    NOT NULL,
    face           smallint    NOT NULL,
    hair           smallint    NOT NULL,
    level          integer     NOT NULL DEFAULT 1,
    zone_id        integer     NOT NULL,
    x              real        NOT NULL,
    y              real        NOT NULL,
    z              real        NOT NULL,
    yaw            real        NOT NULL,
    created_at     timestamptz NOT NULL DEFAULT now(),
    deleted_at     timestamptz,                    -- soft delete: the name stays reserved
    last_played_at timestamptz,
    CONSTRAINT characters_id_u32 CHECK (id BETWEEN 1 AND 4294967295),
    -- 3-16 letters (ASCII + Turkish) and digits, starting with a letter.
    CONSTRAINT characters_name_format
        CHECK (name ~ '^[A-Za-zçğıöşüÇĞİÖŞÜ][A-Za-z0-9çğıöşüÇĞİÖŞÜ]{2,15}$'),
    CONSTRAINT characters_name_key_length CHECK (char_length(name_key) BETWEEN 3 AND 16),
    CONSTRAINT characters_race CHECK (race IN (1, 2)),
    CONSTRAINT characters_class CHECK (class BETWEEN 1 AND 4),
    CONSTRAINT characters_face CHECK (face BETWEEN 0 AND 3),
    CONSTRAINT characters_hair CHECK (hair BETWEEN 0 AND 3),
    CONSTRAINT characters_level CHECK (level BETWEEN 1 AND 65535),
    CONSTRAINT characters_zone CHECK (zone_id BETWEEN 0 AND 65535),
    -- NaN compares greater than every number in PostgreSQL, so this also rejects NaN.
    CONSTRAINT characters_position_finite CHECK (
        x > '-Infinity' AND x < 'Infinity' AND y > '-Infinity' AND y < 'Infinity' AND
        z > '-Infinity' AND z < 'Infinity' AND yaw > '-Infinity' AND yaw < 'Infinity')
);

-- Unique across ALL rows, soft-deleted ones included: deleted names stay reserved.
CREATE UNIQUE INDEX characters_name_key_uq ON characters (name_key);

-- Listing / counting an account's live characters.
CREATE INDEX characters_account_live_idx ON characters (account_id, id) WHERE deleted_at IS NULL;

-- At most 3 live characters per account, race-free: the account row is locked before counting, so
-- concurrent inserts/restores for one account are serialised (READ COMMITTED: the count after the
-- lock sees every committed row). The account service checks the same rule first to answer
-- NoFreeSlot before NameTaken; this trigger is the invariant for every writer (vo_admin included).
CREATE FUNCTION characters_enforce_slot_limit() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    IF NEW.deleted_at IS NULL THEN
        PERFORM 1 FROM accounts WHERE id = NEW.account_id FOR UPDATE;
        IF (SELECT count(*) FROM characters
            WHERE account_id = NEW.account_id AND deleted_at IS NULL AND id <> NEW.id) >= 3 THEN
            RAISE EXCEPTION 'account % already has 3 live characters', NEW.account_id
                USING ERRCODE = 'check_violation', CONSTRAINT = 'characters_slot_limit';
        END IF;
    END IF;
    RETURN NEW;
END
$$;

CREATE TRIGGER characters_slot_limit
    BEFORE INSERT OR UPDATE OF deleted_at, account_id ON characters
    FOR EACH ROW EXECUTE FUNCTION characters_enforce_slot_limit();
