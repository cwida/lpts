//
// lpts_corpus_gate — run DuckDB's own sqllogic .test corpus through LPTS (release build) in parallel
// and produce a per-file coverage report; optionally gate it against a committed baseline.
//
// This is a standalone driver (no DuckDB linkage): it spawns one `unittest` process per .test file and
// aggregates the LPTS verdict logs. Invoked via `make coverage-check` / `make coverage-baseline`.
//
// Mechanism: each .test file runs in its OWN `unittest` process with `SET lpts_check=true` and the
// LPTS_CHECK_LOG environment variable set. LPTS is loaded as a STATICALLY-LINKED extension (via the test
// runner's `statically_loaded_extensions` config), NOT by `LOAD`ing the self-contained lpts.duckdb_extension
// dylib. This matters: the loadable dylib embeds its own private copy of libduckdb, so when LPTS re-plans a
// query inside the unittest process (PlanQuery -> Planner::CreatePlan), decorrelation runs against that
// second DuckDB copy. DuckDB's count-bug fix (and other logic) compares AggregateFunction by *function
// pointer*, which fails across two copies — silently dropping `CASE WHEN count IS NULL THEN 0` and producing
// spurious WRONGs. The statically-linked extension shares the host's single DuckDB copy, so pointer identity
// holds and the check reflects real product behavior.
//
// The presence of LPTS_CHECK_LOG puts lpts_check into LOG mode: it NEVER raises (so the file's own expected
// outputs are unaffected — the DuckDB test passes/fails on its own merits) and instead appends one line per
// intercepted top-level SELECT:
//
//     <query_number> OK                          LPTS rewrote it and the result bag matched
//     <query_number> WRONG                       LPTS rewrote it but the result bag differed
//     <query_number> UNSUPPORTED                 deliberate LPTS_<CODE>-formatted "not supported" refusal
//     <query_number> FAIL                        LPTS could not rewrite AND the error was NOT an
//                                                LPTS_<CODE> refusal — emitted SQL failed to parse/bind
//     <query_number> NONDETERMINISTIC: <reason>  LPTS rewrote it but the query is nondeterministic
//
// The driver collapses each file's log into a single, diff-friendly summary line:
//
//     <relpath> ok=<n> wrong=<n> fail=<n> unsupported=<n> ndet=<n> [WRONG[i,...]] [FAIL[i,...]]
//
// WRONG and FAIL are the gated correctness signals (a wrong translation, or invalid SQL); UNSUPPORTED is an
// acceptable, deliberate refusal; NONDETERMINISTIC is suppressed. (LOG mode also neutralizes
// `PRAGMA enable_verification` on the connection so verification-heavy files — ~55% of the corpus — are
// still checked; see LptsCheckOptimize in src/lpts_extension.cpp.)
//
// Usage (run from the repository root):
//   lpts_corpus_gate [SUBDIR] [OUT]              write report (default SUBDIR=test/sql, OUT=stdout)
//   lpts_corpus_gate --update-baseline [SUBDIR]  (re)write test/duckdb_lpts_baseline.txt
//   lpts_corpus_gate --check [SUBDIR]            run + diff vs baseline; exit 1 on a NEW WRONG or FAIL
//
// Env: JOBS (parallelism, default = CPU count), PER_FILE_TIMEOUT (seconds per file, default 120).
//
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace fs = std::filesystem;

struct FileResult {
	std::string path; // forward-slash path relative to the repo root, e.g. duckdb/test/sql/x.test
	int64_t ok = 0;
	int64_t wrong = 0;
	int64_t fail = 0;
	int64_t unsupported = 0;
	int64_t ndet = 0;
	std::vector<std::string> wrong_idx;
	std::vector<std::string> fail_idx;

	bool HasActivity() const {
		return ok || wrong || fail || unsupported || ndet;
	}
	std::string SummaryLine() const {
		std::ostringstream out;
		out << path << " ok=" << ok << " wrong=" << wrong << " fail=" << fail << " unsupported=" << unsupported
		    << " ndet=" << ndet;
		if (!wrong_idx.empty()) {
			out << " WRONG[";
			for (size_t i = 0; i < wrong_idx.size(); i++) {
				out << (i ? "," : "") << wrong_idx[i];
			}
			out << "]";
		}
		if (!fail_idx.empty()) {
			out << " FAIL[";
			for (size_t i = 0; i < fail_idx.size(); i++) {
				out << (i ? "," : "") << fail_idx[i];
			}
			out << "]";
		}
		return out.str();
	}
};

