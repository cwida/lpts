PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=lpts
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

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
	bash scripts/run_duckdb_lpts_coverage.sh --check

coverage-baseline: release
	bash scripts/run_duckdb_lpts_coverage.sh --update-baseline

# `make test` = the extension's own unit tests, then the corpus coverage gate.
test: coverage-check