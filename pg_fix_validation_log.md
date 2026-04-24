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

## 2026-04-20 - Iteration 4: remove MySQL-only option entries

### Scope

- Removed MySQL-only command-line option definitions from `help.cpp` instead of
  keeping hidden/inert options.
- Removed legacy MySQL option aliases from argument normalization, keeping only
  `--no-virtual` as a PostgreSQL-compatible alias for generated columns.
- Removed server-option parsing state and declarations that were only used by
  MySQL-style server variable mutation.
- Removed generated-column column-compression access after deleting the
  corresponding MySQL/Percona-only option.

### Validation

- Build: `cmake --build build -j4`
- Help check: `./build/src/pstress-pg --help` no longer exposes the deleted
  MySQL-only option names or legacy aliases.
- 3-minute local run against `127.0.0.1:5432`
- Command shape: `--tables=3 --threads=2 --seconds=180`
- Result: completed with exit code `0`
- Summary: `346/1045435` queries failed, `99.97%` successful
- Remaining failures: duplicate primary keys (`23505`), FK conflicts (`23503`),
  transaction-abort follow-up errors (`25P02`), and deadlocks (`40P01`).

## 2026-04-23 - Iteration 5: decouple PostgreSQL FK references from INT primary keys

### Scope

- Added explicit FK reference metadata on `FK_table`: parent table, parent
  referenced column, and child FK column.
- Changed FK generation from fixed `ifk_col INTEGER REFERENCES parent(pkey)` to
  single-column PostgreSQL-compatible references.
- FK parent references can now target either a primary-key column or a generated
  unique index on a regular indexable column.
- FK child columns now copy the parent reference column type and length, so
  cases such as `VARCHAR` FK columns are covered.
- Initial load generates deterministic unique values for non-primary referenced
  columns so parent unique indexes can be created reliably.
- Random insert/update values for referenced unique columns use lower-collision
  expressions, and FK-supporting unique indexes are not selected for random
  `DROP INDEX`.

### Validation

- Build: `cmake --build build -j4`
- Targeted smoke: `--tables=3 --threads=1 --seconds=30 --fk-prob=100
  --pk-prob=0 --columns=8 --indexes=4`
- Targeted smoke result: completed with exit code `0`; generated non-PK FK DDL
  such as `FOREIGN KEY (vfk_col) REFERENCES tt_1 (v2)`.
- 3-minute local run against `127.0.0.1:5432`
- Command shape: `--tables=3 --threads=2 --seconds=180`
- Result: completed with exit code `0`
- Summary: `450/347266` queries failed, `99.87%` successful
- Confirmed DDL: `CREATE UNIQUE INDEX tt_1_fkref_v2 ON tt_1(v2)` and
  `FOREIGN KEY (vfk_col) REFERENCES tt_1 (v2)`.
- Remaining failures: FK conflicts (`23503`), duplicate unique/primary values
  (`23505`), deadlocks (`40P01`), and transaction-abort follow-up errors
  (`25P02`).

## 2026-04-23 - Iteration 6: add P0 PostgreSQL scalar data types

### Scope

- Added PostgreSQL scalar column types to random metadata generation:
  `SMALLINT`, `BIGINT`, `NUMERIC`, `DATE`, `TIME`, `TIME WITH TIME ZONE`,
  `TIMESTAMP WITH TIME ZONE`, `INTERVAL`, `BYTEA`, `UUID`, and `JSONB`.
- Split PostgreSQL `JSON` and `JSONB` behavior instead of treating all JSON
  columns as one type.
- Added value generators, deterministic unique expressions, DML update/delete
  handling, index-width estimates, and generated-column expression support for
  the new type family.
- Avoided selecting `JSON` columns for equality predicates because PostgreSQL
  `json` has no equality operator; `JSONB` remains eligible for predicate use.
- Kept generated-column expressions immutable for date/time/binary/text-derived
  terms by avoiding non-immutable casts where PostgreSQL rejects them.

### Validation

- Build: `cmake --build build -j4`
- Targeted smoke: `--tables=4 --threads=1 --seconds=30 --columns=12
  --indexes=4`
