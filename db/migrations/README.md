# db/migrations

Plain SQL migration files for PostgreSQL, applied in order by the server at startup and recorded
in `schema_migrations(version, applied_at)`.

- Naming: `NNNN_short_description.sql` (e.g. `0001_init.sql`), zero-padded, never renumbered.
- A migration is immutable once committed; fix mistakes with a new migration.
- Each file runs in one transaction.

Owned by the server/DB work package (M2). Empty for now.
