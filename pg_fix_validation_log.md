# PostgreSQL Fix Validation Log

## 2026-04-08 - Baseline stabilization before reopening random DDL

### Scope

- Fix PostgreSQL table-definition failures caused by MySQL-style type lengths.
- Replace PostgreSQL generated-column text expressions from `CONCAT(...)` to `||`.
- Correct PostgreSQL partition-key references for partition tables.
- Temporarily disable high-failure PostgreSQL DDL paths:
  `add-drop-partition`, `drop-column`, `modify-column`, `rename-column`,
  `recreate-table`.

### Validation

- 5-minute local run against `127.0.0.1:5432`
- Command shape: `--tables=3 --threads=2 --seconds=300`
- Result: completed with exit code `0`
- Summary: `15641/1115154` queries failed, `98.60%` successful

### Dominant remaining failures after baseline

- `VACUUM cannot run inside a transaction block`
- `operator does not exist: <type> ~~ unknown`
- `column "version" does not exist` from grammar SQL
- occasional duplicate add-column/index names
- oversized PostgreSQL btree indexes
- generated column length overflow

## 2026-04-08 - Iteration 1: VACUUM, LIKE, grammar version probe

### Scope

- Map PostgreSQL optimize path from `VACUUM ANALYZE` to `ANALYZE`.
- Prevent `LIKE` from being generated on non-text PostgreSQL column types.
- Replace grammar query `SELECT @@VERSION` with `SELECT version()`.

### Validation run A

- 3-minute local run against `127.0.0.1:5432`
- Command shape: `--tables=3 --threads=2 --seconds=180`
- Result: completed with exit code `0`
- Summary: `12703/1046880` queries failed, `98.79%` successful

### Findings after run A

- `VACUUM cannot run inside a transaction block` no longer reproduced.
- `SELECT @@VERSION` failures still reproduced because runtime grammar file in
  `build/src/grammar.sql` had not been refreshed.
- `LIKE` failures still reproduced because generated columns were still treated
  as text-compatible without checking generated subtype.

### Iteration 1 follow-up

- Refined PostgreSQL `LIKE` support to allow generated columns only when their
  generated subtype is text-like.
- Changed build flow so `src/grammar.sql` is copied to `build/src/grammar.sql`
  after linking.

### Validation run B

- 3-minute local run against `127.0.0.1:5432`
- Command shape: `--tables=3 --threads=2 --seconds=180`
- Result: completed with exit code `0`
- Summary: `395/941906` queries failed, `99.96%` successful

### Remaining dominant failures after run B

- duplicate column names and duplicate index names
- oversized PostgreSQL btree indexes
- generated-column text-length overflow causing invalid negative lengths
- occasional duplicate primary-key inserts from the random workload

## 2026-04-20 - Iteration 2: PG-only cleanup plus TIMESTAMP/JSONB coverage

### Scope

- Removed the `is_postgresql` compatibility layer and collapsed remaining runtime
  fork branches to PostgreSQL-only behavior.
- Added `TIMESTAMP` and `JSONB` column generation, metadata loading, random value
  generation, DML predicates, generated-column sources, and index-width handling.
- Changed generated-column expressions for `TIMESTAMP` and `JSONB` to immutable
  PostgreSQL-safe expressions.
- Kept DDL inside random transactions from drifting the in-memory schema by
  committing before transactional DDL.
- Made FK-table random inserts/updates use live parent primary-key values.
- Skipped random `TRUNCATE` on tables referenced by FK constraints.

### Validation run A

- 3-minute local run against `127.0.0.1:5432`
- Command shape: `--tables=3 --threads=2 --seconds=180`
- Result: completed with exit code `0`
- Summary: `477/1000364` queries failed, `99.95%` successful
- Findings: `22003` and `42703` no longer reproduced; `42P17` still appeared
  from non-immutable generated-column expressions involving timestamp/json text.

### Validation run B

- 3-minute local run against `127.0.0.1:5432`
- Command shape: `--tables=3 --threads=2 --seconds=180`
- Result: completed with exit code `0`
- Summary: `459/892717` queries failed, `99.95%` successful
- Findings: `22003`, `42703`, and `42P17` no longer reproduced; remaining
  notable failure was `0A000` from truncating FK-referenced tables.

### Validation run C

- 3-minute local run against `127.0.0.1:5432`
- Command shape: `--tables=3 --threads=2 --seconds=180`
- Result: completed with exit code `0`
- Summary: `348/898258` queries failed, `99.96%` successful
- Key checked failures not reproduced: `22003`, `42703`, `42P17`, `0A000`

### Remaining expected random workload failures

- duplicate primary-key inserts from explicit random key values
- occasional FK conflicts under concurrent parent/child changes
- occasional transaction-abort follow-up errors after an expected constraint
  failure
- rare deadlocks under concurrent DML

## 2026-04-20 - Iteration 3: remove MySQL-only feature logic

### Scope

- Removed PostgreSQL-inert MySQL-only workload execution paths from
  `random_test.cpp`, including storage encryption, table compression, storage
  engine changes, server variable changes, tablespace encryption/rename/discard,
  key rotation, keyring reload, redo-log toggles, database encryption, and undo
  tablespace SQL.
- Removed now-unused MySQL-only in-memory state from `random_test.cpp`.
- Kept the old command-line options parseable in `help.cpp` for compatibility,
  but made them inert and non-workload options rather than disabling them at
  runtime from `random_test.cpp`.

### Validation

- Build: `cmake --build build -j4`
- 3-minute local run against `127.0.0.1:5432`
- Command shape: `--tables=3 --threads=2 --seconds=180`
- Result: completed with exit code `0`
- Summary: `7/473350` queries failed, `100.00%` successful
- Remaining failures: partition constraint misses from random partition-targeted
  inserts only (`23514`)
