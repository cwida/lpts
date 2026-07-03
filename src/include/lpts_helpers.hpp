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

} // namespace duckdb
