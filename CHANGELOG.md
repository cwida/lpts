# Changelog

## Unreleased

### Removed

- `PRAGMA lpts_exec('query')`, the boolean-returning `PRAGMA lpts_check('query')`, and
  the `lpts_check_log` setting. Round-trip verification is now driven by the `lpts_check`
  session setting (see Added).

### Added

- The DuckDB-corpus coverage gate is now part of `make test` (unit tests + gate) with dedicated
  `make coverage-check` / `make coverage-baseline` targets. A non-rewritable query is classified by its
  error: a deliberate `LPTS_<CODE>`-formatted "not supported" refusal logs `UNSUPPORTED` (acceptable);
  any other error logs `FAIL` (LPTS emitted SQL that failed to parse/bind — a translation bug). The
  gate fails on any NEW `WRONG` and any NEW `FAIL`. All intentional refusal messages have been
  standardized to the `LPTS_<CODE>: ...` format so the check can enforce this. Baseline:
  `wrong=0 fail=0` (every non-rewritable query is a deliberate UNSUPPORTED refusal).
- `lpts_check` BOOLEAN session setting (default `false`). With `SET lpts_check = true`,
  every top-level `SELECT` is intercepted: LPTS runs the original and the rewritten query
  side by side and compares their result bags with an order-independent hash. The query
  returns its normal rows unchanged. Strict mode raises `Invalid Input Error: LPTS check
  failed: ...` on a mismatch and `Invalid Input Error: LPTS check: unsupported query
  (LPTS could not check it): ...` when LPTS cannot rewrite the query; nondeterministic
  queries are detected and pass — the nondeterminism heuristic covers order-sensitive
  aggregates (string_agg/group_concat/listagg/list/array_agg, even with an ORDER BY whose
  key may not be unique), approximate aggregates (approx_quantile/approx_count_distinct/
  reservoir_quantile/approx_top_k), window functions over tied keys, ORDER BY with
  LIMIT/OFFSET, random(), and floating aggregates. If the `LPTS_CHECK_LOG` environment
  variable points to a file, LPTS never raises and instead appends `<n> OK`, `<n> WRONG`,
  `<n> UNSUPPORTED`, `<n> FAIL`, or `<n> NONDETERMINISTIC: <reason>` per intercepted `SELECT`.
  Queries reading
  LPTS's own table functions are skipped; under statement verification the check is skipped
  in strict mode and verification is neutralized in log mode so those files are still checked.
- DuckDB sqllogic suite coverage gate: `scripts/run_duckdb_lpts_coverage.sh` runs DuckDB's
  own `test/sql/**` corpus through LPTS in parallel (log mode) and diffs the per-file result
  against the committed `test/duckdb_lpts_baseline.txt`, failing on any new `WRONG`.

### Changed

- The `lpts_check` nondeterminism detection now also scans the bound plan for sequence functions
  (`nextval`/`currval`/`lastval`), not just the query text. A sequence call hidden inside a macro
  (`SELECT my_macro(...)`) was invisible to the text heuristic, so the round-trip — which runs the original
  then the rewrite and thus advances the sequence twice — was reported as a wrong result. It is now
  correctly treated as nondeterministic (the translation is faithful; it just cannot be verified by
  re-execution).

### Fixed

