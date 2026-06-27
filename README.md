# LPTS

A DuckDB extension for **optimized-plan inspection** and **cross-system SQL
transpilation**. LPTS takes DuckDB's post-optimizer logical plan and reconstructs
equivalent SQL as a sequence of named CTEs.

## PRAGMA Syntax

```sql
PRAGMA lpts('<query>');
```

Example:

```sql
INSTALL lpts FROM community;
LOAD lpts;

SET lpts_input_dialect = 'duckdb';
SET lpts_dialect = 'duckdb';

CREATE TABLE users (id INTEGER, name VARCHAR, age INTEGER);
INSERT INTO users VALUES (1, 'Alice', 30), (2, 'Bob', 22), (3, 'Carol', 28);

PRAGMA lpts('SELECT name FROM users WHERE age > 25');
```

```text
WITH scan_0 (t0_name) AS (SELECT name FROM memory.main.users WHERE age>25),
projection_1 (t1_name) AS (SELECT t0_name FROM scan_0)
SELECT t1_name AS "name" FROM projection_1;
```

LPTS plans the query through DuckDB, optimizes it, then serializes the optimized
logical plan. For more details, check the [related report](LPTS_Research_Project_Report.pdf).

## `EXPLAIN (FORMAT SQL)`

LPTS extends DuckDB's `EXPLAIN` with a `SQL` format. `EXPLAIN (FORMAT SQL) <query>`
returns the optimized logical plan rendered as equivalent CTE SQL — the same output as
`PRAGMA lpts`, but exposed as a first-class `EXPLAIN` statement (similar to the
plan-as-SQL output in Umbra).

```sql
EXPLAIN (FORMAT SQL) SELECT name FROM users WHERE age > 25;
```

```text
┌─────────────────────────────┐
│┌───────────────────────────┐│
││  Optimized Logical Plan   ││
│└───────────────────────────┘│
└─────────────────────────────┘
WITH scan_0 (t0_name) AS (SELECT "name" FROM memory.main.users WHERE age>25),
projection_1 (t1_name) AS (SELECT t0_name FROM scan_0)
SELECT t1_name AS "name" FROM projection_1;
```

Because it is a genuine `EXPLAIN` statement, every client treats it like any other
`EXPLAIN`: the CLI prints the SQL as plain multi-line text (no result box), while JDBC,
Python, and other clients receive the standard two-column (`explain_key`, `explain_value`)
EXPLAIN result with the SQL in `explain_value`. It honors `lpts_dialect` just like
`PRAGMA lpts`, and a query that cannot be planned or converted raises a normal error.

## Supported Dialects

The dialect settings accept these values:

| Dialect | Accepted values |
|---|---|
| DuckDB | `duckdb` |
| PostgreSQL | `postgres`, `postgresql` |
| Spark SQL | `spark` |
| Hive | `hive` |
| Trino / Presto | `trino`, `presto` |
| Snowflake | `snowflake` |
| BigQuery | `bigquery`, `bq` |
| Redshift | `redshift` |
| MySQL / MariaDB | `mysql`, `mariadb` |

## Pragmas and Functions

| Function | Description |
|---|---|
| `EXPLAIN (FORMAT SQL) query` | Explain a query as equivalent CTE SQL, via a real `EXPLAIN` statement |
| `PRAGMA lpts('query')` | Return generated CTE SQL |
| `lpts_query('query')` | Table-function form of `PRAGMA lpts` |
| `PRAGMA print_ast('query')` | Print the AST to stdout |
| `print_ast_query('query')` | Table-function form of `PRAGMA print_ast` |
| `lpts_normalize_query('query')` | Return input-dialect SQL normalized to DuckDB SQL |

