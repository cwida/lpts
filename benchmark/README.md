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

The runner executes the SQLStorm TPC-H query set through `PRAGMA lpts_check`.
It reports success, DuckDB errors, LPTS errors, unsupported cases, timeouts, and
incorrect results.

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
