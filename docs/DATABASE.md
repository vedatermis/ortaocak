# VedatOnline Database (PostgreSQL)

Status: M2. The server stores accounts and characters in PostgreSQL (tested against 16; the plan
targets 17). Without a configured database it falls back to the in-memory account service, which
loses everything on exit.

## 1. Configuration

`[db]` in `config/server.toml` (committed defaults) + `config/server.local.toml` (git-ignored, see
`config/server.local.toml.example`). Secrets only ever go in the local file or the environment.

| Key | Default | Meaning |
|---|---|---|
| `connection_string` | `""` | libpq key/value string or `postgresql://` URI of the game database. Empty = no database. Env `VEDAT_DB_URL` wins. |
| `admin_connection_string` | `""` | Maintenance DB (`.../postgres`). Used **only** to `CREATE DATABASE` the game database if it is missing. Env `VEDAT_DB_ADMIN_URL`. |
| `test_connection_string` | — | Read only by the tests (§6). Must name `vedatonline_test`. Env `VEDAT_TEST_DB_URL`. |
| `schema` | `"public"` | Schema holding all tables (`search_path` of every connection). |
| `pool_size` | `4` | Pooled connections. |
| `worker_threads` | `4` | DB worker threads; they also run the Argon2id hashing. |
| `statement_timeout_ms` | `5000` | Per statement (`0` = none). Migrations always run without a timeout. |
| `connect_timeout_s` | `5` | libpq `connect_timeout`. |
| `required` | `true` | Refuse to start when the database is unreachable or migrating fails (exit code 5). `false`: log an error and fall back to the in-memory service. |

Selection: `connection_string` non-empty → `PostgresAccountService`; empty → `MemoryAccountService`.
The startup log shows which one is used, the target as `user@host:port/db` (never the password),
the server version and whether the connection is encrypted.

**TLS.** libpq's default `sslmode=prefer` encrypts when the server offers TLS (the current dev
server does: TLSv1.3) but silently falls back to plaintext and does not verify the certificate.
For any server that is not on localhost use `sslmode=require` at least, ideally `verify-full` with
`sslrootcert=`. The server logs a warning when the connection is not encrypted.

## 2. Startup sequence

1. If `admin_connection_string` is set and the database named in `connection_string` does not
   exist, create it (`TEMPLATE template0 ENCODING 'UTF8' LC_COLLATE/LC_CTYPE 'C'`). Nothing else is
   ever done on the admin connection; only lower-case identifier names are accepted.
2. Connect, log server version and TLS state.
3. Apply pending migrations (§4), log each applied version.
4. `vo_server --migrate-only` exits here (0 = ok, 5 = failed).
5. Open the pool (one connection up front as a reachability check) and start serving.

## 3. Schema (0001_init)

All timestamps are `timestamptz`; every session runs with `TimeZone = 'UTC'`.

**accounts**

| Column | Type | Notes |
|---|---|---|
| `id` | `bigint` identity PK | CHECK 1..2³²−1 (u32 on the wire) |
| `username` | `text` | as entered; CHECK `^[A-Za-z0-9_]{4,20}$` |
| `username_key` | `text` | `vo::game::usernameKey()` (ASCII lower case); **unique index**; CHECK = `lower(username)` |
| `password_hash` | `text` | Argon2id encoded string (libsodium `crypto_pwhash_str`) |
| `created_at`, `last_login_at` | `timestamptz` | |
| `last_login_ip` | `inet` | NULL if unknown |
| `banned`, `ban_reason` | `boolean`, `text` | reason ≤ 200 characters |

**characters**

| Column | Type | Notes |
|---|---|---|
| `id` | `bigint` identity PK | CHECK u32 range |
| `account_id` | `bigint` FK → accounts | `ON DELETE RESTRICT` |
| `name` | `text` | as entered; CHECK 3–16 letters (ASCII + `çğıöşüÇĞİÖŞÜ`) / digits, starting with a letter |
| `name_key` | `text` | `vo::game::characterNameKey()` computed in C++ (all i-like letters fold to one key); **unique across all rows, soft-deleted included** |
| `race`, `class`, `face`, `hair` | `smallint` | CHECK race 1–2, class 1–4, face/hair 0–3 |
| `level` | `integer` | CHECK 1..65535 |
| `zone_id`, `x`, `y`, `z`, `yaw` | `integer`, `real` | CHECK finite (no NaN/∞) |
| `created_at`, `deleted_at`, `last_played_at` | `timestamptz` | `deleted_at` set = soft-deleted; the name stays reserved |

Indexes: `accounts_username_key_uq`, `characters_name_key_uq`, and the partial index
`characters_account_live_idx (account_id, id) WHERE deleted_at IS NULL` for listing/counting.

**Race-freedom.**
- *Auto-registration*: `INSERT ... ON CONFLICT (username_key) DO NOTHING RETURNING id`. The loser of
  a race verifies its password against the winner's row like a normal login.