//===----------------------------------------------------------------------===//
// Process spawning (per-file unittest run with a private environment)
//===----------------------------------------------------------------------===//

// Run `unittest --test-dir duckdb <rel_test>` with the given extra environment variables, stdout/stderr
// discarded, killed after timeout_sec. The child's exit code is irrelevant (a DuckDB test may fail on its
// own merits); only the LPTS log it leaves behind matters.
static void RunUnittestProcess(const std::string &unittest, const std::string &rel_test,
                               const std::vector<std::pair<std::string, std::string>> &extra_env, int64_t timeout_sec) {
#ifdef _WIN32
	// Build an environment block: current environment minus overridden keys, plus the extras.
	// CreateProcessA requires the ANSI block to be sorted case-insensitively by name.
	std::vector<std::string> entries;
	LPCH cur = GetEnvironmentStringsA();
	for (LPCH p = cur; *p;) {
		std::string entry(p);
		p += entry.size() + 1;
		auto eq = entry.find('=');
		std::string key = eq == std::string::npos ? entry : entry.substr(0, eq);
		bool overridden = false;
		for (auto &kv : extra_env) {
			if (_stricmp(key.c_str(), kv.first.c_str()) == 0) {
				overridden = true;
			}
		}
		if (!overridden) {
			entries.push_back(std::move(entry));
		}
	}
	FreeEnvironmentStringsA(cur);
	for (auto &kv : extra_env) {
		entries.push_back(kv.first + "=" + kv.second);
	}
	std::sort(entries.begin(), entries.end(),
	          [](const std::string &a, const std::string &b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
	std::string env_block;
	for (auto &entry : entries) {
		env_block += entry;
		env_block.push_back('\0');
	}
	env_block.push_back('\0');

	std::string cmdline = "\"" + unittest + "\" --test-dir duckdb \"" + rel_test + "\"";

	SECURITY_ATTRIBUTES sa {};
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;
	HANDLE null_handle = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

	STARTUPINFOA si {};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = null_handle;
	si.hStdError = null_handle;
	PROCESS_INFORMATION pi {};
	std::vector<char> cmd(cmdline.begin(), cmdline.end());
	cmd.push_back('\0');
	if (CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, (LPVOID)env_block.data(), nullptr, &si, &pi)) {
		if (WaitForSingleObject(pi.hProcess, (DWORD)(timeout_sec * 1000)) == WAIT_TIMEOUT) {
			TerminateProcess(pi.hProcess, 1);
			WaitForSingleObject(pi.hProcess, 10000);
		}
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
	}
	if (null_handle != INVALID_HANDLE_VALUE) {
		CloseHandle(null_handle);
	}
#else
	// Build the child environment: current environment minus overridden keys, plus the extras.
	std::vector<std::string> env_strings;
	for (char **e = environ; *e; e++) {
		std::string entry(*e);
		auto eq = entry.find('=');
		std::string key = eq == std::string::npos ? entry : entry.substr(0, eq);
		bool overridden = false;
		for (auto &kv : extra_env) {
			if (key == kv.first) {
				overridden = true;
			}
		}
		if (!overridden) {
			env_strings.push_back(std::move(entry));
		}
	}
	for (auto &kv : extra_env) {
		env_strings.push_back(kv.first + "=" + kv.second);
	}
	std::vector<char *> envp;
	for (auto &s : env_strings) {
		envp.push_back(const_cast<char *>(s.c_str()));
	}
	envp.push_back(nullptr);

	std::vector<std::string> arg_strings = {unittest, "--test-dir", "duckdb", rel_test};
	std::vector<char *> argv;
	for (auto &s : arg_strings) {
		argv.push_back(const_cast<char *>(s.c_str()));
	}
	argv.push_back(nullptr);

	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
	posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);

	pid_t pid = -1;
	int rc = posix_spawn(&pid, unittest.c_str(), &fa, nullptr, argv.data(), envp.data());
	posix_spawn_file_actions_destroy(&fa);
	if (rc != 0) {
		return;
	}

	// Wait with a timeout: poll, then SIGKILL the runaway.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
	while (true) {
		int status = 0;
		pid_t done = waitpid(pid, &status, WNOHANG);
		if (done == pid || done < 0) {
			break;
		}
		if (std::chrono::steady_clock::now() >= deadline) {
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
#endif
}

//===----------------------------------------------------------------------===//
// LPTS log parsing
//===----------------------------------------------------------------------===//

static FileResult ParseLptsLog(const std::string &test_path, const fs::path &log_path) {
	FileResult result;
	result.path = test_path;
	std::ifstream in(log_path);
	std::string line;
	while (std::getline(in, line)) {
		auto space = line.find(' ');
		if (space == std::string::npos) {
			continue;
		}
		std::string qnum = line.substr(0, space);
		std::string verdict = line.substr(space + 1);
		if (verdict == "OK") {
			result.ok++;
		} else if (verdict == "WRONG") {
			result.wrong++;
			result.wrong_idx.push_back(qnum);
		} else if (verdict == "FAIL") {
			result.fail++;
			result.fail_idx.push_back(qnum);
		} else if (verdict == "UNSUPPORTED") {
			result.unsupported++;
		} else if (verdict.rfind("NONDETERMINISTIC", 0) == 0) {
			result.ndet++;
		}
	}
	return result;
}

//===----------------------------------------------------------------------===//
// Baseline parsing + gate
//===----------------------------------------------------------------------===//

struct BaselineEntry {
	int64_t wrong = 0;
	int64_t fail = 0;
	std::set<std::string> wrong_idx;
	std::set<std::string> fail_idx;
};

static int64_t ExtractCount(const std::string &line, const std::string &field) {
	auto pos = line.find(" " + field + "=");
	if (pos == std::string::npos) {
		return 0;
	}
	return std::atoll(line.c_str() + pos + field.size() + 2);
}

static std::set<std::string> ExtractIndices(const std::string &line, const std::string &tag) {
	std::set<std::string> result;
	auto pos = line.find(" " + tag + "[");
	if (pos == std::string::npos) {
		return result;
	}
	auto start = pos + tag.size() + 2;
	auto end = line.find(']', start);
	if (end == std::string::npos) {
		return result;
	}
	std::stringstream list(line.substr(start, end - start));
	std::string idx;
	while (std::getline(list, idx, ',')) {
		if (!idx.empty()) {
			result.insert(idx);
		}
	}
	return result;
}

static std::map<std::string, BaselineEntry> LoadSummaryLines(const fs::path &path) {
	std::map<std::string, BaselineEntry> entries;
	std::ifstream in(path);
	std::string line;
	while (std::getline(in, line)) {
		if (line.empty() || line[0] == '#') {
			continue;
		}
		auto space = line.find(' ');
		if (space == std::string::npos) {
			continue;
		}
		BaselineEntry entry;
		entry.wrong = ExtractCount(line, "wrong");
		entry.fail = ExtractCount(line, "fail");
		entry.wrong_idx = ExtractIndices(line, "WRONG");
		entry.fail_idx = ExtractIndices(line, "FAIL");
		entries[line.substr(0, space)] = std::move(entry);
	}
	return entries;
}

static std::string JoinIndices(const std::set<std::string> &indices) {
	std::string result;
	for (auto &idx : indices) {
		result += (result.empty() ? "#" : ", #") + idx;
	}
	return result;
}

// Compare the current run against the committed baseline. A WRONG result and a FAIL (a non-rewrite whose
// error is not an LPTS_<CODE>-formatted UNSUPPORTED refusal) are both gate failures: the first is a wrong
// translation, the second is emitted SQL that did not even parse/bind. Returns the process exit code.
static int RunGate(const std::map<std::string, BaselineEntry> &base, const std::vector<FileResult> &results) {
	int64_t improved = 0;
	std::vector<std::string> regressions;
	for (auto &r : results) {
		BaselineEntry b;
		auto it = base.find(r.path);
		if (it != base.end()) {
			b = it->second;
		}
		std::set<std::string> cur_wrong(r.wrong_idx.begin(), r.wrong_idx.end());
		std::set<std::string> cur_fail(r.fail_idx.begin(), r.fail_idx.end());
		std::set<std::string> new_wrong, new_fail;
		for (auto &idx : cur_wrong) {
			if (!b.wrong_idx.count(idx)) {
				new_wrong.insert(idx);
			}
		}
		for (auto &idx : cur_fail) {
			if (!b.fail_idx.count(idx)) {
				new_fail.insert(idx);
			}
		}
		if (r.wrong > b.wrong || !new_wrong.empty()) {
			regressions.push_back("  " + r.path + "  new WRONG at query " +
			                      JoinIndices(new_wrong.empty() ? cur_wrong : new_wrong));
		}
		if (r.fail > b.fail || !new_fail.empty()) {
			regressions.push_back("  " + r.path + "  new FAIL (non-LPTS error) at query " +
			                      JoinIndices(new_fail.empty() ? cur_fail : new_fail));
		}
		if (r.wrong < b.wrong || r.fail < b.fail) {
			improved++;
		}
	}
	if (improved) {
		printf("note: %lld file(s) improved vs baseline (fewer WRONG/FAIL — refresh the baseline)\n",
		       (long long)improved);
	}
	if (!regressions.empty()) {
		printf("REGRESSION: %zu file(s) gained WRONG or FAIL queries vs baseline:\n", regressions.size());
		for (auto &line : regressions) {
			printf("%s\n", line.c_str());
		}
		return 1;
	}
	printf("OK: no new WRONG or FAIL vs baseline\n");
	return 0;
}

//===----------------------------------------------------------------------===//
// main
//===----------------------------------------------------------------------===//

static int64_t ReadEnvInt(const char *name, int64_t default_value) {
	const char *value = std::getenv(name);
	if (!value || !*value) {
		return default_value;
	}
	int64_t parsed = std::atoll(value);
	return parsed > 0 ? parsed : default_value;
}

int main(int argc, char **argv) {
	std::string mode = "run";
	std::string subdir = "test/sql";
	std::string out_path; // empty = stdout

	std::vector<std::string> args(argv + 1, argv + argc);
	if (!args.empty() && args[0] == "--check") {
		mode = "check";
		if (args.size() > 1) {
			subdir = args[1];
		}
	} else if (!args.empty() && args[0] == "--update-baseline") {
		mode = "baseline";
		if (args.size() > 1) {
			subdir = args[1];
		}
	} else if (!args.empty()) {
		subdir = args[0];
		if (args.size() > 1) {
			out_path = args[1];
		}
	}

	const fs::path unittest = fs::path("build") / "release" / "test" /
#ifdef _WIN32
	                          "unittest.exe";
#else
	                          "unittest";
#endif
	const fs::path baseline_path = fs::path("test") / "duckdb_lpts_baseline.txt";
	const fs::path corpus_root = fs::path("duckdb") / subdir;

	if (!fs::exists(unittest) || !fs::exists(corpus_root)) {
		fprintf(stderr, "run from the repository root after a release build (need %s and %s)\n",
		        unittest.generic_string().c_str(), corpus_root.generic_string().c_str());
		return 2;
	}
	if (mode == "check" && !fs::exists(baseline_path)) {
		fprintf(stderr, "no baseline at %s; run --update-baseline first\n", baseline_path.generic_string().c_str());
		return 2;
	}

	const int64_t timeout_sec = ReadEnvInt("PER_FILE_TIMEOUT", 120);
	const int64_t default_jobs = std::max<int64_t>(1, std::thread::hardware_concurrency());
	const int64_t jobs = ReadEnvInt("JOBS", default_jobs);

	// Enumerate the corpus, sorted for a stable, diffable report order.
	std::vector<std::string> files;
	for (auto &entry : fs::recursive_directory_iterator(corpus_root)) {
		if (entry.is_regular_file() && entry.path().extension() == ".test") {
			files.push_back(entry.path().generic_string());
		}
	}
	std::sort(files.begin(), files.end());
	fprintf(stderr, "running %zu file(s) under %s through LPTS (JOBS=%lld) ...\n", files.size(),
	        corpus_root.generic_string().c_str(), (long long)jobs);

	// Per-run scratch directory for the per-file LPTS logs.
	fs::path workdir = fs::temp_directory_path() / ("lpts_corpus_gate_" + std::to_string(
#ifdef _WIN32
	                                                                          GetCurrentProcessId()
#else
	                                                                          getpid()
#endif
	                                                                              ));
	fs::create_directories(workdir);

	// Worker pool: each worker claims the next file, runs it in its own unittest process, parses the log.
	std::vector<FileResult> results(files.size());
	std::atomic<size_t> next_file {0};
	std::atomic<size_t> completed {0};
	auto worker = [&]() {
		while (true) {
			size_t i = next_file.fetch_add(1);
			if (i >= files.size()) {
				return;
			}
			const std::string &full = files[i];
			// Path relative to the --test-dir root (`duckdb`).
			std::string rel = full.substr(std::string("duckdb/").size());
			std::string safe = full;
			std::replace(safe.begin(), safe.end(), '/', '_');
			fs::path log = workdir / (safe + ".log");
			std::ofstream(log).close(); // truncate

			// Load lpts as a statically-linked extension (host's DuckDB copy) rather than LOADing the
			// self-contained dylib — see the header comment. core_functions must stay in the list (it is
			// the runner default). threads=1 so file-level parallelism owns the cores.
			std::vector<std::pair<std::string, std::string>> extra_env = {
			    {"LPTS_CHECK_LOG", log.string()},
			    {"DUCKDB_TEST_STATICALLY_LOADED_EXTENSIONS", "[core_functions, lpts]"},
			    {"DUCKDB_TEST_ON_NEW_CONNECTION", "SET lpts_check=true; PRAGMA threads=1"},
			};
			RunUnittestProcess(unittest.string(), rel, extra_env, timeout_sec);
			results[i] = ParseLptsLog(full, log);

			size_t done = completed.fetch_add(1) + 1;
			if (done % 500 == 0) {
				fprintf(stderr, "  [%zu/%zu]\n", done, files.size());
			}
		}
	};
	std::vector<std::thread> pool;
	for (int64_t t = 0; t < jobs; t++) {
		pool.emplace_back(worker);
	}
	for (auto &t : pool) {
		t.join();
	}
	std::error_code ignored;
	fs::remove_all(workdir, ignored);

	// Totals header + per-file summary lines (already in sorted file order).
	FileResult totals;
	for (auto &r : results) {
		totals.ok += r.ok;
		totals.wrong += r.wrong;
		totals.fail += r.fail;
		totals.unsupported += r.unsupported;
		totals.ndet += r.ndet;
	}
	std::ostringstream report;
	report << "# totals: files=" << results.size() << " ok=" << totals.ok << " wrong=" << totals.wrong
	       << " fail=" << totals.fail << " unsupported=" << totals.unsupported << " ndet=" << totals.ndet << "\n";

	int exit_code = 0;
	if (mode == "baseline") {
		// Keep the totals header + only files LPTS actually engaged; drop the fully-skipped/empty files so
		// the committed baseline stays signal. The gate treats a file absent from the baseline as "had no
		// WRONG/FAIL", so omitting zero-activity files never hides a future regression.
		int64_t active = 0;
		for (auto &r : results) {
			if (r.HasActivity()) {
				report << r.SummaryLine() << "\n";
				active++;
			}
		}
		std::ofstream(baseline_path) << report.str();
		fprintf(stderr, "wrote baseline: %s (%lld files with activity)\n", baseline_path.generic_string().c_str(),
		        (long long)active);
		fprintf(stderr, "# totals: files=%zu ok=%lld wrong=%lld fail=%lld unsupported=%lld ndet=%lld\n", results.size(),
		        (long long)totals.ok, (long long)totals.wrong, (long long)totals.fail, (long long)totals.unsupported,
		        (long long)totals.ndet);
	} else if (mode == "check") {
		exit_code = RunGate(LoadSummaryLines(baseline_path), results);
	} else {
		for (auto &r : results) {
			report << r.SummaryLine() << "\n";
		}
		if (out_path.empty()) {
			fputs(report.str().c_str(), stdout);
		} else {
			std::ofstream(out_path) << report.str();
		}
	}
	fprintf(stderr, "done.\n");
	return exit_code;
}
