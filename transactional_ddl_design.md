# Transactional DDL Design

## Background

The current PostgreSQL version already has a random transaction mechanism, but
it explicitly prevents DDL from entering those transactions.

This was a deliberate safety choice:

- PostgreSQL DDL is transactional
- the tool updates its in-memory schema model immediately after successful DDL
- if such DDL later gets rolled back, the in-memory model and database state
  diverge

So the new requirement is not “just add one more probability flag”.

It is a new execution model:

- run DDL inside transactions
- keep transaction size controllable
- minimize transaction aborts
- support create/drop table and regular DDL
- avoid schema-model drift

This document discusses the current state and settles the design direction.

## Current transaction parameters

The current transaction-related options are:

- `--trx-prob-k`
- `--trx-size`
- `--commit-prob`
- `--savepoint-prob-k`

### `--trx-prob-k`

Meaning:

- probability, out of `1000`, of starting a random transaction

Current behavior:

- if chosen, the thread runs `START TRANSACTION`
- the following random SQLs are grouped into one general-purpose transaction

### `--trx-size`

Meaning:

- average / random upper-bound size of the current random transaction

Current behavior:

- once the transaction starts, the tool picks a transaction length
- statements are consumed until the counter reaches zero

### `--commit-prob`

Meaning:

- probability that a random transaction ends with `COMMIT`
- otherwise the tool executes `ROLLBACK`

Current behavior:

- rollback is an intentional workload feature
- this is useful for generic DML transaction stress
- this is directly at odds with the new goal of “尽可能保证事务成功”

### `--savepoint-prob-k`

Meaning:

- probability of using `SAVEPOINT` inside a random transaction
- there is also a built-in chance of `ROLLBACK TO SAVEPOINT`

Current behavior:

- this is designed to explore partial rollback behavior
- it again conflicts with the new goal of keeping transactional DDL stable and
  highly successful

## Current DDL-in-transaction behavior

The key current logic is:

- when a random transaction is open
- and the next picked statement is marked as DDL
- the tool immediately commits the open transaction first
- then runs the DDL outside the transaction

This means:

- current `trx` options do not support transactional DDL
- current design actively forbids it

## Why existing `trx` parameters are not sufficient

The current `trx` parameters are built for **general random SQL transactions**,
not for **high-success transactional DDL blocks**.

Reusing them directly would create several problems.

### Problem 1: `commit-prob` intentionally injects rollback

Transactional DDL wants:

- high success
- committed end state

Current `commit-prob` wants:

- random commit / rollback mix

That is not compatible as a default.

### Problem 2: `savepoint-prob-k` intentionally injects partial rollback

Transactional DDL wants:

- predictable transactional scope
- stable metadata application

Current savepoint logic wants:

- random nested rollback behavior

This is unsafe for DDL unless we also add staged schema metadata handling.

### Problem 3: current random transaction path mixes unrelated statement classes

The current transaction scheduler is a generic wrapper around the random option
picker.

That means if DDL is simply allowed into it, the transaction can become:

- mixed DML + DDL
- mixed low-risk + high-risk DDL
- mixed table-level operations across unrelated objects

That is the opposite of “尽可能保证事务成功”.

### Problem 4: current in-memory metadata model assumes DDL success is final

Today, after a DDL statement succeeds, many table methods immediately mutate
the in-memory table/index/column structures.

This is acceptable only because:

- the statement is currently executed outside a user-managed transaction
- there is no later rollback of that same statement

Once DDL enters a transaction, this assumption breaks.

## Current random DDL surface

The current DDL-flagged workload operations include:

- `modify-column`
- `drop-column`
- `add-column`
- `drop-index`
- `add-index`
- `rename-index`
- `rename-column`
- `analyze`
- `add-drop-partition`
- `optimize`
- `truncate`
- `recreate-table`

These are not equally suitable for transactional DDL.

## Design goals

