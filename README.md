# LPTS

A DuckDB extension for **optimized-plan inspection** and **cross-system SQL
compilation**. LPTS takes DuckDB's post-optimizer logical plan and reconstructs
equivalent SQL as a sequence of named CTEs.

This makes optimizer rewrites visible: filter pushdown, join reordering, CTE
materialization, top-N rewrites, and subquery decorrelation can all show up in
the generated SQL.

## PRAGMA Syntax

```sql
PRAGMA lpts('<query>');
```

Example:

```sql
LOAD 'build/release/extension/lpts/lpts.duckdb_extension';

CREATE TABLE users (id INTEGER, name VARCHAR, age INTEGER);
INSERT INTO users VALUES (1, 'Alice', 30), (2, 'Bob', 22), (3, 'Carol', 28);

PRAGMA lpts('SELECT name FROM users WHERE age > 25');
```

```text
WITH scan_0 (t0_name) AS (SELECT name FROM memory.main.users WHERE age>25),
projection_1 (t1_name) AS (SELECT t0_name FROM scan_0)
SELECT t1_name AS "name" FROM projection_1;
```

Check round-trip correctness:

```sql
PRAGMA lpts_check('SELECT name FROM users WHERE age > 25');
```

```text
true
```

## Pragmas and Functions

| Function | Description |
|---|---|
| `PRAGMA lpts('query')` | Return generated CTE SQL |
| `lpts_query('query')` | Table-function form of `PRAGMA lpts` |
| `PRAGMA lpts_exec('query')` | Execute the generated SQL |
| `PRAGMA lpts_check('query')` | Compare original and generated SQL with bag equality |
| `PRAGMA print_ast('query')` | Print the AST to stdout |
| `print_ast_query('query')` | Table-function form of `PRAGMA print_ast` |

## Supported Operators

LPTS is intended to cover all logical operators produced by optimized DuckDB
SELECT plans. The current regression suite round-trips all 22 TPC-H queries and
exercises joins, aggregates, windows, set operations, CTEs, recursive CTEs,
table functions, DuckLake scans, and inserts.

Unsupported optimizer edge cases fail explicitly with `NotImplementedException`.

## Settings

| Setting | Type | Default | Description |
|---|---|---|---|
| `lpts_dialect` | VARCHAR | `duckdb` | Output dialect: `duckdb` or `postgres` |

```sql
SET lpts_dialect = 'postgres';
PRAGMA lpts('SELECT name FROM users WHERE age > 25');
```

PostgreSQL output currently removes DuckDB catalog/schema qualifiers and remaps
a small set of function names. Full dialect portability is still in progress.

## Limitations

- Source tables must exist when LPTS plans the query.
- LPTS reconstructs the optimized plan, not the original SQL text.
- LPTS does not preserve formatting, alias spelling, or original CTE structure.
- PostgreSQL dialect support is partial.
- `PRAGMA lpts_check` can fail on nondeterministic queries, such as unordered
  aggregates or `LIMIT` queries with ties.

## Documentation

- **[Implementation](IMPLEMENTATION.md)** - build instructions, CLion setup, pipeline notes, and development workflow
- **[Tests](test/README.md)** - SQLLogicTest conventions
- **[Benchmarks](benchmark/README.md)** - SQLStorm benchmark runner

## TODO

- TODO dialects: complete PostgreSQL rendering for casts, intervals, function
  names, date/time format strings, identifier quoting, and null ordering.
- TODO put the PDF report: add `LPTS_Research_Project_Report.pdf` to the repo
  and link it from this README.
