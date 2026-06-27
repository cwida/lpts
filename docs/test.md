# Testing LPTS

Tests are DuckDB SQLLogicTests under `test/sql/`.

## Run All Tests

```bash
GEN=ninja make
build/release/test/unittest
```

## Run One Test File

```bash
build/release/test/unittest "test/sql/select.test"
build/release/test/unittest "test/sql/tpch.test"
```

## Round-Trip Rule

Every feature test needs a round-trip check. Set the check mode once, right after
`require lpts`, then run the query directly:

```sql
require lpts

statement ok
SET lpts_check = true;

query III rowsort
SELECT * FROM users;
----
1	Alice	30
```

With `lpts_check` on, every bare `SELECT` is transparently rewritten by LPTS and
compared against the original; a wrong rewrite raises an error and fails the test.
When you do not need to assert rows, use `statement ok` followed by the bare query.

A query LPTS rewrites incorrectly is a `statement error` whose expected text
contains `LPTS check failed`.

Use `lpts_query('<query>')` only when the exact generated SQL is the behavior
under test — for example input-dialect tests that cannot run as bare DuckDB
statements assert through `SELECT sql FROM lpts_query('...')` instead.

## Real Query Checks

Before marking a feature done, test it with real queries in the DuckDB shell:

```sql
INSTALL lpts FROM community;
LOAD lpts;

CREATE TABLE users (id INTEGER, name VARCHAR, age INTEGER);
INSERT INTO users VALUES (1, 'Alice', 30), (2, 'Bob', 22);

SET lpts_check = true;

PRAGMA lpts('SELECT name FROM users WHERE age > 25');
SELECT name FROM users WHERE age > 25;   -- runs normally; raises if rewritten wrong
```

## DuckDB suite coverage (regression gate)

LPTS is also exercised against DuckDB's *own* sqllogic corpus (`duckdb/test/sql/**`, ~3300 files). Each
file runs in its own release `unittest` process with `SET lpts_check=true` and the `LPTS_CHECK_LOG`
environment variable set — LOG mode, which never raises (so the DuckDB test's own outcome is unchanged)
and instead logs LPTS's verdict per intercepted SELECT (`OK` / `WRONG` / `FAIL` / `NONDETERMINISTIC`). In
LOG mode LPTS also neutralizes `PRAGMA enable_verification` (which ~55% of files self-enable) so those
files are still checked.

The driver `scripts/run_duckdb_lpts_coverage.sh` parallelizes this across files (the `unittest` binary
itself is single-threaded; parallelism is file-level sharding via `xargs -P`, the way DuckDB CI
parallelizes) and collapses each file's log into one diff-friendly summary line:

```
duckdb/test/sql/... ok=<n> wrong=<n> fail=<n> ndet=<n> [WRONG[i,j,...]]
```

`WRONG` is the actionable signal — LPTS rewrote the query but produced a different result bag. `FAIL`
means LPTS cannot yet rewrite that query (expected for unsupported features); `NONDETERMINISTIC` is
suppressed.

```bash
GEN=ninja make                                              # release build first
JOBS=12 scripts/run_duckdb_lpts_coverage.sh                 # print the full report to stdout
JOBS=12 scripts/run_duckdb_lpts_coverage.sh --update-baseline   # (re)write test/duckdb_lpts_baseline.txt
JOBS=12 scripts/run_duckdb_lpts_coverage.sh --check             # exit non-zero if any file gained a WRONG
```

`test/duckdb_lpts_baseline.txt` is the committed regression floor. `--check` re-runs the suite and fails
only when a query that previously matched (or a new query) now returns a **different** result — a real
LPTS correctness regression. Changes in `FAIL`/coverage are reported but non-fatal; when you *fix* a
WRONG, refresh the baseline with `--update-baseline`. The run takes a few minutes, so wire it into CI as a
separate on-demand/nightly job — it is not part of the fast `make unittest`. Investigate any *new* `WRONG`
the same way as a SQLStorm `incorrect`: a genuine LPTS rewrite bug, or a nondeterminism the
`IsLikelyNondeterministicSQL` heuristic missed.

## Notes

- Prefer small tables and focused queries.
- Use `rowsort` when row order is not part of the behavior.
- Use explicit `ORDER BY` when order is part of the behavior.
- Do not remove a failing test to make the suite pass.
