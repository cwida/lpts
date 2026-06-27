#!/usr/bin/env bash
#
# Run DuckDB's own sqllogic .test corpus through LPTS (release build) in parallel and produce a per-file
# coverage report; optionally gate it against a committed baseline.
#
# Mechanism: each .test file runs in its OWN `unittest` process with `SET lpts_check=true` and the
# LPTS_CHECK_LOG environment variable set. The presence of LPTS_CHECK_LOG puts lpts_check into LOG mode:
# it NEVER raises (so the file's own expected outputs are unaffected — the DuckDB test passes/fails on its
# own merits) and instead appends one line per intercepted top-level SELECT:
#
#     <query_number> OK                          LPTS rewrote it and the result bag matched
#     <query_number> WRONG                       LPTS rewrote it but the result bag differed
#     <query_number> FAIL                        LPTS could not rewrite it (unsupported feature)
#     <query_number> NONDETERMINISTIC: <reason>  LPTS rewrote it but the query is nondeterministic
#
# The driver collapses each file's log into a single, diff-friendly summary line:
#
#     <relpath> ok=<n> wrong=<n> fail=<n> ndet=<n> [WRONG[i,j,...]]
#
# WRONG is the actionable correctness signal (LPTS produced different results); FAIL is expected for
# features LPTS does not yet rewrite; NONDETERMINISTIC is suppressed. (LOG mode also neutralizes
# `PRAGMA enable_verification` on the connection so verification-heavy files — ~55% of the corpus — are
# still checked; see LptsCheckOptimize in src/lpts_extension.cpp.)
#
# Usage:
#   scripts/run_duckdb_lpts_coverage.sh [SUBDIR] [OUT]            # write report (default SUBDIR=test/sql, OUT=stdout)
#   scripts/run_duckdb_lpts_coverage.sh --update-baseline [SUBDIR]  # (re)write test/duckdb_lpts_baseline.txt
#   scripts/run_duckdb_lpts_coverage.sh --check [SUBDIR]          # run + diff vs baseline; exit 1 on a NEW WRONG
#
# Env: JOBS (parallelism, default = CPU count), PER_FILE_TIMEOUT (seconds, default 120).
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EXT="$ROOT/build/release/extension/lpts/lpts.duckdb_extension"
UNITTEST="$ROOT/build/release/test/unittest"
BASELINE="$ROOT/test/duckdb_lpts_baseline.txt"
PER_FILE_TIMEOUT="${PER_FILE_TIMEOUT:-120}"

# ------------------------------------------------------------------ per-file worker (re-exec of this script)
if [ "${1:-}" = "--worker" ]; then
	full="$2"
	rel="${full#duckdb/}" # path relative to the --test-dir root (`duckdb`)
	safe="$(printf '%s' "$full" | tr '/' '_')"
	log="$LPTS_COV_WORKDIR/$safe.log"
	: >"$log"
	LPTS_CHECK_LOG="$log" \
		DUCKDB_TEST_ON_NEW_CONNECTION="LOAD '$EXT'; SET lpts_check=true; PRAGMA threads=1" \
		timeout "$PER_FILE_TIMEOUT" "$UNITTEST" --test-dir duckdb "$rel" >/dev/null 2>&1
	# grep -c always prints a count (0 if none); set -u tolerates its nonzero exit on no-match.
	ok=$(grep -c ' OK$' "$log" 2>/dev/null)
	wrong=$(grep -c ' WRONG$' "$log" 2>/dev/null)
	fail=$(grep -c ' FAIL$' "$log" 2>/dev/null)
	ndet=$(grep -c ' NONDETERMINISTIC' "$log" 2>/dev/null)
	line="$full ok=$ok wrong=$wrong fail=$fail ndet=$ndet"
	if [ "$wrong" -gt 0 ]; then
		wlist=$(awk '$2=="WRONG"{printf "%s,",$1}' "$log" | sed 's/,$//')
		line="$line WRONG[$wlist]"
	fi
	printf '%s\n' "$line" >"$LPTS_COV_WORKDIR/$safe.sum"
	exit 0
fi