The new transactional DDL mechanism should satisfy these goals:

1. Be separate from the current general random transaction path
2. Keep transaction size explicitly controllable
3. Prefer successful commit over random rollback behavior
4. Avoid transaction-abort cascades
5. Support:
   - create table
   - drop table
   - regular DDL
6. Avoid in-memory schema drift on rollback or failed commit
7. Minimize overlap and semantic conflict with current `trx` parameters

## Candidate approaches

### Approach A: allow existing random DDL into current `trx` flow

Idea:

- remove the current “DDL must be outside random transaction” guard
- let `trx-prob-k`, `trx-size`, `commit-prob`, and `savepoint-prob-k` govern
  DDL too

Pros:

- smallest implementation surface

Cons:

- directly conflicts with the new success goal
- rollback and savepoint semantics are wrong for schema metadata
- mixed DML/DDL transactions become noisy and fragile
- high risk of metadata divergence

Assessment:

- not recommended

### Approach B: separate transactional DDL executor with metadata snapshot / restore

Idea:

- keep the current `trx` mechanism unchanged
- add a new, independent transactional DDL path
- when selected, it:
  - captures a schema snapshot
  - opens a dedicated DDL transaction
  - executes a curated DDL sequence
  - commits on success
  - on failure or commit error, rolls back and restores the snapshot in memory

Pros:

- clean semantic separation
- controllable transaction size
- easier to guarantee success
- explicit place to enforce safe DDL selection

Cons:

- more implementation work
- requires schema snapshot / restore support

Assessment:

- recommended

### Approach C: separate transactional DDL executor, but only on scratch tables

Idea:

- same as Approach B
- but transactional DDL is restricted to temporary or scratch tables created by
  the feature itself

Pros:

- safest
- highest chance of success
- lowest interference with ongoing workload

Cons:

- coverage is narrower
- weaker stress for existing shared tables

Assessment:

- good conservative variant
- too narrow as the main long-term mode

## Settled direction

**Choose Approach B.**

That means:

- do not overload existing `trx` semantics
- add a new dedicated transactional DDL mechanism
- add schema snapshot / restore to keep metadata correct
- use a curated high-success DDL subset

## Recommended parameter design

Although the request starts as “新增一个参数”, one knob is not enough if
transactional DDL size must also be controllable.

The recommended minimal parameter set is:

- `--trx-ddl-prob-k`
- `--trx-ddl-size`

### `--trx-ddl-prob-k`

Meaning:

- probability, out of `1000`, of starting a dedicated transactional DDL block

Recommended default:

- `0`

Reason:

- current behavior should remain unchanged unless explicitly enabled

### `--trx-ddl-size`

Meaning:

- number of DDL statements planned inside one transactional DDL block

Recommended default:

- `3`

Recommended semantics:

- bounded random size in `1..N`, where `N` is `trx-ddl-size`

This keeps transaction DDL small and controllable.

## Why not reuse `trx-size`

It may look tempting to reuse `trx-size`, but that would create semantic
overlap.

`trx-size` currently means:

- size of a generic random SQL transaction

The new feature needs:

- size of a curated DDL-only transaction block

Those two should remain independent.

## Recommended execution model

### 1. Separate scheduler path

Transactional DDL should be scheduled only when:

- the thread is not already inside a normal random transaction
- the feature is enabled via `--trx-ddl-prob-k`
- the picked opportunity wins its own probability check

This keeps it independent from the existing `trx` machinery.

### 2. DDL-only block

The new mechanism should generate:

- DDL-only transactions

Not:

- mixed DML + DDL transactions

Reason:

- much easier to control success
- simpler rollback semantics
- less ambiguity with current random transaction parameters

### 3. Single transaction block lifecycle

Recommended lifecycle:

1. choose a transactional DDL plan
2. take an in-memory schema snapshot
3. `START TRANSACTION`
4. execute each DDL in sequence
5. if any DDL fails:
   - `ROLLBACK`
   - restore in-memory snapshot
   - stop the block immediately
