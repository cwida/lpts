#pragma once

#include "duckdb.hpp"
#include "sql_dialect.hpp"

#include <sstream>
#include <string>
#include <vector>

namespace duckdb {

/// Convert a vector of strings into a separated list (e.g. "a, b, c").
string VecToSeparatedList(vector<string> input_list, const string &separator = ", ");

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

/// Escape single quotes in a string by doubling them (e.g. "it's" -> "it''s").
string EscapeSingleQuotes(const string &input);

/// Convert a SQL string to lowercase, preserving case inside string literals.
string SQLToLowercase(const string &sql);

/// Remove redundant whitespace from a query string.
void RemoveRedundantWhitespaces(string &query);

} // namespace duckdb
