# LPTS Implementation

LPTS converts DuckDB's optimized logical plan into readable SQL. The output is a
flat CTE program that preserves the semantics of the optimized plan.

```text
Logical Plan -> AST -> CTE List -> SQL String
```

## Build

Sync submodules before building.

```bash
git submodule update --init --recursive
GEN=ninja make
```

The release build produces:

```text
build/release/duckdb
build/release/test/unittest
build/release/extension/lpts/lpts.duckdb_extension
build/release/extension/lpts/lpts_sqlstorm_benchmark
```

Run the local shell and load the extension:

```bash
build/release/duckdb -unsigned
```

```sql
LOAD 'build/release/extension/lpts/lpts.duckdb_extension';
```

## Tests

Run one SQLLogicTest file directly:

```bash
build/release/test/unittest "test/sql/select.test"
```

Run a high-value regression file:

```bash
build/release/test/unittest "test/sql/tpch.test"
```

Benchmark commands live in `benchmark/README.md`.

Every new feature test must include `PRAGMA lpts_check('<query>')` and expect
`true`. Use `PRAGMA lpts_exec('<query>')` when you also want concrete result
rows.

## Phase 1: Logical Plan To AST

Entry point: `LogicalPlanToAst` in `src/lpts_pipeline.cpp`.

DuckDB parses, binds, plans, and optimizes the input query before LPTS sees it.
LPTS walks the optimized `LogicalOperator` tree bottom-up and creates a typed
AST node for each supported operator.

The AST is dialect-independent. It stores table names, CTE column names,
expressions, join conditions, grouping information, and CTE references without
choosing final SQL syntax.

The main bookkeeping structure maps DuckDB `ColumnBinding` values to stable CTE
column names. A scan creates names such as `t0_name`; later filters,
projections, joins, and aggregates resolve internal column bindings through that
map.

## Phase 2: AST To CTE List

Entry point: `AstToCteList` in `src/lpts_pipeline.cpp`.

The AST flattener walks the tree in post-order. Each AST node becomes one
`CteNode`. The traversal order guarantees that every generated CTE references
only CTEs defined earlier in the `WITH` clause.

Common CTE names:

| AST node | CTE name |
|---|---|
| `AstGetNode` | `scan_N` |
| `AstFilterNode` | `filter_N` |
| `AstProjectNode` | `projection_N` |
| `AstAggregateNode` | `aggregate_N` |
| `AstJoinNode` | `join_N` |
| `AstUnionNode` | `union_N` |
| `AstOrderNode` | `order_N` |
| `AstLimitNode` | `limit_N` |
| `AstTopNNode` | `topn_N` |
| `AstDistinctNode` | `distinct_N` |
| `AstRecursiveCteNode` | `recursive_cte_N` |

Materialized CTEs and delim joins need custom ordering. The flattener emits the
shared CTE body before any `CteRef` scan, and it emits delim scans before the
inner side of a decorrelated subquery join.

## Phase 3: CTE List To SQL

Implementation: `src/cte_nodes.cpp`.

Each `CteNode::ToQuery()` renders the body of one CTE. `CteList::ToQuery(true)`
joins those CTEs into a final `WITH ... SELECT` string. The final read node maps
LPTS column names back to user-facing output names.

The dialect setting is applied during AST flattening and expression rendering.
DuckDB output keeps fully qualified table names. PostgreSQL output currently
removes DuckDB catalog/schema qualifiers and remaps a small set of function
names.

## Entry Points

Implementation: `src/lpts_extension.cpp`.

| Entry point | Purpose |
|---|---|
| `PRAGMA lpts('<query>')` | Return generated SQL as one row. |
| `lpts_query('<query>')` | Return generated SQL from a table function. |
| `PRAGMA lpts_exec('<query>')` | Generate SQL and execute it. |
| `PRAGMA lpts_check('<query>')` | Compare original and generated SQL with bag equality. |
| `PRAGMA print_ast('<query>')` | Print the AST to stdout. |
| `print_ast_query('<query>')` | Return the AST as a table-function result. |
| `SET lpts_dialect = 'duckdb'` | Emit DuckDB SQL. |
| `SET lpts_dialect = 'postgres'` | Emit partial PostgreSQL SQL. |

## Source Map

| File | Role |
|---|---|
| `src/lpts_extension.cpp` | Extension loading, SQL entry points, planning, optimizer invocation |
| `src/lpts_pipeline.cpp` | Logical plan traversal, expression rendering, AST flattening |
| `src/include/lpts_ast.hpp` | AST node classes |
| `src/lpts_ast.cpp` | AST string rendering |
| `src/lpts_ast_renderer.cpp` | Box/tree renderer for AST debugging |
| `src/include/cte_nodes.hpp` | CTE node classes |
| `src/cte_nodes.cpp` | SQL serialization for each CTE node |
| `src/lpts_helpers.cpp` | Shared string helpers |
| `src/include/lpts_debug.hpp` | `LPTS_DEBUG_PRINT` macro |
| `test/sql/*.test` | SQLLogicTests for round-trip correctness |

## Adding An Operator

1. Search the existing code and tests for a similar operator.
2. Add or extend an AST node in `src/include/lpts_ast.hpp`.
3. Extract the operator fields in `LogicalPlanToAst`.
4. Register any new column bindings before parent operators need them.
5. Add a CTE node or reuse an existing node in `src/include/cte_nodes.hpp`.
6. Render the SQL in `src/cte_nodes.cpp`.
7. Add `LPTS_DEBUG_PRINT` statements around the new extraction and flattening
   paths.
8. Add SQLLogicTests with `PRAGMA lpts_check('<query>')` returning `true`.
9. Verify with real queries using `PRAGMA lpts`, `PRAGMA lpts_exec`, and
   `PRAGMA lpts_check`.

## Debugging

Inspect DuckDB's plan first:

```sql
EXPLAIN SELECT name FROM users WHERE age > 25;
```

Inspect the LPTS AST:

```sql
PRAGMA print_ast('SELECT name FROM users WHERE age > 25');
SELECT ast FROM print_ast_query('SELECT name FROM users WHERE age > 25');
```

Enable debug logging by setting `LPTS_DEBUG` to `1` in
`src/include/lpts_debug.hpp`. Set it back to `0` before committing.

## Known Limits

- LPTS serializes the optimized logical plan, not the original parse tree.
- PostgreSQL output is partial and needs more dialect-specific rendering.
- Unsupported operators and expression forms throw `NotImplementedException`.
- Recursive CTE support serializes the recursive step inline, so only the
  operators supported by that inline renderer work inside the recursive step.
- Nondeterministic queries can fail `lpts_check` even when generated SQL is
  semantically acceptable.
