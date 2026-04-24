# Generated Column Optimization Plan

## Background

The historical MySQL-side issue is still relevant to the current PostgreSQL
version in one important dimension:

- when a table has a very large column count, the tool can generate too many
  generated columns
- each generated column can depend on too many source columns
- the final `CREATE TABLE` statement becomes unnecessarily wide and complex
- once one generated-column expression is too large or too expensive, the whole
  table creation fails

This round does **not** change code. The goal is to document the current
behavior, identify its strengths and weaknesses, and settle on an optimization
plan.

## Current implementation

The current PostgreSQL generated-column logic lives mainly in:

- `src/random_test.cpp`
- `src/random_test.hpp`
- `src/help.cpp`

### 1. How generated columns are enabled

There is only one current control:

- `--no-generated-columns`

This is a hard on/off switch. There is no finer control for:

- generated-column count per table
- source-column count per generated column
- expression width / complexity
- behavior differences for narrow vs. very wide tables

### 2. How generated columns are chosen during initial table creation

In `Table::CreateDefaultColumn()` the logic is:

- the first column may become the primary key
- for the remaining columns:
  - only columns in the last `20%` of the table are eligible to become
    generated columns
  - each eligible position has a `50%` chance to become `GENERATED`

That means the current strategy is intentionally concentrated near the tail of
the schema.

For a table with `300` columns:

- the last `60` positions are eligible
- about `30` generated columns are expected on average

This is already a large number for one table.

### 3. How generated-column result types are chosen

The current constructor for `Generated_Column` randomly chooses a generated
result type from a small set:

- `INT`
- `BIGINT`
- `NUMERIC`
- `VARCHAR`
- `CHAR`
- `BLOB` when blob columns are enabled

This is a good property: generated results are kept in a relatively stable and
small family instead of exploding into every PostgreSQL type.

### 4. How source columns are chosen

For each generated column, the current code computes:

- `columns = rand_int(.6 * table->columns_->size()) + 1`

Because `rand_int()` is inclusive, this means the source-column count is chosen
from:

- `1 .. floor(0.6 * current_column_count) + 1`

For wide tables, this is the main risk.

For example, when the table already has about `250` real columns:

- the upper bound is about `151`
- the average dependency count is roughly `76`

When the table is closer to `300` columns:

- the upper bound is about `181`
- the average dependency count is roughly `91`

So with `300` columns, the tool can easily produce:

- around `30` generated columns
- each generated column depending on dozens of source columns

This is much more complex than necessary for stress coverage.

### 5. Current safeguards

The current implementation does have some useful safeguards:

- generated columns cannot depend on other generated columns
- this keeps dependency depth at `1`
- many PostgreSQL-specific complex families are excluded from source selection:
  - geometry
  - arrays
  - ranges
  - `TIMETZ`
  - `TIMESTAMPTZ`
  - `MONEY`
  - `XML`
  - `TSVECTOR`
  - `TSQUERY`
- all generated columns are created as `STORED`

These are meaningful positives. The current system does avoid recursive chains
and avoids some obviously difficult source families.

### 6. Current weaknesses

The main weaknesses are structural rather than syntactic.

#### 6.1 Generated-column count scales too aggressively on wide tables

The current tail-window rule means generated columns grow roughly with total
column count.

At `300` columns, expected generated-column count is already high enough to
make schema complexity jump sharply.

#### 6.2 Per-column dependency count scales linearly with table width

This is the biggest issue.

Even one generated column depending on `70+` source columns is already much
heavier than necessary. Doing that repeatedly in the same table is exactly the
pattern that makes large-table DDL fragile.

#### 6.3 Source positions are not deduplicated

The current selection pushes source positions into a vector without dedup.

That means one source column may be repeated several times in the same
generated expression.

This has no real coverage value, but it does:

- increase expression length
- increase parse cost
- make generated expressions noisier

#### 6.4 Complexity is controlled indirectly, not explicitly

Today there is no explicit budget for:

- maximum generated columns per table
- maximum source columns per generated column
- maximum generated expression width
- maximum total generated complexity in one table

Without explicit budgets, wide-table cases are left to probability alone.

#### 6.5 Wide-table failures are all-or-nothing

If one generated-column definition makes `CREATE TABLE` fail, the whole table
creation fails.

That is a poor tradeoff:

- we lose the whole table
- but the goal was only to add some generated-column coverage

