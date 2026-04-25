# Review Adoption Checklist

Source review/design note:
[synthetic-drifting-lemur.md](/Users/gongliangbiao/.claude/plans/synthetic-drifting-lemur.md)

## Positioning

This source file is closer to a feature roadmap draft than a strict code-review
comment list.

That means each item should be treated as one of:

- adopt as-is
- adopt but redesign for current codebase
- defer
- reject

This checklist records that decision against the current PostgreSQL-oriented
`pstress-pg` codebase.

## Summary

### Adopt first

- `JOIN` query testing
- non-recursive `CTE` testing
- prepared statement testing
- read-only execution plan sampling

### Adopt later

- view testing
- config system enhancement
- trigger testing
- function / procedure testing

### Do not implement as written

- “add config file support from scratch”
- SQL-schema-style prepared statement naming (`pstress.stmt_x`)
- generic `EXPLAIN ANALYZE` wrapping inside `execute_sql()` for all SQL
- trigger examples that always `RETURN NEW`

## Detailed checklist

| Item | Decision | Why | Estimated effort |
| --- | --- | --- | --- |
| JOIN query testing | Adopt | High coverage value, fits current random query model | 3-5 days |
| Non-recursive CTE testing | Adopt | Good query-shape expansion with manageable risk | 2-3 days |
| Recursive CTE testing | Defer | Higher failure risk, lower immediate value than plain CTE | +1-2 days after non-recursive CTE |
| Prepared statement testing | Adopt with redesign | Valuable, but should use libpq prepared APIs rather than the SQL sketch in the doc | 2-4 days |
| Read-only execution plan sampling | Adopt with redesign | Useful observability feature, but only safe for read-only SQL in first version | 1-2 days |
| EXPLAIN ANALYZE on generic SQL | Reject as written | Would execute mutating SQL twice and distort workload semantics | N/A |
| View testing | Adopt later | Reasonable PG coverage, but lower priority than JOIN/CTE/prepared | 2-4 days |
| Trigger testing | Defer | High metadata and side-effect complexity | 5-8 days |
| Function / procedure testing | Defer | Requires new object lifecycle, metadata, cleanup, and dependency handling | 5-10 days |
| YAML config support | Defer / redesign | Current code already has INI config support, so this is not greenfield work | 2-4 days if YAML is still desired |
| INI config enhancement | Adopt later | Extends what already exists with lower risk than switching formats | 0.5-1.5 days |
| Reusable skills docs | Optional | Helpful for workflow reuse, but not a core repo priority | 1-2 days outside main feature work |

## Worth adopting now

### 1. JOIN query testing

Decision:

- adopt

Why:

- expands read-query coverage significantly
- naturally complements current table/column randomization
- can likely reuse existing table metadata without introducing new object types

Current-doc gaps:

- join compatibility rules are oversimplified
- current code does not already expose helpers like
  `find_column_by_type()` or `build_where_clause()`
- implementation needs to fit existing `pick_some_option()` and
  `run_some_query()` dispatch structure

Suggested scope:

- start with `INNER JOIN`
- optionally add `LEFT JOIN`
- skip `RIGHT JOIN` and `CROSS JOIN` in first version
- support only type-safe scalar join predicates at first

Estimated effort:

- 3-5 days

### 2. Non-recursive CTE testing

Decision:

- adopt

Why:

- good SQL-shape diversity
- easier to stabilize than recursive CTE
- can be built as a read-only query path without new metadata classes

Current-doc gaps:

- the draft assumes helpers that do not exist
- recursive example is too generic and not aligned with current table-driven
  generation model

Suggested scope:

- start with 1-2 simple `WITH` clauses
- read-only only
- avoid recursive CTE in first pass

Estimated effort:

- 2-3 days

### 3. Prepared statement testing

Decision:

- adopt, but redesign

Why:

- good PG client behavior coverage
- better aligned with libpq-level testing than SQL `PREPARE` as a schema-like
  object

Current-doc issues:

- `pstress.stmt_x` style naming is not the right model
- metadata model in the draft is too light for real parameter typing and reuse
- direct SQL `PREPARE`/`EXECUTE` is possible, but libpq-native
  `PQprepare/PQexecPrepared` is the more natural fit here

Suggested scope:

- session-local prepared statements only
- start with simple `SELECT` and `UPDATE/DELETE ... WHERE`
- do not persist prepared-statement metadata to the existing schema metadata file

Estimated effort:

- 2-4 days

### 4. Read-only execution plan sampling

Decision:

- adopt, but redesign

Why:

- strong diagnostic value
- relatively contained if limited to read-only SQL

Current-doc issues:

- wrapping all SQL in `EXPLAIN` or `EXPLAIN ANALYZE` inside `execute_sql()`
  is unsafe
- `EXPLAIN ANALYZE` on mutating SQL changes semantics and adds real execution
  overhead

Suggested scope:

- sample only `SELECT`
- start with `EXPLAIN`, not `EXPLAIN ANALYZE`
- write plans to a dedicated log

Estimated effort:

- 1-2 days

## Worth doing later

### 5. View testing

Decision:

- adopt later

Why:

- adds object coverage without the full complexity of triggers/functions
- still requires new metadata lifecycle and cleanup paths

Risks:

- dependent object cleanup
- stale metadata if base tables are dropped/recreated/renamed

Estimated effort:

- 2-4 days

### 6. Config system enhancement

Decision:

- adopt later, but not as written

Why:

- current code already supports `--config-file` with INI parsing
- the draft is wrong to treat config support as missing

Current state:

- config file option exists in [common.hpp](/Users/gongliangbiao/Desktop/Codes/pstress/src/common.hpp)
- INI load path exists in [pstress.cpp](/Users/gongliangbiao/Desktop/Codes/pstress/src/pstress.cpp)

Suggested direction:

- first improve INI usability and option coverage
- only add YAML if there is a concrete operational need

Estimated effort:

- INI enhancement: 0.5-1.5 days
- YAML support: 2-4 days

### 7. Trigger testing

Decision:

- defer

Why:

- introduces side effects into existing DML paths
- increases schema dependency and cleanup complexity
- likely to amplify random failures if introduced too early

Current-doc issues:

- example always returns `NEW`, which is not universally valid
- trigger body generation is much more constrained in PostgreSQL than the draft
  suggests

Estimated effort:

- 5-8 days

### 8. Function / procedure testing

Decision:

- defer

Why:

- requires new metadata objects
- needs lifecycle management, dependency handling, and cleanup rules
- higher implementation cost than JOIN/CTE/prepared/view

Current-doc issues:

- function body examples are placeholders, not current-code-compatible design
- object model is underspecified for parameter typing and dependency tracking

Estimated effort:

- 5-10 days

## Not recommended as written

### 9. “Add config file support” as a new feature

Decision:

- reject as written

Reason:

- this is already present in the codebase
- the real task would be to extend or replace existing INI-based behavior

### 10. Global SQL `EXPLAIN ANALYZE` wrapping in `execute_sql()`

Decision:

- reject as written

Reason:

- mutating SQL would execute twice
- workload timing and semantics would be distorted
- logging would become harder to interpret

### 11. Schema-style prepared statement naming

Decision:

- reject as written

Reason:

- prepared statements are session-scoped execution artifacts
- they should not be modeled like `pstress` schema objects

### 12. Trigger examples that always `RETURN NEW`

Decision:

- reject as written

Reason:

- not valid as a general PostgreSQL trigger template
- would need event-aware generation

## Recommended roadmap

### Phase A

- JOIN query testing
- non-recursive CTE testing

### Phase B

- prepared statement testing
- read-only execution plan sampling

### Phase C

- view testing
- config enhancement

### Phase D

- trigger testing
- function / procedure testing

## Practical note

The effort estimates in the source review/design note are generally too
optimistic for the current codebase.

Main reasons:

- current logic is heavily centered in `random_test.cpp`
- many new features need dispatch wiring, logging, metadata consistency, and
  PostgreSQL-specific behavior checks
- validation cost is non-trivial because each substantial feature should be
  followed by build + local stress verification
