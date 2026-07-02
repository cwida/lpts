# Testing LPTS

Tests are DuckDB SQLLogicTests under `test/sql/`.

## Run All Tests

```bash
GEN=ninja make unittest   # LPTS's own tests (test/sql/*, fast — use during iteration)
GEN=ninja make test       # unittest + the DuckDB-corpus coverage gate (~4 min — run before pushing)
```

(`build/release/test/unittest "test/sql/*"` is the underlying command for the unit tests.)

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
and instead logs LPTS's verdict per intercepted SELECT (`OK` / `WRONG` / `UNSUPPORTED` / `FAIL` /
`NONDETERMINISTIC`).
In LOG mode LPTS also neutralizes `PRAGMA enable_verification` (which ~55% of files self-enable) so those
files are still checked.

LPTS is loaded as a **statically-linked** extension (`DUCKDB_TEST_STATICALLY_LOADED_EXTENSIONS='[core_functions, lpts]'`),
*not* by `LOAD`ing the self-contained `lpts.duckdb_extension` dylib. This is a correctness requirement, not a
convenience: the loadable dylib embeds its own private copy of libduckdb, and because LPTS re-plans queries
*inside* the process (`PlanQuery` → `Planner::CreatePlan`), decorrelation would run against that second DuckDB
copy. DuckDB compares `AggregateFunction`s by function *pointer* (e.g. the count-bug `CASE WHEN count IS NULL
THEN 0` rewrite), which silently fails across two copies and yields spurious `WRONG`s. The statically-linked
extension shares the host's single DuckDB copy, matching how a real `duckdb` binary resolves the loaded dylib.

The driver `scripts/run_duckdb_lpts_coverage.sh` parallelizes this across files (the `unittest` binary
itself is single-threaded; parallelism is file-level sharding via `xargs -P`, the way DuckDB CI
parallelizes) and collapses each file's log into one diff-friendly summary line:

```
duckdb/test/sql/... ok=<n> wrong=<n> fail=<n> unsupported=<n> ndet=<n> [WRONG[i,...]] [FAIL[i,...]]
```

Two verdicts are gated failures:

- `WRONG` — LPTS rewrote the query but produced a different result bag (a wrong translation).
- `FAIL` — LPTS could not rewrite the query and the error was **not** an `LPTS_<CODE>`-formatted
  refusal, i.e. LPTS emitted SQL that failed to parse/bind (a translation bug).

An `UNSUPPORTED` verdict is acceptable: it is a deliberate, coded "not supported" refusal — every such
refusal is raised through `ThrowLptsNotImplemented` (or an equivalent `LPTS_<CODE>: ...` message), and
the check classifies anything else as `FAIL`. `NONDETERMINISTIC` is suppressed.

```bash
GEN=ninja make test          # unit tests + the corpus coverage gate (the standard way to run it)
make coverage-check          # just the gate (exit non-zero on any new WRONG / FAIL)
make coverage-baseline       # (re)write test/duckdb_lpts_baseline.txt after intentional changes
JOBS=12 scripts/run_duckdb_lpts_coverage.sh   # print the full per-file report to stdout
```

`test/duckdb_lpts_baseline.txt` is the committed regression floor (currently
`wrong=0 fail=0` — every non-rewritable query is a deliberate UNSUPPORTED refusal). `--check` re-runs
the suite and fails when a query gained a WRONG or FAIL verdict vs the baseline. Changes in
`UNSUPPORTED`/coverage are reported but non-fatal; when you *fix* something, refresh the baseline with
`make coverage-baseline`. The full run takes a few minutes (`JOBS=<n>` to tune). Investigate any new
gated verdict the same way as a SQLStorm `incorrect`: a genuine LPTS rewrite bug, or a nondeterminism
the `IsLikelyNondeterministicSQL` heuristic missed.

## Notes

- Prefer small tables and focused queries.
- Use `rowsort` when row order is not part of the behavior.
- Use explicit `ORDER BY` when order is part of the behavior.
- Do not remove a failing test to make the suite pass.