- Second sweep finishing the invalid-SQL FAIL elimination — **~50 → 0** (every remaining FAIL in the
  coverage gate is now an intentional `LPTS_UNSUPPORTED_*`):
  - Constant `ORDER BY` keys (a folded alias like `10 AS j`) are dropped: DuckDB re-reads a bare — even
    parenthesized — integer as an ordinal, and ordering by a constant is a no-op.
  - UNION-typed constants rebuild as `CAST(union_value(tag := v) AS UNION(...))`; TYPE-typed constants
    (`get_type`/`make_type`) as `get_type(CAST(NULL AS <type>))`; GEOMETRY constants (a native type id
    since v1.5, printed as bare WKT) as `CAST('<wkt>' AS GEOMETRY)`.
  - `list_reduce` over an empty/folded list: emitted lambdas pad to the function's required arity
    (`lambda : body` does not parse), casts to the untyped-NULL type are dropped, and `NULL[]`-typed
    empty-list constants render structurally (`[]`).
  - Set-operation branches select only the set-op's arity (an inner `ORDER BY expr` can leave an extra
    projected column; `SELECT *` then mismatches the column counts).
  - Table-function scans: filter-only columns keep their real name in the `_tf(...)` alias, and the alias
    now names EVERY output position with its bound name (placeholders could collide with or displace
    function-derived columns like auto-detected hive partition keys, incl. inside recursive CTE steps).
  - Struct-field ColumnIndex children are only rendered as scan-level extraction when the scan output is
    actually replaced (`IsPushdownExtract`) — otherwise they are field-PRUNING info and the expressions
    above re-extract.
  - `LATERAL fn(...) WITH ORDINALITY`: the ordinality modifier is re-emitted, and in-out passthrough
    columns (the child's own bindings, appended by `projected_input`) are recognized by their foreign
    table_index — also when the lateral sits inside a recursive CTE step (the delim source now inlines).
  - TABLE-argument functions (`summary((SELECT ...))`) embed the child as the argument instead of the
    comma-lateral form.
  - RANGE window frames with INTERVAL offsets: the offset is recovered from `<order key> ± <offset>`
    comparing modulo casts (DuckDB casts the order key inconsistently between ORDER BY and the frame).
  - Lambda parameters that would SHADOW a captured outer column after de-prefixing are renamed;
    `equi_width_bins` string arguments keep an explicit `::VARCHAR` (overload ambiguity).
  - Recursive CTE headers and self-reference reads quote reserved-word column names ("begin", "end").
- Large sweep of rendering bugs that made LPTS emit *invalid SQL* (queries FAILing the round-trip check
  with Binder/Parser errors instead of translating) — ~950 → ~50 across DuckDB's sqllogic corpus:
  - Dummy-scan bindings registered under a name (`t0_1`) that mismatched the CTE header (`t0_dummy`),
    breaking every uncorrelated constant IN/ANY/ALL.
  - Multi-condition mark joins without an equality (`(a,b) != ANY(...)`, `= ALL(...)`) rendered with
    per-row AND semantics; the nested-loop join actually matches each condition INDEPENDENTLY (an OR of
    existentials, with LHS-NULL → NULL taking precedence). New OR-form 3-valued mark rendering.
  - Unnamed-struct (ROW) constants and casts: `STRUCT(INTEGER, INTEGER)` has no SQL syntax. Constants are
    rebuilt structurally (`row(...)`/`struct_pack(...)`/`[...]`/`map(...)`); names-only / NULL-slot-typing
    casts render their child bare; other unnamed-struct or `AGGREGATE_STATE<...>` cast targets FAIL cleanly.
  - MAP constants (display form `{k=v}` is not SQL) now render as `CAST('<text>' AS MAP(...))`.
  - `date_part(['year',...], d)` (struct variant): the erased constant part-list argument is rebuilt from
    the STRUCT return type's field names.
  - Slice sentinels: omitted bounds (`s[:2]`, `s[2:]`, `array_pop_back/front`) render as bracket syntax,
    an omitted end with a step as the `-` end marker (`s[a:-:step]`), and the full-range step -1 slice as
    `list_reverse(...)`.
  - Quantile-family window functions and `list_aggr(l, 'quantile', p)` re-append the quantile argument
    from bind data (it is erased from the children at bind time).
  - Named-argument functions (`struct_insert`, `struct_update`, `write_log`) re-emit `name := expr`.
  - `EXPORT_STATE` aggregates re-emit `(<f>(args) EXPORT_STATE)` instead of the internal
    `aggregate_state_export_<f>` name.
  - `WHERE rowid = ...`: a table filter keyed by a virtual column id crashed (vector index -1); resolved
    via the virtual-columns map.
  - Struct-extraction pushdown over VARIANT columns navigates with bracket syntax (`v['field']`) instead
    of `struct_extract` (which walks the VARIANT's physical layout and fails).
  - `list_reduce(l, lambda, initial)`: the lambda is inserted at its declared argument position instead of
    appended after the trailing initial value; comprehension body references render under the canonical
    parameter name; non-identifier parameter names (`"x.y"`) are quoted.
  - Recursive CTE headers and self-reference scans de-duplicate column names; the flat-SQL de-prefixing
    pass judges uniqueness case-insensitively (DuckDB resolves identifiers ignoring case).
  - Deep recursive `UNNEST` keeps one UNNEST level per CTE (fusing levels nested UNNEST calls).
  - NUL bytes: string values render as `'part' || chr(0) || 'part'`; identifiers containing NUL FAIL
    cleanly (unrepresentable in SQL).
  Tests: `test/sql/fail_reduction.test`.
- Correlated `EXISTS` with a NULL correlation key returned the wrong result. The MARK→LEFT-join mark was
  `(rhs_key IS NOT NULL)`, which reads false when the correlation key is itself NULL yet legitimately
  matches (`NULL IS NOT DISTINCT FROM NULL`) — so `EXISTS` over a subquery that is true for the NULL outer
  row (e.g. an uncorrelated `i IS NULL` disjunct) wrongly returned false. The mark now tests a non-null
  match sentinel attached to the deduped RHS, so it reflects "did a row match" regardless of key nullability.
- Multi-column (row) `IN` subqueries lost three-valued logic. `(x,y) IN (SELECT a,b)` was rendered as a
  2-valued mark (returning FALSE where SQL requires NULL). The 3-valued mark (TRUE / FALSE / NULL) now
  generalizes to any number of membership comparisons: `matched = EXISTS(all equalities)`, `indeterminate =
  EXISTS(each comparison holds-or-has-a-NULL-operand)`. Single-column IN/ANY/ALL is unchanged.
- Scalar subqueries that can return more than one row are now recognized as nondeterministic instead of
  reported as WRONG in round-trip checking. With `scalar_subquery_error_on_multiple_rows=false` DuckDB
  returns an arbitrary row (by default it errors); LPTS's decorrelation cannot reproduce that arbitrary
  choice. `lpts_check` now scans the plan for a SINGLE join whose subquery side is not row-count-bounded
  (not an ungrouped aggregate or LIMIT) and excuses a *divergence* as nondeterministic — a genuinely
  single-row scalar subquery still matches and passes, so real translation bugs are not masked.
- Duplicate CTE column-name collisions. When two output columns of a CTE derived from source columns
  with the same name, LPTS emitted a header like `(t7_i, t7_i)`; DuckDB resolves later references to the
  first, silently dropping/duplicating the second column and corrupting results. Generated column names
  are now de-duplicated for UNION / EXCEPT / INTERSECT outputs and for a DELIM-GET's duplicate-eliminated
  correlation columns (fixes wrong results in UNION-with-`ORDER BY`-on-input-names, `EXCEPT ALL`,
  correlated scalar subqueries with an inner `UNION ALL`, correlated subqueries with a window, and
  `LATERAL` referencing two same-named bindings).
- `HAVING` without `GROUP BY` (e.g. `SELECT 1 FROM t HAVING 1<2`). The implicit single-group aggregate
  collapses the input to one row purely by being an aggregate; LPTS fused a constant projection over it,
  dropping the aggregation and emitting one row per input row. LPTS no longer fuses a projection onto an
  ungrouped aggregate when the projection (and HAVING) reference none of its aggregate outputs — the
  aggregate stays its own CTE so cardinality collapses correctly.
- `read_parquet` schema-affecting named arguments were dropped. `union_by_name` (unifies differing
  per-file schemas into a superset, widening column types) and an explicit `hive_partitioning=false`
  (suppresses auto-detection of path-derived partition columns) are now preserved in the rewrite; the
  positional `_tf(...)` alias is built from the bound (union) output positions, so they round-trip.
- DuckDB-suite coverage gate: correlated `COUNT(*)` scalar subqueries reported spurious `WRONG`s
  (`test/sql/subquery/scalar/test_count_star_subquery.test`, 6 → 0). Root cause was in the coverage
  *harness*, not LPTS translation: it activated LPTS by `LOAD`ing the self-contained
  `lpts.duckdb_extension` dylib, which embeds its own private copy of libduckdb. Because LPTS re-plans
  queries in-process (`PlanQuery` → `Planner::CreatePlan`), decorrelation ran against that second DuckDB
  copy, and DuckDB's count-bug rewrite (`CASE WHEN count_star() IS NULL THEN 0 ELSE count_star() END`) is
  gated on an `AggregateFunction` function-*pointer* comparison that fails across two copies — silently
  dropping the fix (NULL instead of 0). `scripts/run_duckdb_lpts_coverage.sh` now loads lpts as a
  statically-linked extension (host's single DuckDB copy) via `DUCKDB_TEST_STATICALLY_LOADED_EXTENSIONS`,
  matching how a real `duckdb` binary resolves the dylib. Committed baseline: `wrong=19 → 13`.
- `array_to_string(list, sep)` dropped its separator. DuckDB compiles it to `list_aggr(list, 'string_agg')`,
  and the separator is not a function argument — it lives in the nested `string_agg` aggregate inside the
  `list_aggregate` bind data (`ListAggregatesBindData::aggr_expr`). LPTS now recovers it from there and
  re-emits it as a trailing argument (`list_aggr(list, 'string_agg', '<sep>')`, which `list_aggregate`
  forwards to the sub-aggregate), so the joined string matches.
- Generated CTE column names could collide under DuckDB's case-insensitive identifier resolution. Two
  output columns differing only in case (`SELECT 1 "hello", 2 "HeLlO"`) produced names `t1_hello` /
  `t1_HeLlO`, which DuckDB treats as the same column — the second reference resolved to the first (wrong
  value, and broken `NATURAL JOIN`/`USING`). Column-name de-duplication is now case-insensitive. Separately,
  distinct non-ASCII column names that sanitize to the same ASCII fragment (e.g. different emoji all reduce
  to `_____`) no longer collide: a colliding plain scan column keeps its real name in the SELECT body but
  gets a distinct suffixed CTE identifier. (LPTS was not lowercasing identifiers — it preserves case; the
  bug was that the de-dup was case-sensitive while DuckDB is not.)

### Added

- `WITH ORDINALITY` now raises NotImplemented. It compiles to a distinctive `row_number() OVER (ROWS
  BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)` with no PARTITION BY or ORDER BY — the ordinality follows
  an unspecified scan order that the rewrite cannot reproduce. Detection is narrowed to that exact
  ROWS-frame signature, so a plain `row_number() OVER ()` (which DuckDB gives a RANGE frame) is unaffected.
- LPTS now raises NotImplemented (instead of silently emitting a divergent query) for two more constructs
  it cannot faithfully reproduce: a recursive CTE `USING KEY` and references to its recurring table
  (latest-row-per-key semantics, not plain recursion); and a lateral/correlated `GROUPING SETS` whose keys
  include an inlined constant (which would render as a bare integer in `GROUP BY` and be re-read as an
  ordinal position). Both are detected precisely, so ordinary recursive CTEs and grouping sets are
  unaffected.

### Fixed

- A filter on a hive partition column (e.g. `WHERE key = 'a'` over `parquet_scan('.../key=*/...')`) was
  silently dropped, so the rewrite read every partition instead of one. Such a filter is consumed by file
  pruning and removed from the structured plan; it survives only as the `file_filters` display string in
  the scan's `extra_info` (built from the predicate's `ToString()`). LPTS now re-applies that string as a
  scan filter (reading all files then filtering yields the same rows; the partition column is re-exposed by
  auto-detected hive partitioning). Only a single, well-formed parenthesized predicate is re-applied —
  multiple pruned filters are concatenated without a separator, so those are left for the round-trip check.
- A typed `NULL` passed as a table-function argument lost its type: `test_vector_types(NULL::INT[])`
  rendered as `test_vector_types(NULL)`, which selects different data. Table-function parameters now render
  through the constant renderer, so a typed NULL becomes `CAST(NULL AS INTEGER[])` (and other lossy-typed
  parameters are cast too); ordinary parameters are unchanged.
- Integer/BIT constants lost their type on the round-trip in two spots. (1) `INTEGER`'s minimum value —
  the bare literal `-2147483648` re-parses as `BIGINT` (since `2147483648` overflows `INT32` before the
  negation), so LPTS now casts it explicitly (`CAST('-2147483648' AS INTEGER)`); ordinary integers stay
  bare. This fixed the decimal-string, exponent-string, `BIT::INT`, and multiplication-overflow casts that
  all fold to `INT_MIN`. (2) A `BIT` constant in a pushed-down filter (`WHERE b = '111'`) rendered via
  DuckDB's `TableFilter::ToString` as the bare `b = 111`, which re-parses as the integer `111` and matches
  nothing; constant-comparison filters whose constant needs a type-preserving cast now render through the
  expression path (`CAST('111' AS BIT)`), while ordinary filter constants keep the compact form.
- Lambdas that capture an outer column were rendered incorrectly. `list_transform(l, x -> x + n)` (where
  `n` is an outer column) became `lambda n, x: x + n` — the captured column was emitted as a lambda
  *parameter*, shadowing the outer reference. DuckDB binds captures as trailing lambda parameters; LPTS now
  lists only the real parameters and renders each capture as its outer expression (so column renaming
  applies — e.g. a captured `range` becomes `t0_range`). Fixes correlated lambdas in `list_transform`/
  `list_filter` (arrow and `lambda` syntax), including captured positional column references (`#1`).
- Scalar subqueries (`SINGLE` joins) could multiply the outer row. A scalar subquery must yield at most one
  row per outer row, but LPTS converted the `SINGLE` join to a plain `LEFT` join without collapsing the
  right side — a decorrelated non-aggregated subquery over a multi-row table (e.g.
  `(SELECT 42 + i1.i FROM integers)`) then matched several right rows and duplicated the outer row. The
  `SINGLE` join now deduplicates its right side (`LEFT JOIN (SELECT DISTINCT * FROM rhs)`), matching the
  one-row-per-outer semantics. Fixed correlated scalar subqueries (plain, through a CTE, in a join
  condition) and correlated subqueries alongside a SEMI join.
- `CUBE`/`ROLLUP`/`GROUPING SETS` over equal join keys produced wrong results and now raise NotImplemented.
  DuckDB's duplicate-group optimizer collapses equal grouping keys (e.g. `col3` = `col1` from a join
  condition) to the same expression, so two grouping dimensions render to the same column name. SQL
  grouping sets reference columns by name and cannot express the two collapsed dimensions independently
  (a set containing one but not the other, and the super-aggregate NULLs, land wrong), so LPTS refuses
  rather than emit a plausible-but-wrong result. Plain `GROUP BY` over the same keys is unaffected.
- Descending and negative quantiles lost their direction on the round-trip. `percentile_cont/disc(p)
  WITHIN GROUP (ORDER BY x DESC)` and a negative quantile argument (`quantile_disc(x, -0.5)`, which DuckDB
  normalizes to quantile `0.5` plus a `desc` flag) were both rendered as a bare `quantile_cont(x, p)`,
  computing the quantile from the wrong end. LPTS now reads the `desc` flag from the quantile bind data and
  emits the in-aggregate ordering form `quantile_cont(x, p ORDER BY x DESC)`. Fixed ~11 wrong results
  across the ordered-aggregate and quantile tests.
- The multi-file reader's path/reader-derived virtual columns — `filename`, `file_row_number`,
  `file_index` — were emitted as a plain `_tf(...)` column alias on the rewritten scan, which aliased a
  real data column under the virtual name and read the wrong values. LPTS now raises NotImplemented for a
  projected `filename`/`file_row_number`/`file_index` (matched by the multi-file reader's column
  identifiers), since they are derived from the file path and scan position and cannot be reproduced as a
  static read. (Hive partition columns are left as-is: a re-read auto-detects the same partitioning and
  usually round-trips, so failing them would lose far more coverage than it gains.)
- Folded `ENUM` constants lost their type on the round-trip: `'happy'::mood` (and the `ENUM(...)` that
  `union_tag(...)` returns) rendered as the bare string `'happy'`, which re-parses as `VARCHAR`, so the
  type-sensitive result comparison diverged. ENUM constants now render as `CAST('<value>' AS ENUM(...))`
  (the type's full member list is inlined), matching the other lossy-constant types. Fixed ~21 wrong
  results across the enum and `union_tag`/`union_cast` tests.
- Struct field-extraction pushdown was dropped on the round-trip. When DuckDB pushes a struct field
  access (`s.info.a`) into the scan, the projected column carries a sub-field path, but LPTS rendered only
  the top-level column — so `SELECT s.info.a` became `SELECT info` (the whole struct, wrong value and type)
  and `SELECT s.info.a, s.info.b` collapsed to a duplicated `info`. The scan now emits a nested
  `struct_extract(...)` expression for each pushed-down field path (resolving field names from the column's
  STRUCT type), aliased to the leaf field name and de-duplicated when the same field is projected twice.
  Scan columns gained a per-column "is expression" flag so these are emitted verbatim rather than quoted as
  identifiers, across the plain-scan, pipeline-merge, and recursive-CTE-inline renderers. A pushdown extract
  that also carries a cast/restructure on the extracted value (`s.info.a::BIGINT`, or
  `s.info::STRUCT(b BOOL, a INT)` which reorders fields) is wrapped in an explicit `CAST(... AS <type>)` so
  the type and field order round-trip. The few cases LPTS still cannot reproduce now raise NotImplemented
  (rather than silently returning the wrong column): a field path through a non-STRUCT level, and
  field-extraction pushdown over a table function (`read_parquet`/`read_csv`), whose base column is only
  reachable through the `_tf(...)` alias. This fixed ~130 wrong results across the struct/nested-struct
  projection-pushdown and storage tests.
- Correlated/uncorrelated `IN`/`ANY`/`ALL` (MARK joins) were rendered as a `LEFT JOIN` with a 2-valued
  match test, which (a) multiplied the left row when an inequality matched several right rows and (b) lost
  the 3-valued (NULL/indeterminate) semantics — e.g. `x IN (S)` with a NULL in `S`, or `i <= ALL(S)` with a
  NULL, read FALSE instead of NULL. Single-condition, NULL-propagating mark joins now render as a
  correlated `EXISTS` producing exactly one row per left row with a 3-valued mark (TRUE / NULL when the
  comparison is indeterminate / FALSE). This also covers *correlated* ANY/ALL whose mark join carries the
  null-safe correlation keys (`IS NOT DISTINCT FROM`) alongside a single membership comparison — the
  correlation keys gate which rows count toward the indeterminate (NULL) case. The same 3-valued mark now
  applies on the DELIM/DEPENDENT join path (`LOGICAL_DELIM_JOIN`/`LOGICAL_DEPENDENT_JOIN`), so a correlated
  ALL/ANY whose subquery returns a projected expression (e.g. `i > ALL(SELECT (i + i1.i - 1) / 2 ...)`) is
  decorrelated correctly instead of using the old 2-valued match. `EXISTS` subqueries (genuinely 2-valued)
  and multi-comparison mark joins keep the prior rendering.
- A `DISTINCT` whose dedup key is a proper subset of the output columns was rewritten as plain
  `SELECT DISTINCT` over all columns, returning too many rows. This covers both `DISTINCT ON
  (targets) ... ORDER BY o` and `SELECT DISTINCT a ... ORDER BY b` (DuckDB dedups on `a` but
  carries `b` for the ORDER BY). It now rewrites to a portable
  `row_number() OVER (PARTITION BY targets [ORDER BY o]) = 1` filter.
- Table functions (`read_csv`, `read_parquet`, ...) dropped their named options, so the rewrite
  read the source differently (e.g. without `header=0` the first row became a header). Named
  parameters are now reproduced as `name = <literal>` (`union_by_name`/`hive_types` are left to
  LPTS's own column projection).
- Folded constants of non-default types lost their type on the round-trip (a `BIT` literal
  re-parsed as an integer; `HUGEINT`/`FLOAT`/`DOUBLE`/`DECIMAL`/`TINYINT`/... as `INTEGER`/`DOUBLE`;
  a `DOUBLE[]` as `DECIMAL[]`; a typed `NULL` as the untyped SQLNULL), so the type-sensitive result
  comparison diverged. Constants now render via `Value::ToSQLString()` and, for the lossy scalar and
  nested (`LIST`/`ARRAY`/`STRUCT`) types and typed NULLs, an explicit `CAST(... AS <type>)`.
- Table functions read by path (`SELECT * FROM 'x.parquet'`) could scramble or fail: the positional
  `_tf(...)` column alias used LPTS's projected column order, but the function emits columns in file
  order, so projection pushdown (e.g. a `WHERE` on a non-first column) mis-mapped every name to the
  wrong physical column. The `_tf` alias is now emitted in the function's output order, so the columns
  map correctly regardless of projection reordering/subsetting.
- More queries are recognized as nondeterministic: `LIMIT`/`OFFSET`/`FETCH` without a total ordering
  (the chosen subset is unspecified), and floating aggregates with an in-argument `ORDER BY`
  (`sum(x ORDER BY y)` — summation order affects the result).
- Pushed-down scan filters lost operator grouping: a `CONJUNCTION_AND`/`CONJUNCTION_OR` table filter
  joined its children with bare `AND`/`OR`, so a nested `a AND (b OR c)` rendered as `a AND b OR c`
  (which precedence re-reads as `(a AND b) OR c`). Each child is now parenthesized.
- More queries are recognized as nondeterministic (and thus not flagged as round-trip failures):
  row sampling (`USING SAMPLE`/`TABLESAMPLE`), approximate aggregates, UUID generators, `stats()`,
  sequence access (`nextval`/`currval`), and wall-clock/transaction time functions.

### Added

- LPTS now raises a clear NotImplemented error (instead of silently producing a divergent query) for
  constructs it cannot faithfully reproduce as static SQL: `VARIANT` values (including nested in
  list/struct/map), the meta/dynamic table functions `query()`/`query_table()`/`sniff_csv()`,
  `read_*` calls with path-derived columns (`hive_partitioning`/`filename`), and `alias(expr)` (it
  reflects a column name that LPTS rewrites, so its string result cannot match the original).

### Changed

- Bumped the DuckDB target from `v1.5.3` to `v1.5.4` (submodule and CI). The 1.5.4
  optimizer pushes trivial projections/filters into the scan, so single-table
  `EXPLAIN (FORMAT SQL)` plans may collapse to a plain `SELECT` with no CTE; round-trip
  correctness is unchanged.

### Added

- `EXPLAIN (FORMAT SQL) <query>` renders the optimized logical plan as equivalent CTE SQL
  (the same output as `PRAGMA lpts`), exposed as a genuine `EXPLAIN` statement. The CLI
  prints it as plain multi-line text and other clients (JDBC, Python, ...) receive the
  standard two-column `explain_key`/`explain_value` EXPLAIN result, similar to Umbra's
  plan-as-SQL output. Honors `lpts_dialect`. Implemented without changes to DuckDB core.

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