# ------------------------------------------------------------------ main
MODE="run"
SUBDIR="test/sql"
OUT="/dev/stdout"
case "${1:-}" in
--check) MODE="check"; SUBDIR="${2:-test/sql}" ;;
--update-baseline) MODE="baseline"; SUBDIR="${2:-test/sql}" ;;
"") ;;
*) SUBDIR="${1}"; OUT="${2:-/dev/stdout}" ;;
esac
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}"

if [ ! -x "$UNITTEST" ] || [ ! -f "$EXT" ]; then
	echo "build first: GEN=ninja make (need $UNITTEST and $EXT)" >&2
	exit 1
fi

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT
export LPTS_COV_WORKDIR="$WORKDIR" EXT UNITTEST PER_FILE_TIMEOUT

cd "$ROOT"
nfiles=$(find "duckdb/$SUBDIR" -name '*.test' | wc -l | tr -d ' ')
echo "running $nfiles file(s) under duckdb/$SUBDIR through LPTS (JOBS=$JOBS) ..." >&2
find "duckdb/$SUBDIR" -name '*.test' | sort | xargs -P "$JOBS" -n1 "$0" --worker

REPORT="$WORKDIR/report.txt"
{
	cat "$WORKDIR"/*.sum 2>/dev/null | sort |
		awk '{lines[NR]=$0
		      for(i=1;i<=NF;i++){
		        if($i~/^ok=/)o+=substr($i,4); if($i~/^wrong=/)w+=substr($i,7)
		        if($i~/^fail=/)f+=substr($i,6); if($i~/^ndet=/)n+=substr($i,6)}}
		     END{printf "# totals: files=%d ok=%d wrong=%d fail=%d ndet=%d\n",NR,o,w,f,n
		         for(k=1;k<=NR;k++)print lines[k]}'
} >"$REPORT"

case "$MODE" in
baseline)
	# Keep the totals header + only files LPTS actually engaged (any ok/wrong/fail/ndet > 0); drop the
	# fully-skipped/empty files so the committed baseline stays signal. The gate treats a file absent from
	# the baseline as "had no WRONG", so omitting zero-activity files never hides a future regression.
	awk 'NR==1 && /^# totals:/ {print; next}
	     /ok=0 wrong=0 fail=0 ndet=0$/ {next}
	     {print}' "$REPORT" >"$BASELINE"
	echo "wrote baseline: $BASELINE ($(grep -cv '^#' "$BASELINE") files with activity)" >&2
	grep '^# totals:' "$BASELINE" >&2
	;;
check)
	if [ ! -f "$BASELINE" ]; then
		echo "no baseline at $BASELINE; run --update-baseline first" >&2
		exit 2
	fi
	python3 - "$BASELINE" "$REPORT" <<'PY'
import sys, re
def load(p):
    d = {}
    for ln in open(p):
        ln = ln.strip()
        if not ln or ln.startswith('#'):
            continue
        rel = ln.split(' ', 1)[0]
        m = re.search(r'wrong=(\d+)', ln)
        w = int(m.group(1)) if m else 0
        mi = re.search(r'WRONG\[([^\]]*)\]', ln)
        idx = set(filter(None, (mi.group(1).split(',') if mi else [])))
        d[rel] = (w, idx)
    return d
base, cur = load(sys.argv[1]), load(sys.argv[2])
regr, improved = [], 0
for rel, (w, idx) in sorted(cur.items()):
    bw, bidx = base.get(rel, (0, set()))
    new = idx - bidx
    if w > bw or (rel not in base and w > 0) or new:
        regr.append((rel, sorted(new) or sorted(idx)))
    if w < bw:
        improved += 1
if improved:
    print(f"note: {improved} file(s) have FEWER WRONGs than baseline (improvement — refresh the baseline)")
if regr:
    print(f"REGRESSION: {len(regr)} file(s) gained WRONG queries vs baseline:")
    for rel, ix in regr:
        print(f"  {rel}  new WRONG at query #{ix}")
    sys.exit(1)
print("OK: no new WRONG vs baseline")
PY
	;;
*)
	cat "$REPORT" >"$OUT"
	;;
esac
echo "done." >&2
