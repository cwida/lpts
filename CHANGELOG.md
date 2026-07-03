# Changelog

## Unreleased

## 1.0.0 - 2026-06-23

### Added

- `EXPLAIN (FORMAT SQL) <query>` renders the optimized logical plan as equivalent CTE SQL
  (the same output as `PRAGMA lpts`), exposed as a genuine `EXPLAIN` statement. The CLI
  prints it as plain multi-line text and other clients (JDBC, Python, ...) receive the
  standard two-column `explain_key`/`explain_value` EXPLAIN result, similar to Umbra's
  plan-as-SQL output. Honors `lpts_dialect`. Implemented without changes to DuckDB core.

### Changed

- Generated SQL is now pretty-printed across multiple lines instead of a single line,
  with HAVING fused into its aggregate and join build-side CTEs marked, for more
  readable output.
- Fewer CTEs are emitted while keeping execution order unambiguous, and column names
  are now human-readable (e.g. `t0_scan (t0_name)`) rather than purely positional.
- Bumped the DuckDB target from `v1.5.3` to `v1.5.4` (submodule and CI). The 1.5.4
  optimizer pushes trivial projections/filters into the scan, so single-table plans may
  collapse to a plain `SELECT` with no separate projection CTE; round-trip correctness
  is unchanged.

## 0.9.0 - 2026-05-29

This is the first release-candidate-quality LPTS milestone. It is intended to
stabilize the public DuckDB extension surface before a future `1.0.0` API
stability release.

### Compatibility

- DuckDB target: `v1.5.3`
- extension-ci-tools target: `v1.5-variegata`
- Release tag: `v0.9.0`

### Public API

- `PRAGMA lpts('query')` returns generated CTE SQL.
- `lpts_query('query')` returns generated CTE SQL as a table function.
- `PRAGMA lpts_exec('query')` executes the generated SQL.
- `PRAGMA lpts_check('query')` compares the original query and generated SQL with bag equality.
- `PRAGMA print_ast('query')` prints the LPTS AST tree.
- `print_ast_query('query')` returns the AST tree as a table function.
- `lpts_normalize_query('query')` returns input-dialect SQL normalized to DuckDB SQL.

### Settings

- `lpts_dialect`: output dialect for generated SQL.
- `lpts_input_dialect`: input dialect to normalize before DuckDB parses and plans the query.

Supported dialect values: `duckdb`, `postgres`/`postgresql`, `spark`, `hive`,
`trino`, `presto`, `snowflake`, `bigquery`/`bq`, `redshift`, `mysql`, and
`mariadb`.

### Highlights

- Converts DuckDB optimized logical plans into readable CTE SQL.
- Supports AST inspection through `print_ast` and `print_ast_query`.
- Supports round-trip validation through `lpts_check`.
- Supports input dialect normalization for common SQL syntax differences.
- Supports output dialect rendering for common warehouse and lakehouse engines.
- Exposes function descriptions and examples through `duckdb_functions()`.
- Includes a SQLStorm TPC-H benchmark runner.

### Validation

- Local SQLLogicTest suite: `2890` assertions passed in `66` test cases.
- SQLStorm TPC-H `sf=0.001` baseline:
  - Total queries: `17036`
  - Strict successes: `15833`
  - Nondeterministic: `387`
  - DuckDB errors: `807`
  - LPTS timeouts: `4`
  - DuckDB timeouts: `5`
- SQLStorm TPC-H `sf=0.0001` baseline:
  - Total queries: `17036`
  - Strict successes: `15646`
  - Not implemented: `578`
  - DuckDB errors: `802`
  - Nondeterministic: `6`
  - Timeouts: `4`

The preferred SQLStorm baseline for this release is `sf=0.001`, because it
reflects the latest stronger LPTS coverage.

### Known Caveats

- LPTS serializes DuckDB's optimized logical plan, not the original SQL text or formatting.
- `lpts_check` can fail for nondeterministic queries, such as queries without a fully specified output order.
- Input dialect support is normalization for common syntax, not a full SQLGlot/Coral-style parser stack.
- Some SQLStorm failures are DuckDB-side errors, timeouts, or scale-factor-dependent optimizer behavior.
