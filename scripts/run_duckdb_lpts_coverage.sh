#!/usr/bin/env bash
#
# Run DuckDB's own sqllogic .test corpus through LPTS (release build) in parallel and produce a per-file
# coverage report; optionally gate it against a committed baseline.
#
# Mechanism: each .test file runs in its OWN `unittest` process with `SET lpts_check=true` and the
# LPTS_CHECK_LOG environment variable set. LPTS is loaded as a STATICALLY-LINKED extension (via the test
# runner's `statically_loaded_extensions` config), NOT by `LOAD`ing the self-contained lpts.duckdb_extension
# dylib. This matters: the loadable dylib embeds its own private copy of libduckdb, so when LPTS re-plans a
# query inside the unittest process (PlanQuery -> Planner::CreatePlan), decorrelation runs against that
# second DuckDB copy. DuckDB's count-bug fix (and other logic) compares AggregateFunction by *function
# pointer*, which fails across two copies — silently dropping `CASE WHEN count IS NULL THEN 0` and producing
# spurious WRONGs. The statically-linked extension shares the host's single DuckDB copy, so pointer identity
# holds and the check reflects real product behavior.
#
# The presence of LPTS_CHECK_LOG puts lpts_check into LOG mode:
# it NEVER raises (so the file's own expected outputs are unaffected — the DuckDB test passes/fails on its
# own merits) and instead appends one line per intercepted top-level SELECT:
#
#     <query_number> OK                          LPTS rewrote it and the result bag matched
#     <query_number> WRONG                       LPTS rewrote it but the result bag differed
#     <query_number> UNSUPPORTED                 deliberate LPTS_<CODE>-formatted "not supported" refusal
#     <query_number> FAIL                        LPTS could not rewrite AND the error was NOT an
#                                                LPTS_<CODE> refusal — emitted SQL failed to parse/bind
#     <query_number> NONDETERMINISTIC: <reason>  LPTS rewrote it but the query is nondeterministic
#
# The driver collapses each file's log into a single, diff-friendly summary line:
#
#     <relpath> ok=<n> wrong=<n> fail=<n> unsupported=<n> ndet=<n> [WRONG[i,...]] [FAIL[i,...]]
#
# WRONG and FAIL are the gated correctness signals (a wrong translation, or invalid SQL);
# UNSUPPORTED is an acceptable, deliberate refusal; NONDETERMINISTIC is suppressed. (LOG mode also neutralizes
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
	# Load lpts as a statically-linked extension (host's DuckDB copy) rather than LOADing the self-contained
	# dylib — see the header comment. core_functions must stay in the list (it is the runner default).
	LPTS_CHECK_LOG="$log" \
		DUCKDB_TEST_STATICALLY_LOADED_EXTENSIONS='[core_functions, lpts]' \
		DUCKDB_TEST_ON_NEW_CONNECTION="SET lpts_check=true; PRAGMA threads=1" \
		timeout "$PER_FILE_TIMEOUT" "$UNITTEST" --test-dir duckdb "$rel" >/dev/null 2>&1
	# grep -c always prints a count (0 if none); set -u tolerates its nonzero exit on no-match.
	ok=$(grep -c ' OK$' "$log" 2>/dev/null)
	wrong=$(grep -c ' WRONG$' "$log" 2>/dev/null)
	# UNSUPPORTED: a deliberate LPTS_<CODE>-formatted refusal — acceptable, not gated.
	unsupported=$(grep -c ' UNSUPPORTED$' "$log" 2>/dev/null)
	# FAIL: LPTS could not rewrite the query AND the error was not an LPTS_<CODE>-formatted refusal —
	# i.e. LPTS emitted SQL that failed to parse/bind. A translation bug: gated like WRONG.
	fail=$(grep -c ' FAIL$' "$log" 2>/dev/null)
	ndet=$(grep -c ' NONDETERMINISTIC' "$log" 2>/dev/null)
	line="$full ok=$ok wrong=$wrong fail=$fail unsupported=$unsupported ndet=$ndet"
	if [ "$wrong" -gt 0 ]; then
		wlist=$(awk '$2=="WRONG"{printf "%s,",$1}' "$log" | sed 's/,$//')
		line="$line WRONG[$wlist]"
	fi
	if [ "$fail" -gt 0 ]; then
		flist=$(awk '$2=="FAIL"{printf "%s,",$1}' "$log" | sed 's/,$//')
		line="$line FAIL[$flist]"
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
		        if($i~/^fail=/)f+=substr($i,6); if($i~/^unsupported=/)u+=substr($i,13)
		        if($i~/^ndet=/)n+=substr($i,6)}}
		     END{printf "# totals: files=%d ok=%d wrong=%d fail=%d unsupported=%d ndet=%d\n",NR,o,w,f,u,n
		         for(k=1;k<=NR;k++)print lines[k]}'
} >"$REPORT"

case "$MODE" in
baseline)
	# Keep the totals header + only files LPTS actually engaged (any ok/wrong/fail/ndet > 0); drop the
	# fully-skipped/empty files so the committed baseline stays signal. The gate treats a file absent from
	# the baseline as "had no WRONG", so omitting zero-activity files never hides a future regression.
	awk 'NR==1 && /^# totals:/ {print; next}
	     /ok=0 wrong=0 fail=0 unsupported=0 ndet=0$/ {next}
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
        def num(field):
            m = re.search(field + r'=(\d+)', ln)
            return int(m.group(1)) if m else 0
        def idxs(tag):
            mi = re.search(tag + r'\[([^\]]*)\]', ln)
            return set(filter(None, (mi.group(1).split(',') if mi else [])))
        d[rel] = (num('wrong'), idxs('WRONG'), num('fail'), idxs('FAIL'))
    return d
base, cur = load(sys.argv[1]), load(sys.argv[2])
regr, improved = [], 0
for rel, (w, idx, u, uidx) in sorted(cur.items()):
    bw, bidx, bu, buidx = base.get(rel, (0, set(), 0, set()))
    new = idx - bidx
    new_u = uidx - buidx
    # A WRONG result and a FAIL (a non-rewrite whose error is not an LPTS_<CODE>-formatted
    # UNSUPPORTED refusal) are both gate failures: the first is a wrong translation, the second
    # is emitted SQL that did not even parse/bind.
    if w > bw or (rel not in base and w > 0) or new:
        regr.append((rel, "WRONG", sorted(new) or sorted(idx)))
    if u > bu or (rel not in base and u > 0) or new_u:
        regr.append((rel, "FAIL (non-LPTS error)", sorted(new_u) or sorted(uidx)))
    if w < bw or u < bu:
        improved += 1
if improved:
    print(f"note: {improved} file(s) improved vs baseline (fewer WRONG/FAIL — refresh the baseline)")
if regr:
    print(f"REGRESSION: {len(regr)} file(s) gained WRONG or FAIL queries vs baseline:")
    for rel, kind, ix in regr:
        print(f"  {rel}  new {kind} at query #{ix}")
    sys.exit(1)
print("OK: no new WRONG or FAIL vs baseline")
PY
	check_rc=$?
	;;
*)
	cat "$REPORT" >"$OUT"
	;;
esac
echo "done." >&2
# Propagate the gate result so CI fails on a new WRONG (the trailing echo above must not mask it).
exit "${check_rc:-0}"