- Targeted smoke result: completed with exit code `0`; no `syntax error`,
  `FATAL`, `42883`, `42P17`, or `22008` structural type-generation failures.
- Smoke summary: `274/134060` queries failed, `99.80%` successful.
- 3-minute local run against `127.0.0.1:5432`
- Command shape: `--tables=3 --threads=2 --seconds=180 --columns=12
  --indexes=4`
- Result: completed with exit code `0`
- Summary: `258/241196` queries failed, `99.89%` successful
- Confirmed DDL/value coverage in this random sample: `BIGINT`, `NUMERIC`,
  `DATE`, `TIME`, `UUID`, and `JSON`.
- Added but not sampled by this particular 3-minute random run: `SMALLINT`,
  `TIME WITH TIME ZONE`, `TIMESTAMP WITH TIME ZONE`, `INTERVAL`, `BYTEA`, and
  `JSONB`.
- Remaining failures: FK conflicts (`23503`), duplicate unique/primary values
  (`23505`), deadlocks (`40P01`), transaction-abort follow-up errors (`25P02`),
  and one random check/partition constraint miss (`23514`).

## 2026-04-23 - Iteration 7: add P1 PostgreSQL network, bit, money, XML, and text-search types

### Scope

- Added PostgreSQL P1 type coverage to random metadata generation:
  `BIT`, `BIT VARYING`, `INET`, `CIDR`, `MACADDR`, `MACADDR8`, `MONEY`,
  `XML`, `TSVECTOR`, and `TSQUERY`.
- Added PostgreSQL-safe random value generators, deterministic unique values,
  DML value formatting, metadata reload handling, and type-name round-tripping
  for the new P1 family.
- Enabled comparable P1 types such as `BIT`, `INET`, `CIDR`, `MACADDR`,
  `MACADDR8`, and `MONEY` for random `WHERE` predicate selection, while keeping
  `XML`, `TSVECTOR`, and `TSQUERY` out of unsupported equality paths.
- Kept generated columns PostgreSQL-valid by filtering out non-immutable source
  types such as `TIMETZ`, `TIMESTAMPTZ`, `XML`, `TSVECTOR`, `TSQUERY`, and
  `MONEY`.
- Wrapped generated integer arithmetic through bounded `NUMERIC` expressions to
  avoid `SMALLINT`/`INT`/`BIGINT` overflow during random generated-column DDL.

### Validation

- Build: `cmake --build build -j4`
- Targeted smoke: `--tables=4 --threads=1 --seconds=30 --columns=16
  --indexes=4`
- Smoke findings during fix loop:
  invalid `CIDR` literals (`22P02`), duplicate unique values on `CIDR`
  referenceable columns (`23505`), non-immutable generated expressions
  (`42P17`), and generated integer overflow (`22003`).
- Smoke result after fixes: completed with exit code `0`; no `syntax error`,
  `FATAL`, `42P17`, `42883`, `22P*`, or `22003` structural failures remained.
- 3-minute local run against `127.0.0.1:5432`
- Command shape: `--tables=3 --threads=2 --seconds=180 --columns=16
  --indexes=4`
- Result: completed with exit code `0`
- Summary: `1369/600163` queries failed, `99.77%` successful
- Confirmed DDL/value coverage in this random sample: `BIT`, `BIT VARYING`,
  `INET`, `CIDR`, `MACADDR`, `MACADDR8`, `MONEY`, `XML`, `TSVECTOR`, and
  `TSQUERY`.
- Remaining failures: duplicate unique/primary values (`23505`), FK conflicts
  (`23503`), transaction-abort follow-up errors (`25P02`), and deadlocks
  (`40P01`).

## 2026-04-23 - Iteration 8: add P2 PostgreSQL geometric types

### Scope

- Added PostgreSQL geometric type coverage to random metadata generation:
  `POINT`, `LINE`, `LSEG`, `BOX`, `PATH`, `POLYGON`, and `CIRCLE`.
- Added PostgreSQL-valid geometric literal/value generators for insert, update,
  and metadata reload paths.
- Integrated the new P2 family into add/modify-column flows, type-name parsing,
  type-name serialization, and random schema generation.