Generated columns should be treated as optional stress features, not as a
single point of failure for base-table creation.

#### 6.6 Current strategy clusters generated columns at the end

This is simple, but not ideal.

Clustering generated columns in the final `20%` of positions causes “bursts” of
schema complexity instead of a smoother distribution.

It also means that very wide tables tend to get many generated columns all in
the same region.

#### 6.7 There is no “conservative mode”

Right now there are only two real choices:

- use generated columns
- disable generated columns entirely

That is too coarse for production-style stress testing.

## Advantages of the current approach

To avoid overcorrecting, it is worth stating what the current design gets
right.

### Advantage 1: implementation is simple

The current logic is easy to understand:

- pick some later columns
- build generated expressions from prior columns

This simplicity makes it easy to maintain.

### Advantage 2: dependency depth is bounded

Generated columns are not allowed to depend on generated columns.

This is one of the most important current safety properties and should be kept.

### Advantage 3: expression families are PostgreSQL-aware

The current code already contains PostgreSQL-specific source conversion logic
for numeric and text generated expressions.

That means the tool is not blindly ported from MySQL semantics.

### Advantage 4: coverage is broad

The current approach exercises:

- numeric expression generation
- text concatenation paths
- casts from multiple source types
- DDL with stored generated columns

That coverage is useful and should not be lost.

## Disadvantages of the current approach

The costs now outweigh the benefits for wide tables.

### Disadvantage 1: generated-column count is too high for wide tables

For very wide schemas, the generated-column count is not “a little high”; it is
structurally high.

### Disadvantage 2: expression breadth is much too large

Dozens of source columns per generated column creates complexity without adding
proportional test value.

### Disadvantage 3: expression quality degrades with width

As tables get wider, generated expressions become:

- longer
- more repetitive
- harder to reason about
- more likely to hit engine-specific DDL limits or planner/parser edge cases

### Disadvantage 4: no graceful degradation

A generated-column feature should degrade to:

- fewer generated columns
- simpler expressions
- fallback to plain columns

The current logic does not do that.

## Candidate optimization approaches

### Option A: disable generated columns automatically on wide tables

Idea:

- if column count exceeds a threshold, stop generating generated columns

Pros:

- simplest and safest
- immediately removes the failure class

Cons:

- loses generated-column coverage exactly where wide-schema stress is valuable
- too blunt

Assessment:

- acceptable as an emergency stopgap
- not recommended as the long-term design

### Option B: keep generated columns, but add hard caps

Idea:

- keep current behavior
- add fixed caps such as:
  - max generated columns per table
  - max source columns per generated column

Pros:

- simple
- low implementation risk
- addresses the biggest failure driver

Cons:

- still somewhat static
- does not solve clustering or lack of graceful fallback by itself

Assessment:

- much better than current behavior
- still incomplete

### Option C: adaptive budgeted generation with fallback

Idea:

- generated columns remain enabled
- but generation is controlled by explicit per-table and per-column budgets
- when the budget is exhausted, the tool falls back to plain columns or skips
  generated-column creation

Pros:

- preserves coverage
- scales well from narrow to wide tables
- handles large tables gracefully
- avoids all-or-nothing failure

Cons:

- slightly more implementation work
- introduces more policy knobs

Assessment:

- best long-term solution

## Recommended plan

**Choose Option C: adaptive budgeted generation with fallback.**

This should be the settled direction.

The goal is not to remove generated columns. The goal is to keep them useful
while making them proportionate to table width.

## Proposed optimization design

### 1. Introduce a per-table generated-column budget

Instead of “last 20% of columns with 50% chance”, generated-column creation
should be limited by an explicit budget.

Recommended policy:

- generated columns are allowed only after a minimum number of eligible base
  columns already exist
- generated-column count per table is controlled by:
  - a ratio budget
  - an absolute hard cap

Recommended default:

- ratio target: about `2%` to `5%` of table columns
- hard cap: `8`

Examples:

- `50` columns -> `1` to `2` generated columns
- `100` columns -> `2` to `5` generated columns
- `300` columns -> still capped at about `6` to `8`, not `30`

This is enough to preserve coverage without letting generated columns dominate
the schema.

### 2. Introduce a per-generated-column dependency budget

The number of source columns should no longer scale as `60%` of table width.

Recommended default:

- source count target: `1` to `4`
- hard cap: `8`

This is the most important optimization.

Rationale:

- `1` to `4` source columns is enough to exercise expression logic
- `5` to `8` can still be used occasionally for diversity
- `70+` source columns does not add useful coverage proportional to the risk

### 3. Deduplicate source columns

Source positions should be unique inside one generated expression.

This is a straightforward quality improvement:

- shorter expressions
- less redundant work
- same or better coverage

This change should be mandatory in the implementation.

### 4. Prefer low-cost source columns

Source selection should be biased toward “cheap” source columns.

Preferred source families:

- integer-like numeric
- `NUMERIC`
- `BOOL`
- `VARCHAR`
- `CHAR`
- `DATE`
- `TIME`
- `TIMESTAMP`

De-prioritize or exclude for generated-column sources in conservative mode:

- `BLOB`
- `BYTEA`
- `JSON`
- `JSONB`
- `BIT`
- `VARBIT`
- types that require expensive serialization or wide text expansion

Reason:

- these types are still useful in general table coverage
- but they are poor defaults for large generated expressions

### 5. Introduce an expression-size budget

Generated-column construction should stop growing once the expression reaches a
budget.

Recommended budget dimensions:

- maximum source-column count
- maximum projected output width for text generated columns
- optional maximum SQL expression length

Recommended default:

- text generated output width target: `32` to `128`
- hard stop when the generated expression would exceed the configured budget

This keeps generated DDL compact and predictable.

### 6. Replace tail clustering with distributed placement

Generated columns should not be concentrated only in the final `20%` of the
table.

Better rule:

- once enough base columns exist, each new column position may become generated
  only if the table budget still allows it

This gives:

- smoother distribution
- fewer late bursts
- better behavior for wide tables

### 7. Add fallback behavior

Generated-column creation should not be allowed to sink the whole table.

Recommended fallback order:

1. try generated column under current budget
2. if candidate violates local complexity policy, regenerate a simpler
   generated expression
3. if still not viable after bounded retries, fall back to a plain scalar
   column

This is especially important during initial `CREATE TABLE`.

The stress tool should prefer:

- “table created with slightly less generated coverage”

over:

- “table creation failed entirely”

### 8. Add more granular user controls

The current `--no-generated-columns` flag should stay, but it is not enough.

Recommended additional controls:

- `--generated-columns-mode=off|conservative|balanced|aggressive`
- `--generated-columns-max-per-table=<n>`
- `--generated-columns-max-deps=<n>`
- `--generated-columns-max-width=<n>`

Recommended default mode:

- `balanced`

Recommended semantics:

- `off`: disable generated columns entirely
- `conservative`: very small counts, cheap source types only
- `balanced`: default production-like behavior
- `aggressive`: broader coverage, but still under hard caps

## Proposed implementation priorities

When this plan is implemented, the work should be done in this order:

### Priority 1

Control risk immediately:

- add max generated columns per table
- add max source columns per generated column
- deduplicate source columns

This alone should address the current “300 columns causes table creation
failure” class most effectively.

### Priority 2

Improve quality:

- bias toward cheap source columns
- add expression-size budget
- remove late-tail clustering

### Priority 3

Improve operability:

- add mode-based CLI controls
- add better logging or counters for generated-column usage

## Final recommendation

The current generated-column implementation is **functionally correct in idea**
but **too aggressive in scale** for very wide tables.

The key problem is not that generated columns exist. The key problem is that
both of these dimensions scale too far:

- generated-column count per table
- dependency count per generated column

The settled optimization strategy should therefore be:

- keep generated-column coverage
- bound it with explicit budgets
- distribute it more evenly
- make source selection cheaper
- add fallback so generated-column complexity can never sink the whole table

In short:

**Do not disable generated columns by default.**

**Do make generated columns budgeted, deduplicated, source-limited, and
fallback-safe.**

## Implementation status

2026-04-24 first implementation round completed:

- implemented:
  - per-table generated-column adaptive budget with hard cap
  - per-generated-column dependency budget with hard cap
  - deduplicated source-column selection
  - reduced source-family set for generated expressions
  - text generated-expression width budget
  - fallback to plain scalar/text columns when generated-column creation cannot
    be completed inside policy
- validated:
  - `300`-column smoke completed successfully
  - `300`-column 3-minute validation completed successfully
- not yet implemented:
  - CLI knobs for generated-column mode / caps
  - richer cost-based source weighting beyond current exclusion-based filtering
  - explicit logging counters for “generated requested vs generated created vs
    fallback used”
