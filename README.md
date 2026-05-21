# LPTS

A DuckDB extension for **logical-plan-to-SQL reconstruction**. LPTS converts
DuckDB's optimized logical plan into equivalent SQL expressed as a sequence of
named CTEs.

This is useful for inspecting optimizer rewrites, debugging compiler-generated
plans, and porting optimized query semantics across SQL systems.

## Quick start

```bash
git submodule update --init --recursive
GEN=ninja make
build/release/duckdb -unsigned
```

```sql
LOAD 'build/release/extension/lpts/lpts.duckdb_extension';

-- Create a table and convert a query to CTE SQL
CREATE TABLE users (id INTEGER, name VARCHAR, age INTEGER);
INSERT INTO users VALUES (1, 'Alice', 30), (2, 'Bob', 22), (3, 'Carol', 28);

PRAGMA lpts('SELECT name FROM users WHERE age > 25');
```

```text
WITH scan_0 (t0_name) AS (SELECT name FROM memory.main.users WHERE age>25),
projection_1 (t1_name) AS (SELECT t0_name FROM scan_0)
SELECT t1_name AS "name" FROM projection_1;
```

```sql
-- Check semantic round-trip correctness
PRAGMA lpts_check('SELECT name FROM users WHERE age > 25');
```

```text
true
```

## How It Works

LPTS runs after DuckDB parsing, binding, planning, and optimization. It converts
the optimized plan through three representations:

```text
Logical Plan -> AST -> CTE List -> SQL
```

The output reflects the optimized plan, not the original query text. Optimizer
effects such as filter pushdown, CTE materialization, top-N fusion, and subquery
decorrelation can appear directly in the generated SQL.

## Supported Query Shapes

| Query shape | Coverage |
|---|---|
| `SELECT ... FROM`, `WHERE`, scalar expressions | Round-trip tested |
| `GROUP BY`, `HAVING`, aggregate functions | Round-trip tested |
| Inner, outer, cross, semi, anti, mark, dependent joins | Round-trip tested |
| `UNION`, `UNION ALL`, `EXCEPT`, `INTERSECT` | Round-trip tested |
| `ORDER BY`, `LIMIT`, `OFFSET`, top-N plans | Round-trip tested |
| `DISTINCT` | Round-trip tested |
| Window functions and frames | Round-trip tested |
| Inlined, materialized, nested, and recursive CTEs | Round-trip tested |
| Table functions, `VALUES`, DuckLake scans | Round-trip tested |
| `INSERT INTO ... SELECT ...` | Supported |

The test suite includes all 22 TPC-H queries at scale factor `0.01`.

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

## Pragmas and Functions

| Function | Description |
|---|---|
| `PRAGMA lpts('query')` | Return generated CTE SQL |
| `lpts_query('query')` | Table-function form of `PRAGMA lpts` |
| `PRAGMA lpts_exec('query')` | Execute the generated SQL |
| `PRAGMA lpts_check('query')` | Compare original and generated SQL with bag equality |
| `PRAGMA print_ast('query')` | Print the AST to stdout |
| `print_ast_query('query')` | Table-function form of `PRAGMA print_ast` |

## Limitations

- Source tables must exist when LPTS plans the query.
- LPTS does not preserve formatting, alias spelling, or original CTE structure.
- PostgreSQL dialect support is partial.
- Unsupported logical operators or expression forms throw
  `NotImplementedException`.
- `PRAGMA lpts_check` can fail on nondeterministic queries, such as unordered
  aggregates or `LIMIT` queries with ties.

## Documentation

- **[Implementation](IMPLEMENTATION.md)** - build instructions, pipeline notes, and development workflow
- **[Tests](test/README.md)** - SQLLogicTest conventions
- **[Benchmarks](benchmark/README.md)** - SQLStorm benchmark runner

## TODO

- TODO dialects: complete PostgreSQL rendering for casts, intervals, function
  names, date/time format strings, identifier quoting, and null ordering.
- TODO put the PDF report: add `LPTS_Research_Project_Report.pdf` to the repo
  and link it from this README.