To verify round-trip correctness, turn on the `lpts_check` session setting (see
[Settings](#settings)). Once on, every top-level `SELECT` runs normally and is
transparently compared against its LPTS rewrite. Nondeterministic queries — for
example when row order or tie-breaking is not fully specified — are detected and
pass without error.

## Use Cases

- Inspect optimized DuckDB plans as SQL.
- Debug optimizer rewrites such as filter pushdown, join reordering, top-N,
  materialized CTEs, and subquery decorrelation.
- Generate a CTE program that communicates the optimized execution shape.
- Emit SQL for another engine with `lpts_dialect`.
- Convert other SQL dialect syntax to DuckDB SQL with `lpts_input_dialect`,
  then execute or inspect it.

## Supported Operators

LPTS is intended to cover all logical operators produced by optimized DuckDB
SELECT plans. The current regression suite round-trips all 22 TPC-H queries and
exercises joins, aggregates, windows, set operations, CTEs, recursive CTEs,
table functions, DuckLake scans, and inserts.

Unsupported optimizer edge cases fail explicitly with `NotImplementedException`.

## Settings

| Setting | Type | Default | Description |
|---|---|---|---|
| `lpts_dialect` | VARCHAR | `duckdb` | Output dialect for generated SQL |
| `lpts_input_dialect` | VARCHAR | `duckdb` | Input dialect to normalize before DuckDB parses and plans the query |
| `lpts_check` | BOOLEAN | `false` | Transparently verify round-trip correctness of every top-level `SELECT` (see below) |
| `lpts_enable_data_dependent_optimizers` | BOOLEAN | `false` | Allow LPTS planning to use optimizers that depend on current data, statistics, cardinality estimates, row groups, or runtime dynamic filters |

By default, LPTS avoids data-dependent optimizers so generated SQL does not bake
in snapshot-specific facts such as `WHERE false` from current table statistics.
Enable `lpts_enable_data_dependent_optimizers` when you want DuckDB's full
optimized plan shape and accept that the SQL may depend on planning-time data.

### Round-trip checking with `lpts_check`

Turn on `lpts_check` to verify LPTS transparently:

```sql
SET lpts_check = true;
```

Once on, every top-level `SELECT` you run is intercepted. LPTS runs the original
query and its LPTS rewrite side by side and compares their result bags with an
order-independent hash. The query returns its normal rows unchanged.

By default the check is strict. If the rewrite's result bag differs, LPTS raises
`Invalid Input Error: LPTS check failed: ...`. If LPTS cannot rewrite the query at
all, it raises `Invalid Input Error: LPTS check: unsupported query (LPTS could not
check it): ...`. Nondeterministic queries — no fully specified order, `random()`,
unordered aggregates, and the like — are detected and pass without error.

Queries that read LPTS's own table functions (`lpts_query`, `print_ast_query`,
`lpts_normalize_query`) are skipped, as are queries running under statement
verification (for example `PRAGMA enable_verification`).

Set the environment variable `LPTS_CHECK_LOG` to a file path to switch to log
mode. With `lpts_check` on and `LPTS_CHECK_LOG` set, LPTS never raises; instead it
appends one line per intercepted `SELECT` tagging the outcome: `<n> FAIL` (could not
rewrite), `<n> OK` (bags matched), `<n> WRONG` (bags differed), or
`<n> NONDETERMINISTIC: <reason>` (rewritten but nondeterministic, so the comparison is
not trusted, with the heuristic's explanation). `<n>` is the 1-based interception index.
This is how DuckDB's own sqllogic corpus is run through LPTS.

## Examples

```sql
CREATE TABLE events (id INTEGER, ts TIMESTAMP, name VARCHAR, "order" INTEGER);
INSERT INTO events VALUES
    (1, TIMESTAMP '2024-01-15 08:09:10', 'alpha', 10),
    (11, TIMESTAMP '2024-01-16 11:12:13', 'beta', 20);

SET lpts_dialect = 'postgres';

-- Return generated CTE SQL directly in the shell.
PRAGMA lpts(
    'SELECT strftime(ts, ''%Y-%m-%d'') AS day
     FROM events
     WHERE id > 10
     ORDER BY day'
);

-- Return generated CTE SQL as a table row, useful in scripts and tests.
SELECT sql
FROM lpts_query(
    'SELECT strftime(ts, ''%Y-%m-%d'') AS day
     FROM events
     WHERE id > 10
     ORDER BY day'
);
```

```text
WITH scan_0 (t0_ts) AS (SELECT ts FROM events WHERE id>10),
projection_1 (t1_day) AS (SELECT to_char(t0_ts, 'YYYY-MM-DD') FROM scan_0),
order_2 (t1_day) AS (SELECT t1_day FROM projection_1 ORDER BY t1_day ASC NULLS LAST)
SELECT t1_day AS "day" FROM order_2;
```

```sql
SET lpts_dialect = 'duckdb';

-- Turn on transparent round-trip checking once per session.
SET lpts_check = true;

-- Runs normally and returns its rows; raises only if LPTS rewrites it wrong.
SELECT name FROM events WHERE id > 10 ORDER BY name;
```

```text
name
----
beta
```

```sql
-- Print the AST tree to stdout for interactive debugging.
PRAGMA print_ast('SELECT name FROM events WHERE id > 10 ORDER BY name');

-- Return the AST tree as a table row, useful for tools and regression tests.
SELECT ast
FROM print_ast_query('SELECT name FROM events WHERE id > 10 ORDER BY name');
```

```sql
SET lpts_input_dialect = 'mysql';

-- Normalize source-dialect SQL to DuckDB SQL before planning or execution.
SELECT sql
FROM lpts_normalize_query(
    'SELECT `order`, DATE_FORMAT(ts, ''%Y-%m-%d %H:%i:%s'') AS formatted FROM events LIMIT 5, 10'
);
```

```text
SELECT "order", strftime(ts, '%Y-%m-%d %H:%M:%S') AS formatted FROM events LIMIT 10 OFFSET 5
```

## Documentation

- **[Building](docs/building.md)** - build, local loading, updating, and CLion setup
- **[Tests](docs/test.md)** - SQLLogicTest conventions
- **[Benchmark](docs/benchmark.md)** - SQLStorm benchmark runner

Maintainer: [ila](https://github.com/ila)
