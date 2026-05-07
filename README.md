# pstress-pg

`pstress-pg` is a PostgreSQL-oriented random workload generator adapted from the
original MySQL-focused `pstress` codebase.

This repository is now centered on **running concurrent random PostgreSQL
workloads against an existing PostgreSQL server**, with PostgreSQL-aware schema
generation, data types, DDL/DML behavior, and logging.

This README is written for the current PostgreSQL adaptation and supersedes the
old MySQL-centric usage notes.

## What This Tool Does

At a high level, `pstress-pg`:

- connects to an existing PostgreSQL instance
- creates and manages a dedicated schema named `pstress`
- generates random tables, indexes, and initial data
- runs concurrent random SQL and DDL workloads
- logs succeeded and failed SQL for later analysis
- optionally persists metadata between steps with `--prepare` and `--step`

The primary binary is:

```bash
build/src/pstress-pg
```

## Safety and Scope

This tool is destructive inside the `pstress` schema.

On a fresh direct run, it executes schema-reset operations such as:

- `DROP SCHEMA IF EXISTS pstress CASCADE`
- `CREATE SCHEMA pstress`
- `SET search_path TO pstress`

Use it only against:

- a dedicated test database
- a dedicated PostgreSQL user
- a server where schema churn and random concurrent DDL are acceptable

Current supported workflow is primarily:

- direct binary execution against an already running PostgreSQL server

Legacy upstream shell scripts and option generators under
[pstress/](/Users/gongliangbiao/Desktop/Codes/pstress/pstress) are still in the
repository, but they are historical MySQL-era artifacts and are **not** the
recommended PostgreSQL workflow.

## Build

### Prerequisites

- CMake `>= 3.10`
- a C++ compiler with C++17 support
- PostgreSQL client development headers and `libpq`

The build system looks for PostgreSQL using
[cmake/PQFindPostgreSQL.cmake](/Users/gongliangbiao/Desktop/Codes/pstress/cmake/PQFindPostgreSQL.cmake).

### Typical build

If PostgreSQL headers and libraries are in standard system paths:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

If autodetection fails, point CMake at the PostgreSQL installation prefix:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBASEDIR="$(pg_config --prefix)"
cmake --build build -j4
```

### Test the connection

```bash
./build/src/pstress-pg \
  --address=127.0.0.1 \
  --port=5432 \
  --user=your_user \
  --database=postgres \
  --test-connection
```

## Quick Start

### Basic direct run

```bash
./build/src/pstress-pg \
  --address=127.0.0.1 \
  --port=5432 \
  --user=your_user \
  --database=postgres \
  --tables=10 \
  --threads=4 \
  --seconds=60 \
  --logdir=./log \
  --log-all-queries \
  --log-failed-queries
```

Notes:

- `--database` is the database to connect to.
- The workload itself runs in schema `pstress` inside that database.
- `--logdir` receives the step log and per-thread SQL logs.

### Prepare + replay workflow

Step 1: create metadata and initial data only

```bash
./build/src/pstress-pg \
  --address=127.0.0.1 \
  --port=5432 \
  --user=your_user \
  --database=postgres \
  --tables=20 \
  --records=500 \
  --threads=8 \
  --prepare \
  --logdir=./log
```

Step 2: reload metadata and continue from the previous step

```bash
./build/src/pstress-pg \
  --address=127.0.0.1 \
  --port=5432 \
  --user=your_user \
  --database=postgres \
  --threads=8 \
  --seconds=180 \
  --step=2 \
  --logdir=./log
