PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=lpts
EXT_CONFIG=${PROJ_DIR}extension_config.cmake
OVERRIDE_GIT_DESCRIBE?=v2.0.0

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# ------------------------------------------------------------------------------------------------
# DuckDB sqllogic corpus through LPTS (round-trip coverage gate).
#
# Runs every duckdb/test/sql/**.test file with `SET lpts_check = true` (LOG mode) and diffs the
# per-query verdicts against the committed baseline (test/duckdb_lpts_baseline.txt). The gate FAILS on:
#   - any NEW `WRONG`  (LPTS rewrote the query, the result bag differed), or
#   - any NEW `FAIL`   (LPTS could not rewrite AND the error was not an LPTS_<CODE>-formatted
#                       "not supported" refusal — i.e. LPTS emitted SQL that failed to parse/bind:
#                       a translation bug).
# An `UNSUPPORTED` verdict (a deliberate LPTS_UNSUPPORTED_* refusal) is acceptable and not gated.
#
# The full run takes a few minutes (file-level parallelism, JOBS=<n> to tune).
# After intentionally fixing/adding coverage, refresh the baseline with `make coverage-baseline`.
# ------------------------------------------------------------------------------------------------
.PHONY: coverage-check coverage-baseline
coverage-check: release
	build/release/extension/lpts/lpts_corpus_gate --check

coverage-baseline: release
	build/release/extension/lpts/lpts_corpus_gate --update-baseline

# `make test` runs SQLStorm regressions, unit tests, then the DuckDB corpus gate.
test: coverage-check

# Keep the exact SQLStorm regressions in their upstream corpus rather than
# duplicating hundreds of lines of SQL in test/sql/cte.test.
ifneq ($(OS),Windows_NT)
.PHONY: sqlstorm-regressions
sqlstorm-regressions: release
	build/release/extension/lpts/lpts_sqlstorm_benchmark --tpch_sf 0.001 --timeout 10 \
		--cases 32742,33660,1137,20918,20200,21010,21797,4020,7445,30619 \
		--require-success --out /tmp/lpts-sqlstorm-regressions.csv

test_release: sqlstorm-regressions
endif