6. if all DDL succeed:
   - `COMMIT`
   - if commit succeeds, keep in-memory changes
   - if commit fails, restore in-memory snapshot

This model is the core of the design.

## Metadata strategy

This is the hardest and most important part.

### Requirement

The tool must not keep in-memory schema changes that were later rolled back by
PostgreSQL.

### Recommended solution

Use **schema snapshot / restore** around each transactional DDL block.

Recommended implementation direction:

- serialize the current schema metadata into an in-memory string before
  starting the transactional DDL block
- let existing DDL helpers mutate the live in-memory model during execution
- if the block rolls back or commit fails:
  - discard the mutated in-memory model
  - rebuild it from the snapshot

Why this is preferable to per-DDL deferred mutators:

- current code already has serialization logic
- many DDL helpers are already written and mutate in-memory state inline
- snapshot / restore is simpler than refactoring every DDL helper into a
  two-phase “prepare SQL then apply patch on commit” model

## DDL subset for transactional DDL

To keep success rate high, the transactional DDL executor should not use the
entire existing DDL pool.

### Recommended allowed subset in first implementation

- `add-column`
- `drop-column`
- `rename-column`
- `add-index`
- `drop-index`
- `rename-index`
- `create table`
- `drop table`

### Recommended optional second-wave additions

- `modify-column`
- `truncate`
- `recreate-table`

### Recommended exclusions in first implementation

- `add-drop-partition`
- `optimize`
- `analyze`

Reasons:

- `add-drop-partition` is multi-step and higher-risk
- `optimize` / `analyze` do not add much transactional DDL value
- `modify-column` is often more failure-prone due to type/cast constraints
- `truncate` and `recreate-table` are more disruptive to surrounding workload

## Create/drop table semantics

The requirement explicitly asks for create/drop table support.

Recommended first implementation:

- transactional DDL blocks may create and drop tables
- but these create/drop statements should prefer **dedicated transactional
  scratch tables**

This gives a better success profile because:

- lower lock conflict
- less interference with existing workload tables
- simpler reasoning about rollback

Regular DDL against existing tables should still be allowed, but create/drop
table should preferentially target dedicated transaction-DDL work tables first.

## Success-first behavior

To minimize transaction aborts, the new transactional DDL executor should use
these rules:

- no random rollback at block end
- no random savepoints in the block
- stop immediately on first failed DDL
- use a curated DDL subset
- keep transaction size small
- prefer a single target table family inside one block

In other words:

- **transactional DDL should always try to commit**
- rollback should be an error recovery path, not a workload feature

## Interaction with existing `trx` parameters

### `trx-prob-k`

Relationship:

- no direct conflict if scheduling is separated

Rule:

- normal `trx` and `trx-ddl` should be mutually exclusive per event opportunity

Recommended behavior:

- if a transactional DDL block starts, normal `trx` does not start for that
  iteration

### `trx-size`

Relationship:

- semantic overlap if reused

Rule:

- do not reuse
- use `trx-ddl-size` instead

### `commit-prob`

Relationship:

- directly conflicts with “尽可能保证事务成功”

Rule:

- transactional DDL ignores `commit-prob`
- transactional DDL always attempts `COMMIT`

### `savepoint-prob-k`

Relationship:

- directly conflicts with metadata simplicity and rollback safety

Rule:

- transactional DDL ignores `savepoint-prob-k`
- no savepoints in the first implementation

## Interaction with existing DDL control flags

### `--no-ddl`

Relationship:

- partial overlap

Rule:

- first implementation may treat `--no-ddl` as “disable ordinary random DDL”
- if `--trx-ddl-prob-k > 0`, transactional DDL may still run under `--no-ddl`

Reason:

- the new executor is intentionally separate from the legacy random DDL path
- this makes it possible to validate or use transactional DDL in isolation

