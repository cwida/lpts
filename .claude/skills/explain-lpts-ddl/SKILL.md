---
name: explain-lpts-ddl
description: Reference for LPTS DDL syntax — PRAGMA lpts, lpts_query, the lpts_check round-trip setting, lpts_normalize_query, dialect settings, and print_ast. Auto-loaded when discussing LPTS usage, pragmas, table functions, round-trip checks, or dialect configuration.
---

## LPTS DDL Overview

LPTS extends DuckDB with pragmas, table functions, and a session setting for converting
logical plans to SQL strings. All functions are registered in `src/lpts_extension.cpp`.

### Converting a Query to CTE SQL

**PRAGMA lpts('query')** — Interactive use. Returns the CTE SQL as a single-row result.

```sql
D PRAGMA lpts('SELECT name FROM users WHERE age > 25');
┌─────────────────────────────────────────────────────────────────────┐
│ sql                                                                 │
├─────────────────────────────────────────────────────────────────────┤
│ WITH scan_0(t0_age, t0_name) AS (SELECT age, name FROM ...),       │
│ filter_1 AS (SELECT * FROM scan_0 WHERE (t0_age) > (CAST(25 ...))),│
│ projection_2(t1_name) AS (SELECT t0_name FROM filter_1)            │
│ SELECT t1_name AS name FROM projection_2;                           │
└─────────────────────────────────────────────────────────────────────┘
```

**lpts_query('query')** — Table function for programmatic use and tests:

```sql
SELECT sql FROM lpts_query('SELECT name FROM users WHERE age > 25');
```

Both call the same pipeline:
```cpp
auto ast = LogicalPlanToAst(context, planner.plan);
SqlDialect dialect = ReadDialect(context);
auto cte_list = AstToCteList(*ast, dialect);
string result_sql = cte_list->ToQuery(true);
```

### Round-Trip Correctness Check

**SET lpts_check = true** — The primary correctness mechanism. Once on, every top-level
`SELECT` is intercepted: LPTS runs the original query and its LPTS rewrite side by side
and compares their result bags with an order-independent hash. The query returns its
normal rows unchanged.

```sql
D SET lpts_check = true;
D SELECT name FROM users WHERE age > 25;   -- runs normally; raises if rewritten wrong
----
Carol
```

By default the check is strict. On a mismatch it raises
`Invalid Input Error: LPTS check failed: ...`; when LPTS cannot rewrite the query it
raises `Invalid Input Error: LPTS check: unsupported query (LPTS could not check it):
...`. Nondeterministic queries (no fully specified order, `random()`, unordered
aggregates, etc.) are detected and pass without error. Queries that read LPTS's own
table functions (`lpts_query`, `print_ast_query`, `lpts_normalize_query`) and queries
running under statement verification (for example `PRAGMA enable_verification`) are
skipped.

Setting the `LPTS_CHECK_LOG` environment variable to a file path switches to log mode:
with `lpts_check` on, LPTS never raises and instead appends one line per intercepted
`SELECT` — `<n> FAIL` (could not rewrite), `<n> OK` (bags matched), `<n> WRONG` (bags
differed), or `<n> NONDETERMINISTIC: <reason>` (rewritten but nondeterministic, with the
heuristic's explanation) — where `<n>` is the 1-based interception index. This is how
DuckDB's own sqllogic corpus is run through LPTS.

**Every new test must turn on `lpts_check` and exercise the feature with a bare query.**
This is the authoritative correctness check for LPTS.

### Normalizing Input-Dialect SQL

**lpts_normalize_query('query')** — Table function returning the input-dialect SQL
normalized to DuckDB SQL (honors `lpts_input_dialect`):

```sql
SELECT sql FROM lpts_normalize_query('SELECT `order` FROM events LIMIT 5, 10');
```

### AST Debugging

**PRAGMA print_ast('query')** — Visualizes the AST tree structure:

```sql
D PRAGMA print_ast('SELECT name FROM users WHERE age > 25');
```

Useful for debugging the `LogicalPlanToAst` phase. The box-rendered tree is produced
by `src/lpts_ast_renderer.cpp`.

### Dialect Configuration

```sql
-- Set dialect to PostgreSQL (default: 'duckdb')
SET lpts_dialect = 'postgres';

-- Convert with Postgres-compatible syntax
PRAGMA lpts('SELECT * FROM users');
-- Table references will be unqualified (no catalog.schema prefix)
```

The `lpts_dialect` setting is registered as a DuckDB extension option and read via
`ReadDialect()` in `src/lpts_extension.cpp`. Valid values: `'duckdb'`, `'postgres'`.

### Supported Query Patterns

| Pattern | Operators Used | Example |
|---|---|---|
| Simple scan | GET, PROJECTION | `SELECT * FROM t` |
| Column selection | GET, PROJECTION | `SELECT a, b FROM t` |
| Filter | GET, FILTER, PROJECTION | `SELECT * FROM t WHERE x > 5` |
| Grouped aggregate | GET, AGGREGATE, PROJECTION | `SELECT a, SUM(b) FROM t GROUP BY a` |
| HAVING | GET, AGGREGATE, FILTER | `SELECT a, SUM(b) FROM t GROUP BY a HAVING SUM(b) > 10` |
| Inner join | GET, JOIN, PROJECTION | `SELECT ... FROM t1 JOIN t2 ON ...` |
| UNION / UNION ALL | GET, UNION, PROJECTION | `SELECT a FROM t1 UNION ALL SELECT a FROM t2` |
| ORDER BY | GET, ORDER, PROJECTION | `SELECT * FROM t ORDER BY a` |
| LIMIT / OFFSET | GET, LIMIT, PROJECTION | `SELECT * FROM t LIMIT 10 OFFSET 5` |
| DISTINCT | GET, DISTINCT, PROJECTION | `SELECT DISTINCT a FROM t` |
| Scalar functions | GET, PROJECTION | `SELECT upper(name) FROM t` |
| INSERT | GET, INSERT | `INSERT INTO t SELECT * FROM s` |

### Limitations

- Subqueries in WHERE (correlated and uncorrelated) are partially supported
- Window functions are not yet supported
- FULL OUTER JOIN, CROSS JOIN, SEMI JOIN may hit `NotImplementedException`
- CTEs in the source query (WITH clauses) are expanded by DuckDB's planner before
  LPTS sees them — the output will not preserve the original CTE structure
- Very complex expressions (CASE WHEN, nested casts) may produce verbose output

### Key Source Files

- `src/lpts_extension.cpp` — Extension entry point, registers all pragmas and settings
- `src/lpts_pipeline.cpp` — `LogicalPlanToAst` and `AstToCteList` implementations
- `src/include/lpts_pipeline.hpp` — `SqlDialect` enum and pipeline function declarations
- `src/lpts_ast_renderer.cpp` — Box-rendered AST tree printer for `print_ast`
