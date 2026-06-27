# SQLStorm Benchmark

This folder contains the LPTS SQLStorm runner and the upstream SQLStorm query
corpus. The upstream SQLStorm project README is in
`benchmark/sqlstorm/SQLStorm/README.md`.

## SQLStorm

Build LPTS first:

```bash
GEN=ninja make
```

Run the benchmark:

```bash
build/release/extension/lpts/lpts_sqlstorm_benchmark --tpch_sf 0.001 --timeout 10
```

The runner re-runs each SQLStorm TPC-H query under `SET lpts_check = true` to
verify round-trip correctness. It reports success, DuckDB errors, LPTS errors,
unsupported cases, timeouts, and incorrect results.

Useful options:

| Option | Description |
|---|---|
| `--queries <dir>` | Override the SQLStorm TPC-H query directory. |
| `--out <csv>` | Write results to a specific CSV path. |
| `--timeout <sec>` | Set the per-query timeout. |
| `--tpch_sf <float>` | Set the generated TPC-H scale factor. |
| `--compare_perf` | Compare original DuckDB execution time against generated SQL execution time. |

Run SQLStorm only when a feature is complete or before pushing. A regression in
the number of successful queries needs investigation before the change is ready.

## DuckDB sqllogic suite

A second, broader correctness sweep runs DuckDB's own sqllogic corpus
(`duckdb/test/sql/**`) through LPTS in parallel and gates against a committed
baseline. See [test.md](test.md#duckdb-suite-coverage-regression-gate) and
`scripts/run_duckdb_lpts_coverage.sh`.