- Kept the first P2 integration conservative by excluding geometric types from
  FK parent selection, generated-column source selection, default B-tree index
  selection, and random equality/range predicate selection.
- Fixed a pre-existing PostgreSQL partition-table issue where random inserts
  used `ON CONFLICT` against partitioned tables without a guaranteed matching
  unique target; partition-table inserts now skip `ON CONFLICT`.

### Validation

- Build: `cmake --build build -j4`
- Targeted smoke: `--tables=4 --threads=1 --seconds=30 --columns=16
  --indexes=4`
- Smoke findings during fix loop:
  geometric columns were initially selected for default B-tree indexes
  (`42704`), and partition-table inserts surfaced invalid `ON CONFLICT`
  targets (`42P10`).
- Smoke result after fixes: completed with exit code `0`; no `syntax error`,
  `FATAL`, `42704`, `42883`, `42P10`, `22P*`, `22003`, or `42P17` structural
  failures remained.
- 3-minute local run against `127.0.0.1:5432`
- Command shape: `--tables=3 --threads=2 --seconds=180 --columns=16
  --indexes=4`
- Result: completed with exit code `0`
- Summary: `395/830482` queries failed, `99.95%` successful
- Confirmed DDL/value coverage in this random sample: `POINT`, `LINE`, `LSEG`,
  `BOX`, `PATH`, `POLYGON`, and `CIRCLE`.
- Remaining failures: duplicate unique/primary values (`23505`), FK conflicts
  (`23503`), deadlocks (`40P01`), and random check/partition constraint misses
  (`23514`).

## 2026-04-23 - Iteration 9: add PostgreSQL array and range types

### Scope

- Added PostgreSQL array coverage to random metadata generation:
  `INT[]`, `BIGINT[]`, `NUMERIC[]`, `TEXT[]`, `BOOLEAN[]`, and
  `TIMESTAMP[]`.
- Added PostgreSQL built-in range coverage to random metadata generation:
  `INT4RANGE`, `INT8RANGE`, `NUMRANGE`, `TSRANGE`, `TSTZRANGE`, and
  `DATERANGE`.
- Added PostgreSQL-safe random value generators for arrays and ranges, using
  typed `ARRAY[...]` expressions plus constructor-style range expressions.
- Integrated the new P3 family into add/modify-column flows, type parsing,
  type serialization, metadata reload, and random schema generation.
- Kept the first P3 integration conservative by excluding arrays and ranges
  from FK parent selection, generated-column source selection, default B-tree
  index selection, and random comparison-predicate selection.
- Fixed range generation so date/time range constructors always emit
  non-decreasing bounds; this removed `22000` failures from invalid range
  endpoints.

### Validation

- Build: `cmake --build build -j4`
- Targeted smoke: `--tables=4 --threads=1 --seconds=30 --columns=16
  --indexes=4`
- Smoke findings during fix loop:
  date/time range constructors initially emitted reversed bounds and triggered
  `22000`.
- Smoke result after fixes: completed with exit code `0`; no `syntax error`,
  `FATAL`, `42704`, `42883`, `42P10`, `22000`, `22003`, or `42P17`
  structural failures remained.
- Smoke confirmed DDL/value coverage for the new family, including `INT[]`,
  `NUMERIC[]`, `TEXT[]`, `BOOLEAN[]`, `TIMESTAMP[]`, `INT4RANGE`,
  `INT8RANGE`, `NUMRANGE`, `TSTZRANGE`, and `DATERANGE`.
- 3-minute local run against `127.0.0.1:5432`
- Command shape: `--tables=3 --threads=2 --seconds=180 --columns=16
  --indexes=4`
- Result: completed with exit code `0`
- Summary: `814/258087` queries failed, `99.68%` successful
- Confirmed DDL/value coverage in this random sample: `BIGINT[]`, `NUMERIC[]`,
  `TEXT[]`, `BOOLEAN[]`, `TIMESTAMP[]`, `INT8RANGE`, `NUMRANGE`, `TSRANGE`,
  `TSTZRANGE`, and `DATERANGE`.