```

## What Was Adapted for PostgreSQL

The adaptation is not just a client-library swap. The current code now has
PostgreSQL-specific behavior in schema generation, type generation, predicates,
DDL paths, and recovery of in-memory metadata.

### Core workload behavior adapted

- PostgreSQL `libpq` connection path
- PostgreSQL schema bootstrap in `pstress`
- PostgreSQL-aware random value generation
- PostgreSQL-aware DML predicate generation
- PostgreSQL-aware index width checks
- PostgreSQL-aware generated-column expressions
- PostgreSQL metadata persistence and reload across steps
- PostgreSQL partition DDL / DML behavior
- PostgreSQL transactional DDL handling

### PostgreSQL data type coverage adapted

The current tool generates and exercises the following PostgreSQL type families.

Scalar and common built-ins:

- `SMALLINT`
- `INTEGER`
- `INT`
- `BIGINT`
- `NUMERIC`
- `CHAR`
- `VARCHAR`
- `BOOLEAN`
- `REAL`
- `DOUBLE PRECISION`
- `DATE`
- `TIME`
- `TIME WITH TIME ZONE`
- `TIMESTAMP`
- `TIMESTAMP WITH TIME ZONE`
- `INTERVAL`
- `BYTEA`
- `UUID`
- `JSON`
- `JSONB`

Network, bit, money, XML, and text search:

- `BIT`
- `BIT VARYING`
- `INET`
- `CIDR`
- `MACADDR`
- `MACADDR8`
- `MONEY`
- `XML`
- `TSVECTOR`
- `TSQUERY`

Geometric:

- `POINT`
- `LINE`
- `LSEG`
- `BOX`
- `PATH`
- `POLYGON`
- `CIRCLE`

Arrays:

- `INT[]`
- `BIGINT[]`
- `NUMERIC[]`
- `TEXT[]`
- `BOOLEAN[]`
- `TIMESTAMP[]`

Ranges:

- `INT4RANGE`
- `INT8RANGE`
- `NUMRANGE`
- `TSRANGE`
- `TSTZRANGE`
- `DATERANGE`

Not every type participates in every workload path. For stability, some complex
families are intentionally excluded from:

- foreign-key parent selection
- generated-column source selection
- default B-tree index generation
- some equality-based predicate paths

## What Was Removed

The PostgreSQL adaptation removed or retired MySQL-only functionality that no
longer makes sense in this fork.

### Removed MySQL-only feature areas

- engine-switch workload logic
- encryption-specific MySQL / Percona workload logic
- tablespace-encryption workload logic
- undo tablespace workload logic
- redo-log / master-key / keyring rotation workload logic
- row-format specific workload logic
- MySQL server-variable mutation workload logic
- MySQL-only option parsing and aliases for removed features
- dead MySQL-era option enums that no longer had runtime references

### Removed or effectively retired user-facing options

The old upstream README described many MySQL-only flags that are not part of the
supported PostgreSQL workflow anymore, including categories such as:

- engine selection
- encryption toggles
- tablespace mutation
- undo tablespace options
- redo log key rotation
- MySQL server option files
- MySQL `set global` / `set session` mutation knobs

### Still present but not meaningful on PostgreSQL

Two compatibility knobs remain accepted, but PostgreSQL ignores them:

- `--alter-algorithm`
- `--alter-lock`

They are retained only to keep CLI compatibility with older usage patterns.

## What Was Added

Beyond straight PostgreSQL compatibility, this adaptation added several new
PostgreSQL-focused capabilities.

### 1. PostgreSQL-only cleanup and simplification

- removal of MySQL-only execution branches
- removal of obsolete MySQL-only options from active help output
- dead-code cleanup of obsolete option enums and related residual logic

### 2. Expanded PostgreSQL type coverage

The project now covers broad PostgreSQL type families that were not available in
the original MySQL-oriented tool:

- JSON / JSONB
- timestamptz / timetz / interval
- bytea / uuid
- network types
- money / XML
- text-search types
- geometric types
- arrays
- ranges

### 3. Better DML hit rate

The PostgreSQL fork now keeps lightweight caches of recently successful values
and uses a mixed strategy for `WHERE` generation.

Practical effect:

- more `UPDATE` / `DELETE` / `SELECT` statements hit real rows
- fewer random DML statements are wasted on empty predicates
- workload remains partially random instead of turning into deterministic replay

### 4. Generated-column budgeting and fallback

Generated columns are still supported, but the PostgreSQL fork now uses a safer
budgeted strategy:

- per-table generated-column cap
- per-expression dependency cap
- safer source-type filtering
- fallback to plain columns when generated-column construction is too risky

This greatly improves stability on wide tables.

### 5. Dedicated transactional DDL

The project now supports a separate transactional DDL workload path:

- `--trx-ddl-prob-k`
- `--trx-ddl-size`

This is implemented as a dedicated success-first executor that:

- starts its own transaction
- runs a bounded number of DDL statements
- rolls the whole block back on failure
- restores in-memory schema metadata on rollback

This is separate from the legacy generic transaction path.

### 6. New read-query shapes

Two new PostgreSQL-relevant read workload options were added:

- `--select-with-join`
- `--select-with-cte`

These cover:

- metadata-driven `INNER JOIN`
- non-recursive read-only `WITH` queries

### 7. Step metadata replay support

The PostgreSQL fork now reliably supports:

- `--prepare`
- `--step=2+`
- `--metadata-path`

including PostgreSQL-specific types during metadata serialization and reload.

## Current Supported PostgreSQL Features

From a user perspective, the current PostgreSQL workload supports:

- normal tables
- temporary tables
- foreign-key child tables
- partition tables
- add/drop partition workload
- random insert/update/delete/select
- grammar SQL expansion
- random DDL such as add/drop column, add/drop index, rename column/index
- generated columns
- random transactions with commit / rollback / savepoint behavior
- dedicated transactional DDL blocks
- metadata replay between steps
- pquery mode

## Current Boundaries

The PostgreSQL adaptation is intentionally conservative in some areas.

### Transactional DDL

Current transactional DDL support is a safe subset, not “all DDL”.

The implementation currently focuses on:

- scratch-table create/drop
- safe add/drop column
- add/drop/rename index
- rename column

It intentionally avoids more dangerous schema operations in the transactional
DDL path.

### Query-shape coverage

Implemented:

- plain random `SELECT`
- metadata-driven `JOIN`
- non-recursive read-only `CTE`

Not yet implemented:

- prepared statement workload
- view workload
- trigger workload
- function / procedure workload
- recursive CTE workload

### Legacy scripts

The `pstress/` subtree still contains old upstream scripts and MySQL option
files. They are not the source of truth for the PostgreSQL fork.

For PostgreSQL usage, prefer:

- the binary in `build/src`
- `--help --verbose`
- this README

## Useful PostgreSQL Options

Frequently useful options in the current fork:

- `--tables`
- `--columns`
- `--records`
- `--exact-initial-records`
- `--threads`
- `--seconds`
- `--logdir`
- `--prepare`
- `--step`
- `--metadata-path`
- `--no-partition-tables`
- `--only-partition-tables`
- `--no-temp-tables`
- `--only-temp-tables`
- `--no-generated-columns`
- `--trx-prob-k`
- `--trx-size`
- `--commit-prob`
- `--savepoint-prob-k`
- `--trx-ddl-prob-k`
- `--trx-ddl-size`
- `--select-with-join`
- `--select-with-cte`
- `--partition-types`
- `--add-drop-partition`

See the current live help for exact defaults:

```bash
./build/src/pstress-pg --help --verbose
```

## Example Workloads

### High-hit DML-focused run

```bash
./build/src/pstress-pg \
  --address=127.0.0.1 \
  --port=5432 \
  --user=your_user \
  --database=postgres \
  --tables=40 \
  --columns=20 \
  --records=500 \
  --threads=16 \
  --seconds=180 \
  --no-ddl \
  --logdir=./log
