# db/migrations

Plain SQL migration files for PostgreSQL, applied in order by `vo_server` at startup (and by
`vo_server --migrate-only` / `vo_admin migrate`), recorded in
`schema_migrations(version, name, checksum, applied_at)`. Full workflow: [docs/DATABASE.md](../../docs/DATABASE.md).

- Naming: `NNNN_short_description.sql` (e.g. `0001_init.sql`), zero-padded, lower-case, never renumbered.
- Forward-only and immutable once committed: the runner stores a SHA-256 of each file (CRLF
  normalised to LF) and refuses to start if an applied file changed. Fix mistakes with a new migration.
- Each file runs in one transaction with `search_path` set to the target schema: never
  schema-qualify names, never put `BEGIN`/`COMMIT` in the file.
- `0001_init.sql` must exist forever (the server locates this directory by it).