- Remaining failures: duplicate unique/primary values (`23505`), FK conflicts
  (`23503`), transaction-abort follow-up errors (`25P02`), and deadlocks
  (`40P01`).

## 2026-04-23 - Iteration 10: improve DML hit rate with cached real values

### Scope

- Implemented the first PostgreSQL-only version of the recommended
  `Option 1 + Option 5` plan from
  `dml_hit_rate_improvement_options.md`.
- Added a per-table bounded real-value cache keyed by column name, used to
  retain recently inserted scalar values that are safe to reuse in `WHERE`
  predicates.
- Seeded the cache from successful bulk-load inserts and successful random
  inserts, so the hit-oriented path has real values available immediately
  after initial table load.
- Switched `DELETE ... WHERE`, `SELECT ... WHERE`, and `UPDATE ... WHERE`
  to a mixed strategy:
  - `70%` hit-oriented predicates using cached real values
  - `30%` original random predicates as fallback
- Kept the first iteration conservative by limiting hit-oriented predicates to
  simple scalar PostgreSQL column types and prioritizing columns in this
  order:
  `PK` -> `referenced unique/FK-supporting key` -> other scalar columns.
- Added cache lifecycle handling for `DROP/CREATE`, `TRUNCATE`,
  `DELETE ALL ROWS`, column rename, and column drop so stale values do not
  keep accumulating across destructive table changes.
- Explicitly skipped unsafe cache sources such as `DEFAULT`, `NULL`, and
  subquery-based expressions like FK `(SELECT ... LIMIT 1)` value generation.

### Validation

- Build: `cmake --build build -j4`
- Targeted smoke: `--tables=3 --threads=2 --seconds=30`
- Smoke result: completed with exit code `0`
- Smoke summary: `34/436753` queries failed, `99.99%` successful
- Smoke found no thread failures and no new structural errors such as
  `syntax error`, `FATAL`, `42P10`, `42704`, `42883`, or `22P*`.
- 3-minute local run against `127.0.0.1:5432`
- Command shape: `--tables=3 --threads=2 --seconds=180`
- Result: completed with exit code `0`
- Summary: `83/998776` queries failed, `99.99%` successful
- Remaining failures in this sample were only expected concurrent-random
  classes: duplicate unique/primary values (`23505`), transaction-abort
  follow-up errors (`25P02`), and deadlocks (`40P01`).
- No `Thread N failed`, `syntax error`, `FATAL`, or other new structural
  regressions were observed in the 3-minute run.

### Hit-rate comparison

- Added a same-shape before/after comparison using successful SQL log lines in
  the form `S <sql> rows:<n>`.
- Comparison method:
  - baseline: pre-hit-rate-change code at revision `3f9b07b`
  - candidate: current working tree with cached real-value `WHERE` logic
  - shared command shape:
    `--address=127.0.0.1 --port=5432 --user=gongliangbiao --database=postgres --tables=3 --threads=2 --seconds=180 --log-all-queries --log-failed-queries`
  - hit definition: `rows > 0`
- Baseline summary: `109/1025930` failed, `99.99%` successful
- Candidate summary: `83/998776` failed, `99.99%` successful
- Measured hit-rate change:
  - `SELECT`: `11.80%` -> `56.40%` (`+44.60` percentage points, `4.78x`)
  - `UPDATE`: `7.15%` -> `54.56%` (`+47.41` percentage points, `7.63x`)
  - `DELETE`: `6.29%` -> `54.48%` (`+48.19` percentage points, `8.66x`)
- Additional observations:
  - successful `SELECT` count changed from `459107` to `447591` (`-2.51%`)
  - successful `UPDATE` count changed from `111856` to `108353` (`-3.13%`)
  - successful `DELETE` count changed from `112499` to `108705` (`-3.37%`)
  - average rows per successful `SELECT` changed from `5.74` to `4.03`
  - average rows per successful `UPDATE` changed from `2.26` to `2.07`
  - average rows per successful `DELETE` changed from `2.30` to `2.42`
- Interpretation:
  the cached-value mixed strategy materially improved non-empty result /
  affected-row probability for `SELECT`, `UPDATE`, and `DELETE` without
  introducing a visible stability regression in the 3-minute validation run.
