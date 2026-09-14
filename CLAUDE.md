# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Working with the user

**Before making any changes**, sync the repo:
```bash
git pull                                      # pull latest commits
git submodule update --init --recursive       # update submodules to pinned commits
```

The `--recursive` flag also initializes `.claude/skills/shared` (the shared Claude Code skills submodule from `github.com/ila/duckdb-claude-skills`). If that submodule fails to initialize on its own, run:
```bash
git submodule update --init .claude/skills/shared
```

When you're stuck — either unable to fix a bug after 2-3 attempts, or tempted to work around the actual problem by redefining the objective — **stop and ask the user for directions**. Explain clearly what the specific problem is (e.g., "AstToCteList produces wrong column names for a 3-way join — should I fix the AstJoinNode output ordering or adjust the CTE binding map?"). The user knows this codebase deeply and can often point you to the right solution in one sentence. Do not silently change the goal, declare something impossible, or add bloated workarounds without consulting first. We work as a team.

Always test your changes with real queries (e.g., create a table, run `PRAGMA lpts(...)` to check the SQL output, then `SET lpts_check = true` and run the query directly to verify round-trip correctness) before declaring success, not just unit tests.

**Run the full validation (`make test`, i.e. unittest + the DuckDB-corpus coverage gate) when a feature
is fully done or before pushing** — during iteration, use `make unittest` and targeted single-file runs
instead (the gate takes a few minutes).

**Only run the SQLStorm benchmark when a feature is fully done or before pushing.** Do not run it during iterative development — it takes several minutes even at the smallest scale factor. Run it like this:
```bash
build/release/extension/lpts/lpts_sqlstorm_benchmark --tpch_sf 0.001 --timeout 10
```
The benchmark runs all 17036 SQLStorm TPC-H queries with `lpts_check` enabled (each query re-run under `SET lpts_check = true` for round-trip verification) and prints a summary (success %, not-implemented breakdown, etc.). A regression — fewer `SUCCESS` results than the previous baseline — must be investigated before pushing.

Never execute git commands that could lose code. Always ask the user for permission on those.

## Development rules