- *Name uniqueness*: only the unique index decides; its violation maps to `NameTaken`.
- *3 live characters*: character creation locks the account row (`SELECT ... FOR UPDATE`) before
  counting, so creates for one account are serialised. The trigger `characters_slot_limit`
  enforces the same invariant for every writer (inserts and un-deletes, `vo_admin` included).
  Both rely on READ COMMITTED (the default).

## 4. Migrations

Files: `db/migrations/NNNN_name.sql`, applied in version order. Rules:

- **Forward-only and immutable.** `schema_migrations(version, name, checksum, applied_at)` stores
  the SHA-256 of each applied file (CRLF→LF normalised, BOM stripped). A changed checksum, an
  applied version without a file, or a new file numbered below the newest applied version is a
  hard error: the server does not start (exit 5) and nothing is applied.
- Each file runs in **one transaction** together with its `schema_migrations` row; a failing file
  leaves no trace. Do not write `BEGIN`/`COMMIT` or schema-qualified names.
- Concurrent runners (two servers, server + `vo_admin`) are serialised by
  `pg_advisory_lock(0x564F4D47, hashtext(<schema>))`; the second one waits, then finds nothing to do.
- The database must be UTF8.

Adding a change: create `db/migrations/0002_what_it_does.sql`, run `vo_admin migrate` (or just
start the server) against your local database, run `build.ps1 -Test` (the tests migrate fresh
schemas from scratch), commit.

## 5. vo_admin

Same config loading as the server (`--config <path>`, `server.local.toml`, `VEDAT_DB_URL`).

```
vo_admin migrate                              # create DB if missing + apply migrations
vo_admin status                               # connection, TLS, schema version, counts
vo_admin account create <user>                # prompts for the password twice (no echo)
vo_admin account passwd <user>
vo_admin account ban <user> [reason...]
vo_admin account unban <user>
vo_admin account delete <user> --yes          # remove the account and all its characters for good
vo_admin character list <user>                # all characters incl. soft-deleted
vo_admin character rename <user> <id> <new_name>
vo_admin character restore <user> <id>        # undo a soft delete (needs a free slot)
vo_admin character purge <user> <id> --yes    # delete a soft-deleted character for good (frees the name)
vo_admin testdb drop-schema <vo_test_*|vo_it_*>   # only in vedatonline_test
vo_admin testdb sweep                         # drop test schemas older than 2 h
```

Passwords are never command-line arguments (they would end up in shell history and process
lists). For scripts, pipe the password from a secret store into `--password-stdin`, which reads
the first line of stdin:

```
Get-Secret vo-admin-pw -AsPlainText | vo_admin account create Vedat --password-stdin
```

Exit codes: 0 ok, 1 usage, 2 config, 3 not found / invalid / refused, 4 database error.

## 6. Test database

`vedatonline_test` is **disposable**: tests may create and drop anything inside it, never touch it
for real data. It is created on demand (through `admin_connection_string`) if missing.

- URL: `VEDAT_TEST_DB_URL`, else `db.test_connection_string` in `config/server.local.toml`. The
  tests refuse any URL whose database is not `vedatonline_test` (checked on the URL and with
  `current_database()`).
- Without a URL, or with `VEDAT_TEST_DB_URL=skip`, every PostgreSQL test is **skipped** (ctest
  shows "Skipped"), so `build.ps1 -Test` passes on machines without PostgreSQL.
- Every unit test runs in its own schema `vo_test_<unix time>_<random>`, every DB integration
  scenario (`integration.db.*`) in `vo_it_<unix time>_<random>`; they are dropped afterwards, and
  leftovers of crashed runs older than two hours are swept automatically.
- The pool-recovery test terminates (`pg_terminate_backend`) only its own pooled connections,
  identified by backend PID, database `vedatonline_test` and `application_name = vo_tests`.

## 7. Operations

- **Backups.** `pg_dump -Fc -d vedatonline -f vedatonline_YYYYMMDD.dump` (restore with
  `pg_restore -d <empty db>`). Take one before every deployment that adds a migration; there are
  no down-migrations.
- **What is logged.** Connection targets as `user@host:port/db`, server version, TLS state,
  applied migrations, and DB errors as their first message line + SQLSTATE. Never passwords,
  password hashes (DETAIL lines that could quote row contents are dropped) or full connection strings.
- **Failure behaviour.** A failed request is logged and answered with the protocol's
  `ServerError` result (`LOGIN_RES 5`, `CHAR_CREATE_RES 6`, ...); the session stays open, except
  for `CHAR_LIST_REQ`, whose response has no result field (`ERROR(InternalError)` + close).
  Lost connections are re-opened transparently (a job interrupted by a dead connection is retried
  once); while the database is down, reconnects back off from 100 ms to 5 s and requests fail fast.
- **Ordering.** Requests of one account run in submission order; different accounts run in parallel
  on the worker threads.