```

### Join and CTE-heavy run

```bash
./build/src/pstress-pg \
  --address=127.0.0.1 \
  --port=5432 \
  --user=your_user \
  --database=postgres \
  --tables=40 \
  --columns=15 \
  --records=300 \
  --threads=16 \
  --seconds=180 \
  --select-with-join=500 \
  --select-with-cte=500 \
  --logdir=./log
```

### Transactional DDL-focused run

```bash
./build/src/pstress-pg \
  --address=127.0.0.1 \
  --port=5432 \
  --user=your_user \
  --database=postgres \
  --tables=30 \
  --columns=15 \
  --records=200 \
  --threads=2 \
  --seconds=180 \
  --no-ddl \
  --trx-prob-k=0 \
  --trx-ddl-prob-k=200 \
  --trx-ddl-size=3 \
  --logdir=./log
```

## Logs and Debugging

Typical artifacts in `--logdir`:

- `default.node.tld_ddl_step_<n>.log`
- `default.node.tld_step_<n>_thread-<id>.sql`
- `step_<n>.dll`

Useful things to inspect:

- final `NODE SUMMARY`
- first structural error in a thread log
- whether a failure is a workload-level expected conflict or a true tool bug

The detailed PostgreSQL adaptation history and validation notes are tracked in
[pg_fix_validation_log.md](/Users/gongliangbiao/Desktop/Codes/pstress/pg_fix_validation_log.md).

## Summary of the Adaptation

If you only want the short version, this PostgreSQL fork has:

- **adapted** the random workload generator to PostgreSQL semantics
- **removed** a large set of MySQL-only workload features and dead code
- **added** PostgreSQL type coverage, transactional DDL, CTE/JOIN workload, step
  metadata replay, generated-column safety controls, and better DML hit rate

For PostgreSQL users, this repository should now be treated as a PostgreSQL
random workload tool first, not as a generic upstream README with incidental
PostgreSQL support.