- **New features must have tests.** Ask the user whether to create a new test file or extend an existing one in `test/sql/`.
- **Never remove a failing test to "fix" a failure.** If a test fails, fix the underlying bug. Tests exist for a reason.
- **Only change `src/` and `test/` files** unless explicitly told otherwise. Do not touch CMakeLists.txt, Makefile, vcpkg.json, or any other project infrastructure files.
- **Every test file MUST verify round-trip correctness**, not just that queries run without error. Set `SET lpts_check = true` once after `require lpts`, then run the query directly — LPTS transparently compares the original and the rewritten query and raises an error on a mismatch.
- **Before implementing anything, search the existing codebase** for similar patterns or solutions. Check `BuildNode()` in `src/lpts_ast_builder.cpp` (logical operator → AST) and `FlattenNode()` in `src/lpts_ast_flattener.cpp` (AST → CTE list) as the canonical references for operator-specific logic. Reuse before reinventing.
- **Use helper functions.** Factor shared logic into helpers. Check `src/lpts_helpers.cpp` and `src/include/lpts_helpers.hpp` for existing utilities (`VecToSeparatedList`, `EscapeSingleQuotes`, etc.).
- **Never edit the `duckdb/` submodule.** The DuckDB source is read-only. All LPTS logic lives in `src/` and `test/`.
- **Add `LPTS_DEBUG_PRINT` statements** at key processing points (entry into each operator case, before/after CTE node creation, at pipeline boundaries). Use the existing macro from `src/include/lpts_debug.hpp` — it is compiled out when `LPTS_DEBUG` is 0.
- **Follow the phase split.** Operator *extraction* (reading DuckDB's `LogicalOperator` internals) belongs in `src/lpts_ast_builder.cpp`; *serialization* (SQL text) belongs in `src/lpts_ast_flattener.cpp` and `src/cte_nodes.cpp`; expression rendering belongs in `src/lpts_expression_renderer.cpp`. Do not mix phases.
- **Refuse cleanly, never emit wrong SQL.** When a query cannot be translated faithfully, raise an error whose message starts with `LPTS_<CODE>:` (use `ThrowLptsNotImplemented` from `src/lpts_helpers.cpp`). The coverage gate treats any non-`LPTS_`-formatted failure as a bug.

## What is LPTS?

LPTS (**L**ogical **P**lan **T**o **S**QL) is a DuckDB extension that takes a SQL query, reads DuckDB's internal *logical plan* for that query, and converts it back into an equivalent SQL string made of CTEs (Common Table Expressions). Each CTE corresponds to one operator from the plan.

```
D PRAGMA lpts('SELECT name FROM users WHERE age > 25');
-- WITH
-- t0_scan (t0_name) AS (
--     SELECT  "name"
--     FROM    memory.main.users
--     WHERE   (age>25)
-- )
-- SELECT  t0_name AS "name"
-- FROM    t0_scan;
```

(CTE names are `t{N}_{operator}`; simple filter/projection chains are fused into the scan CTE by the
pipeline-merging pass, and pushed-down filters render as scan-level `WHERE`.)

## Build & Test

```bash
GEN=ninja make          # build (release)
make format-fix         # auto-format all source files (run before committing)

make shell              # launch DuckDB shell with lpts extension loaded
make unittest           # run LPTS's own SQL logic tests (fast, use during iteration)

make test               # unittest + the DuckDB-corpus coverage gate (~4 min; run before pushing)
make coverage-check     # just the coverage gate
make coverage-baseline  # regenerate test/duckdb_lpts_baseline.txt after intentional changes
```

Build outputs go to `build/release/`. DuckDB is a git submodule in `duckdb/`.

The coverage gate runs all ~3300 DuckDB sqllogic files through `lpts_check` (LOG mode) and fails on any
NEW `WRONG` (wrong translation) or NEW `FAIL` (LPTS emitted SQL that did not parse/bind — the error was
not a deliberate `LPTS_<CODE>` refusal). `UNSUPPORTED` (deliberate refusals) and `NONDETERMINISTIC` are
acceptable. The committed baseline invariant is `wrong=0 fail=0`. See `docs/test.md` for details.

### Single test

```bash
build/release/test/unittest "test/sql/select.test"
```

### Interactive testing

```bash
make shell
# Inside the shell:
D CREATE TABLE users (id INTEGER, name VARCHAR, age INTEGER);
D PRAGMA lpts('SELECT name FROM users WHERE age > 25');
D SET lpts_check = true;                                        -- turn on round-trip checking
D SELECT name FROM users WHERE age > 25;                        -- runs normally; raises if rewritten wrong
```

## Architecture

The pipeline has three phases:

```
Logical Plan  →  AST  →  CTE List  →  SQL String
```

### Phase 1: `LogicalPlanToAst` (`src/lpts_ast_builder.cpp`)

Walks DuckDB's `LogicalOperator` tree and builds a dialect-agnostic AST. Each `LogicalOperator` node becomes a corresponding `AstNode` with parent-child relationships preserved via the `children` vector. The per-operator dispatch is `BuildNode()`; expression trees are rendered to SQL text via `src/lpts_expression_renderer.cpp`.

### Phase 2: `AstToCteList` (`src/lpts_ast_flattener.cpp`)

Flattens the AST tree (post-order / bottom-up) into an ordered list of `CteNode` objects via `FlattenNode()`. By default (`lpts_merge_pipeline = true`) chains of single-child pipeline operators (Filter/Project/Aggregate/Order/Limit over a scan) are fused into one `MergedSelectNode` — one CTE per *query block* instead of one per operator. Correlated subqueries are decorrelated here; some subtrees are rendered inline via `AstToInlineSQL()` instead of getting their own CTE.

### Phase 3: CTE List → SQL String (`src/cte_nodes.cpp`)

`CteList::ToQuery(true)` serializes the flat list into a pretty-printed WITH ... SELECT SQL string. Each node class implements `ToQuery(SqlDialect)`; a final pass de-prefixes generated column names (`t0_name` → `name`) when globally unambiguous.

### Entry points: `src/lpts_extension.cpp`

| Function | Usage | Description |
|---|---|---|
| `PRAGMA lpts('query')` | Interactive | Returns CTE SQL for the given query |
| `lpts_query('query')` | Table function / tests | Same as above, usable in SELECT |
| `PRAGMA print_ast('query')` | Interactive | Prints the AST tree |
| `print_ast_query('query')` | Table function / tests | AST tree as a table row |
| `lpts_normalize_query('query')` | Table function / tests | Input-dialect SQL normalized to DuckDB SQL |
| `SET lpts_check = true` | Interactive / tests | Round-trip check: every top-level `SELECT` runs normally and raises if LPTS rewrites it wrong |

Both `lpts` and `lpts_query` call the same pipeline:
```cpp
auto ast = LogicalPlanToAst(context, plan, dialect);
auto cte_list = AstToCteList(*ast, dialect, ReadMergePipeline(context));
string result_sql = cte_list->ToQuery(true);
```

### Dialect support

A session setting controls output dialect:
```sql
SET lpts_dialect = 'postgres';  -- or 'duckdb' (default)
PRAGMA lpts('SELECT * FROM users');
```

The `SqlDialect` enum and dialect predicates are defined in `src/include/sql_dialect.hpp` (implementation: `src/sql_dialect.cpp`). Supported output dialects: `duckdb`, `postgres`, `spark`, `hive`, `trino`/`presto`, `snowflake`, `bigquery`, `redshift`, `mysql`/`mariadb`. Dialect-specific function name/argument rewrites live in `src/dialect_function_map.cpp`; the input-dialect normalizer (`lpts_input_dialect`) lives in `src/lpts_parser.cpp` / `src/lpts_sql_scanner.cpp` / `src/lpts_date_format.cpp`.

### AST node hierarchy (`src/include/lpts_ast.hpp`)

```
AstNode (abstract base, has vector<unique_ptr<AstNode>> children)
├── AstGetNode             — table scan / table function
├── AstFilterNode          — WHERE clause
├── AstProjectNode         — column selection (also windows / unnest)
├── AstAggregateNode       — GROUP BY + aggregates
├── AstJoinNode            — JOIN (incl. MARK/SINGLE/SEMI/ANTI)
├── AstDelimJoinNode       — correlated-subquery delim join
├── AstDelimGetNode        — duplicate-eliminated correlation scan
├── AstUnionNode           — UNION / UNION ALL
├── AstSetOperationNode    — EXCEPT / INTERSECT [ALL]
├── AstRecursiveCteNode    — WITH RECURSIVE
├── AstMaterializedCteNode — WITH ... AS MATERIALIZED
├── AstCteRefNode          — reference to a materialized CTE
├── AstOrderNode           — ORDER BY
├── AstLimitNode           — LIMIT / OFFSET
├── AstTopNNode            — fused ORDER BY + LIMIT
├── AstDistinctNode        — SELECT DISTINCT / DISTINCT ON
├── AstSampleNode          — USING SAMPLE
├── AstPositionalJoinNode  — POSITIONAL JOIN
└── AstInsertNode          — INSERT INTO
```

### CTE node hierarchy (`src/include/cte_nodes.hpp`)

```
CteBaseNode (base, ToQuery(SqlDialect))
├── RootNode (virtual) — the final statement, not wrapped in a CTE
│   ├── FinalReadNode — closing SELECT that renames columns
│   ├── InsertNode    — INSERT INTO ... SELECT * FROM <cte>
│   └── UpdateNode / DeleteNode — declared, not yet implemented
└── CteNode (virtual) — one CTE in the WITH clause
    ├── GetNode / FilterNode / ProjectNode / AggregateNode
    ├── JoinNode / PositionalJoinNode / DelimGetNode
    ├── UnionNode / ExceptNode / CteSetOperationNode
    ├── OrderNode / LimitNode / TopNNode / DistinctNode / SampleNode
    ├── RecursiveCteNode  — WITH RECURSIVE body
    └── MergedSelectNode  — fused query block (pipeline fusion output)
```

## Key Source Files

| File | Purpose |
|---|---|
| `src/lpts_extension.cpp` | Extension entry point. Registers pragmas, table functions, all `lpts_*` settings, and the `lpts_check` optimizer hook (strict + `LPTS_CHECK_LOG` log mode, nondeterminism heuristics). |
| `src/lpts_ast_builder.cpp` | **Phase 1.** `LogicalPlanToAst` / `BuildNode()` — walks the `LogicalOperator` tree, extracts per-operator data into AST nodes. The canonical reference for operator extraction logic. |
| `src/lpts_ast_flattener.cpp` | **Phase 2.** `AstToCteList` / `FlattenNode()` — flattens the AST into a CTE list; pipeline fusion, decorrelation, inline-SQL rendering (`AstToInlineSQL`). |
| `src/cte_nodes.cpp` | **Phase 3.** `ToQuery()` implementations for every CTE node + `CteList::ToQuery` (pretty printer, column de-prefixing). |
| `src/include/cte_nodes.hpp` | CTE node class hierarchy + `CteList` declaration. |
| `src/lpts_expression_renderer.cpp` | Renders bound `Expression` trees to SQL text (constants, casts, functions, windows, lambdas, subquery markers). Shared by phases 1–2. |
| `src/include/lpts_ast.hpp` | AST node class hierarchy (`AstGetNode`, `AstFilterNode`, etc.). |
| `src/lpts_ast.cpp` | AST `ToString()` implementations (and `PrintAst()`). |
| `src/lpts_ast_renderer.cpp` | Box-rendered ASCII tree printer for AST debugging (`PRAGMA print_ast`). |
| `src/include/lpts_pipeline.hpp` | Pipeline entry-point declarations: `LogicalPlanToAst`, `AstToCteList`. |
| `src/sql_dialect.cpp` / `src/include/sql_dialect.hpp` | `SqlDialect` enum, dialect predicates, identifier quoting (`DialectQuoteIdent`). |
| `src/dialect_function_map.cpp` | Per-dialect function name/argument rewrites for output SQL. |
| `src/lpts_parser.cpp`, `src/lpts_sql_scanner.cpp`, `src/lpts_date_format.cpp` | Input-dialect normalization (`lpts_input_dialect`, `lpts_normalize_query`). |
| `src/include/lpts_debug.hpp` | Debug flag (`LPTS_DEBUG`) and `LPTS_DEBUG_PRINT` macro. |
| `src/lpts_helpers.cpp` / `src/include/lpts_helpers.hpp` | Utility functions (`VecToSeparatedList`, `EscapeSingleQuotes`, `ThrowLptsNotImplemented`, etc.). |
| `test/corpus_gate/lpts_corpus_gate.cpp` | Standalone C++ driver for the DuckDB sqllogic suite coverage gate (`make coverage-check`); baseline in `test/duckdb_lpts_baseline.txt`. |
| `test/sql/*.test` | SQL logic tests — must always pass. |

## Testing

### Existing test files

| Test file | Covers |
|---|---|
| `test/sql/select.test` | GET, FILTER, PROJECTION |
| `test/sql/group_by.test` | GET, PROJECTION, AGGREGATE, GROUPING SETS, quantiles |
| `test/sql/having.test` | AGGREGATE + FILTER (HAVING), incl. HAVING without GROUP BY |
| `test/sql/join.test` | JOIN types, MARK/SINGLE joins, UNION |
| `test/sql/union.test` | UNION / UNION ALL |
| `test/sql/setops_unnest.test` | EXCEPT / INTERSECT, UNNEST |
| `test/sql/order_limit.test` | ORDER BY, LIMIT, OFFSET |
| `test/sql/distinct.test` | SELECT DISTINCT / DISTINCT ON |
| `test/sql/functions.test` | Scalar functions, casts, `array_to_string` |
| `test/sql/operators.test` | Operator rendering |
| `test/sql/cast.test` | Constant type fidelity (ENUM, BIT, INT_MIN, typed NULL, ...) |
| `test/sql/lambda.test` | Lambda expressions incl. captures |
| `test/sql/lateral_join.test` | LATERAL joins |
| `test/sql/single_join.test` | Scalar-subquery SINGLE joins |
| `test/sql/correlated_subquery_null.test` | Correlated EXISTS / row-IN NULL (3-valued) semantics |
| `test/sql/cte.test` | CTEs, recursive CTEs |
| `test/sql/window.test` | Window functions |
| `test/sql/cross_product.test` | Cross products |
| `test/sql/between.test` | BETWEEN |
| `test/sql/struct_pushdown.test` | Struct field-extraction pushdown |
| `test/sql/virtual_columns.test` | Virtual/path-derived columns, hive partition filters |
| `test/sql/identifiers.test` | Case-insensitive / unicode identifier handling |
| `test/sql/dup_column_names.test` | Generated CTE column-name de-duplication |
| `test/sql/fail_reduction.test` | Regression tests for once-invalid-SQL renderings (large grab bag) |
| `test/sql/read_parquet_union_by_name.test` | read_parquet named-argument round-trip |
| `test/sql/rendering_edges.test` | Miscellaneous rendering edge cases |
| `test/sql/merge_pipeline.test` | Pipeline-fusion (merged SELECT) behavior |
| `test/sql/pretty_print.test` | Exact generated-SQL layout |
| `test/sql/optimizer.test` | Optimizer interaction |
| `test/sql/data_dependent_optimizers.test` | `lpts_enable_data_dependent_optimizers` |
| `test/sql/tpch.test` | All 22 TPC-H queries under `lpts_check` |
| `test/sql/ducklake.test` | DuckLake scans |
| `test/sql/time_travel.test` | Snapshot pinning: Spark/Delta `VERSION`/`TIMESTAMP AS OF` ↔ DuckDB `AT (...)` |
| `test/sql/explain_format_sql.test` | `EXPLAIN (FORMAT SQL)` |
| `test/sql/print_ast.test` | AST `ToString()` output |
| `test/sql/check_mode.test` | `lpts_check` round-trip semantics (canonical example) |
| `test/sql/pragmas.test` | Public function metadata |
| `test/sql/normalization.test`, `test/sql/input_dialect.test` | Input-dialect normalization |
| `test/sql/dialect_*.test` | Output dialects (Postgres, Spark, Hive/Trino/Presto, Snowflake/Redshift, BigQuery, MySQL/MariaDB) + hardening |

### Test structure conventions

Each test uses the DuckDB SQL logic test format:
```
# name: test/sql/example.test
# description: what this tests
# group: [sql]

require lpts

statement ok
SET lpts_check = true;

statement ok
CREATE TABLE t (id INT, val INT);

statement ok
INSERT INTO t VALUES (1, 10);

query II
SELECT id, val FROM t;
----
1    10
```

The canonical example test file is `test/sql/check_mode.test`.

### Key test functions

- **`SET lpts_check = true`** — the primary correctness mechanism. Set once after `require lpts`, then run queries directly. Every top-level `SELECT` is transparently rewritten by LPTS and compared against the original; a wrong rewrite raises an error and fails the test. **Every new test must turn on `lpts_check` and exercise the feature with a bare query.**
  - Use a bare `SELECT ...;` with a `query` block to also assert rows, or `statement ok` when no row assertion is needed.
  - A query LPTS rewrites wrong is a `statement error` whose expected text contains `LPTS check failed`.
- **`lpts_query('query')`** — returns the generated SQL string. Use when you need to assert the exact SQL structure, or for input-dialect tests (MySQL/Spark/etc.) that cannot run as bare DuckDB statements: assert through `SELECT sql FROM lpts_query('...')`.

### TPC-H coverage queries

`test/sql/tpch.test` runs TPC-H queries through `lpts_check`. These are high-value regression tests for multi-table joins, aggregations, subqueries, and complex filters.

When adding TPC-H tests:
- Set `SET lpts_check = true` once, then run each TPC-H query directly to verify round-trip correctness on TPC-H tables
- TPC-H tables are: `lineitem`, `orders`, `customer`, `part`, `partsupp`, `supplier`, `nation`, `region`
- Each test file should `LOAD tpch` and call `CALL dbgen(sf=0.01)` to generate data at a small scale factor
- Focus on queries that exercise operators the current AST layer supports (start with Q1 for aggregates, Q3/Q5 for multi-way joins)

Example TPC-H test structure:
```sql
# name: test/sql/tpch.test
# description: TPC-H queries via lpts_check for operator coverage
# group: [sql]

require lpts
require tpch

statement ok
SET lpts_check = true;

statement ok
CALL dbgen(sf=0.01);

statement ok
SELECT l_returnflag, l_linestatus, sum(l_quantity) AS sum_qty, count(*) AS count_order FROM lineitem WHERE l_shipdate <= CAST('1998-09-02' AS date) GROUP BY l_returnflag, l_linestatus ORDER BY l_returnflag, l_linestatus;
```

### SQL Storm coverage queries

The SQLStorm corpus lives in `benchmark/sqlstorm/SQLStorm` (17036 TPC-H queries, run in bulk via `lpts_sqlstorm_benchmark` — see `docs/benchmark.md`). These are LLM-generated complex queries including CTEs, multi-way joins, window functions, and FULL OUTER JOIN — useful for stress-testing LPTS operator coverage or as a source of individual regression cases.

When adding SQL Storm tests:
- Select a representative sample (e.g., 10–20 queries that exercise different operator combinations)
- Turn on `lpts_check` and run each query directly to verify correctness; queries LPTS deliberately does not support raise an `LPTS_<CODE>: ...` error
- Document which operators each test query exercises in a comment above the test

## Debugging

Set `#define LPTS_DEBUG 1` in `src/include/lpts_debug.hpp` for verbose stderr trace output. This activates `LPTS_DEBUG_PRINT(...)` throughout the codebase. Remember to set it back to `0` before committing.

```cpp
// src/include/lpts_debug.hpp
#define LPTS_DEBUG 1   // set to 1 for debug output, 0 for production
```

Use `EXPLAIN` to inspect DuckDB's logical plan before implementing a new operator:
```sql
EXPLAIN SELECT ...;
```

Use `PRAGMA lpts(...)` to see the full CTE SQL output, and `PRAGMA print_ast(...)` if available to visualize the AST tree.

## Code style (clang-format / clang-tidy)

Run `make format-fix` to auto-format. The project uses DuckDB's `.clang-format` (LLVM-based):

- **Classes/Enums**: `CamelCase` (e.g., `AstGetNode`, `SqlDialect`)
- **Functions**: `CamelCase` (e.g., `LogicalPlanToAst`, `AstToCteList`)
- **Variables/parameters/members**: `lower_case` (e.g., `table_index`, `cte_column_names`)
- **Constants/static/constexpr**: `UPPER_CASE`
- **Macros**: `UPPER_CASE` (e.g., `LPTS_DEBUG_PRINT`)
- **Tabs for indentation**, width 4
- **Column limit**: 120
- **Braces**: same line as statement (K&R / Allman-attached)
- **Pointers**: right-aligned (`int *ptr`)
- **No short functions on single line**

## Configuration options

| Setting | Type | Default | Description |
|---|---|---|---|
| `lpts_dialect` | VARCHAR | `"duckdb"` | SQL output dialect: `duckdb`, `postgres`, `spark`, `hive`, `trino`/`presto`, `snowflake`, `bigquery`, `redshift`, `mysql`/`mariadb` |
| `lpts_input_dialect` | VARCHAR | `"duckdb"` | Input dialect normalized before DuckDB parses the query (same valid values) |
| `lpts_check` | BOOLEAN | `false` | Transparently verify round-trip correctness of every top-level `SELECT` |
| `lpts_merge_pipeline` | BOOLEAN | `true` | Fuse chains of single-child pipeline operators into one flat SELECT per query block instead of one CTE per operator |
| `lpts_enable_data_dependent_optimizers` | BOOLEAN | `false` | Allow planning optimizers that depend on current data/statistics (off so output SQL is data-independent) |

`lpts_check` strict mode raises `Invalid Input Error: LPTS check failed: ...` on a result mismatch and `Invalid Input Error: LPTS check: unsupported query (LPTS could not check it): ...` when LPTS cannot rewrite the query; nondeterministic queries are detected and pass. Setting the `LPTS_CHECK_LOG` environment variable to a file path switches to log mode: LPTS never raises and instead appends one line per intercepted `SELECT` — `<n> OK` (bags matched), `<n> WRONG` (bags differed), `<n> UNSUPPORTED` (deliberate `LPTS_<CODE>`-formatted "not supported" refusal), `<n> FAIL` (could not rewrite with a non-LPTS error — a translation bug, gated like WRONG), or `<n> NONDETERMINISTIC: <reason>` (rewritten but nondeterministic, with the heuristic's explanation).

## DDL / usage examples

```sql
-- Convert a query to CTE SQL (interactive)
PRAGMA lpts('SELECT name FROM users WHERE age > 25');

-- Convert via table function (for tests / scripting)
SELECT sql FROM lpts_query('SELECT name FROM users WHERE age > 25');

-- Turn on round-trip checking, then run the query directly
SET lpts_check = true;
SELECT name FROM users WHERE age > 25;   -- runs normally; raises if rewritten wrong

-- Switch to Postgres dialect
SET lpts_dialect = 'postgres';
PRAGMA lpts('SELECT * FROM users');
```
# Project Notes

## Build

- Use `GEN=ninja make` for builds
- Makefile includes `extension-ci-tools/makefiles/duckdb_extension.Makefile`
- Extension name: `lpts`

## Project Structure
- DuckDB extension project
- Submodules: `duckdb`, `extension-ci-tools`