### `--only-cl-ddl`

Relationship:

- likely conflict

Rule:

- first implementation should disable random transactional DDL when
  `--only-cl-ddl` is active

Reason:

- `only-cl-ddl` semantically means “run only explicitly provided command-line
  DDL”, not “run new random internal DDL blocks”

### `--only-cl-sql`

Rule:

- transactional DDL should also be disabled under `--only-cl-sql`

## Open design choices

These are the only remaining choices worth explicitly calling out.

### Choice 1: scratch-table preference vs. shared-table preference

Recommendation:

- prefer scratch tables for create/drop
- allow safe ALTER/index DDL on existing tables

### Choice 2: whether `modify-column` belongs in first version

Recommendation:

- no

Reason:

- it has a higher failure rate
- it weakens the “尽可能成功” target

### Choice 3: whether savepoints should be supported later

Recommendation:

- no in first version
- only reconsider after snapshot / restore and block scheduler are proven stable

## Final recommendation

The correct design is:

- a **new, separate transactional DDL executor**
- controlled by:
  - `--trx-ddl-prob-k`
  - `--trx-ddl-size`
- using:
  - DDL-only transactions
  - a curated high-success DDL subset
  - metadata snapshot / restore
  - always-commit semantics
  - no savepoints
  - no reuse of `commit-prob` or `trx-size`

In short:

- keep existing random transactions for generic SQL stress
- add a separate success-first transaction DDL mechanism for transactional DDL
  coverage
- do not mix the two semantics

## Implementation status

The first implementation is now landed, with a few conservative adjustments
made during validation to keep the success rate high.

### Implemented parameters

- `--trx-ddl-prob-k`
- `--trx-ddl-size`

### Implemented execution model

- separate DDL-only transactional executor
- always attempts `COMMIT`
- does not reuse `trx-size`, `commit-prob`, or `savepoint-prob-k`
- no savepoints
- transactional DDL blocks run outside the legacy generic transaction path

### Metadata safety model

- transactional DDL blocks are serialized with ordinary workload DDL
- scratch-table metadata is snapshotted and restored on rollback
- existing-table metadata is snapshotted per target table and restored on
  rollback
- ordinary DML selection paths were hardened so tables with temporarily narrow
  predicate coverage do not spin forever
- PostgreSQL `DROP COLUMN` handling now drops any dependent in-memory index
  metadata entirely, matching PostgreSQL behavior

### First-version DDL subset

On scratch tables:

- `CREATE TABLE`
- `DROP TABLE`
- safe `ADD COLUMN`
- `DROP COLUMN`
- `ADD INDEX`
- `DROP INDEX`
- `RENAME COLUMN`
- `RENAME INDEX`

On existing shared tables:

- safe `ADD COLUMN`
- `ADD INDEX`
- `DROP INDEX`
- `RENAME COLUMN`
- `RENAME INDEX`

Excluded in the first version:

- `modify-column`
- `truncate`
- `recreate-table`
- partition DDL
- shared-table `DROP COLUMN`

### Additional guardrails added during implementation

- transactional `ADD COLUMN` uses a conservative PostgreSQL-safe scalar subset
  instead of the full random `AddColumn()` type surface
- shared-table transactional `ADD COLUMN` is disabled once a table reaches
  `256` columns, to avoid low-value aborts from column-count pressure
- `ColumnRename()` now skips rename attempts that would collide with an
  existing column name

### Validation outcome

Validated locally against PostgreSQL with isolated transactional-DDL runs using
`--no-ddl --trx-ddl-prob-k=200 --trx-ddl-size=3`.

Observed stable behavior:

- no `25P02`
- no thread failure
- no scratch index-name drift after rollback / dependent column drop
- no repeated column-rename collisions
- 3-minute isolated validation completed successfully with only expected
  workload error classes:
  `23505`, `23503`, `40P01`
