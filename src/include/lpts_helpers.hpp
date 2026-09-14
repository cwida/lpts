#pragma once

#include "duckdb.hpp"
#include "sql_dialect.hpp"

#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace duckdb {

/// Convert a vector of strings into a separated list (e.g. "a, b, c").
string VecToSeparatedList(const vector<string> &input_list, const string &separator = ", ");

/// Convert a join condition list to SQL, using TRUE for conditionless joins.
string JoinConditionsToSQL(const vector<string> &conditions);

/// Quote an identifier for SQL output when needed.
string QuoteIdentifier(const string &identifier);

/// Quote a list of identifiers and join them with a separator.
string VecToQuotedIdentifierList(const vector<string> &input_list, const string &separator = ", ");

/// Quote a table name, preserving a DuckDB AT (...) snapshot suffix if present.
string QuoteTableWithOptionalSuffix(const string &table_name);

/// Split a table name that carries a pinned snapshot (`name AT (<PARAM> => <value>)`, the DuckDB
/// spelling LPTS uses internally) into `base_name` and the `dialect`-rendered snapshot qualifier
/// (e.g. ` VERSION AS OF 366` for Spark). Returns false and leaves the outputs untouched when
/// `table_name` carries no snapshot. Throws when `dialect` has no verified time-travel syntax.
bool TrySplitDialectSnapshotSuffix(const string &table_name, SqlDialect dialect, string &base_name, string &suffix);

/// Build catalog.schema.table with each identifier quoted when needed.
string QualifiedTableName(const string &catalog, const string &schema, const string &table_name);

/// Dialect-aware variants. Backtick dialects use backtick quoting; other dialects
/// fall back to `KeywordHelper::WriteOptionallyQuoted` (matching the dialect-blind overloads above).
string DialectVecToQuotedIdentifierList(const vector<string> &input_list, SqlDialect dialect,
                                        const string &separator = ", ");
string DialectQuoteTableWithOptionalSuffix(const string &table_name, SqlDialect dialect);
string DialectQualifiedTableName(const string &catalog, const string &schema, const string &table_name,
                                 SqlDialect dialect);

/// Throw a structured LPTS NotImplementedException with dialect and feature context.
[[noreturn]] void ThrowLptsNotImplemented(const string &code, SqlDialect dialect, const string &feature_kind,
                                          const string &feature_name, const string &context, const string &reason);

/// Escape single quotes in a string by doubling them (e.g. "it's" -> "it''s").
string EscapeSingleQuotes(const string &input);

/// Convert a SQL string to lowercase, preserving case inside string literals.
string SQLToLowercase(const string &sql);

/// True when `name` begins with a generated table prefix "t<digits>_".
bool HasGeneratedTablePrefix(const string &name);

/// Strip a single leading generated "t<digits>_" prefix, if present (else return `name`).
string StripGeneratedTablePrefix(const string &name);

/// Collapse a redundant "X AS X" (identical operand) to just "X" throughout `sql`, leaving the
/// content of single-quoted string literals untouched. Operands are bare identifiers or
/// double-quoted identifiers.
string SwallowSelfAlias(const string &sql);

/// Replace whole identifier tokens in `sql` using `replacements`.
/// Used by pipeline fusion to fold a column reference (e.g. "t4_sum") into its
/// defining expression (e.g. "sum(t0_amount)") when collapsing a chain of
/// operators into a single SELECT.
///
/// Guarantees:
///   - Only whole identifier tokens are replaced (bounded by non-identifier
///     characters), never substrings — so "t0_a" never matches inside "t0_ab".
///   - Text inside single- or double-quoted string literals is left untouched.
///   - All replacements are applied in a single forward pass; an inserted
///     replacement is never re-scanned, so a value that itself contains
///     "tN_" tokens is not cascaded into further substitutions.
string SubstituteColumnTokens(const string &sql, const std::unordered_map<string, string> &replacements);

/// Remove redundant whitespace from a query string.
void RemoveRedundantWhitespaces(string &query);

/// Heuristic: returns true when `sql` is likely to have a nondeterministic result *value or row order*
/// — so a strict round-trip comparison can spuriously differ even when the rewrite is correct. On a
/// match, `reason` is set to a short human-readable explanation. Covers unordered order-sensitive
/// aggregates (string_agg/listagg/list/array_agg without ORDER BY), random(), window functions over
/// potentially-tied keys (row_number/rank/dense_rank/lag/lead/first_value/last_value/nth_value),
/// ORDER BY with LIMIT/OFFSET/FETCH (tied boundary rows), and floating aggregates whose strict equality
/// can depend on evaluation order (avg/stddev*/variance/var_*).
bool IsLikelyNondeterministicSQL(const string &sql, string &reason);

/// True when `sql` references a wall-clock or transaction-time function. LPTS
/// must preserve these expressions through planning: DuckDB's expression
/// rewriter otherwise folds them to the planning instant, changing the query's
/// meaning when the rendered SQL is executed later.
bool HasWallClockFunctionSQL(const string &sql);

// Build the SQL for `DISTINCT ON (targets) ... [ORDER BY orders]` as a portable row_number() filter:
//   SELECT <cols> FROM (SELECT <cols>, row_number() OVER (PARTITION BY <targets> [ORDER BY <orders>])
//                       AS _lpts_distinct_on_rn FROM <from_clause>) AS _lpts_distinct_on
//   WHERE _lpts_distinct_on_rn = 1
// `from_clause` is a CTE name or a parenthesized subquery. `orders` may be empty (arbitrary row per group).
// The rn column is consumed by the WHERE and never selected outward, so the fixed alias is safe when nested.
string BuildDistinctOnQuery(const vector<string> &cols, const vector<string> &targets, const vector<string> &orders,
                            const string &from_clause);

} // namespace duckdb
