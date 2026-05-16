#pragma once

#include "duckdb.hpp"

namespace duckdb {

//==============================================================================
// SqlDialect
//
// Selects the SQL dialect used for CTE/SQL generation.
// Set via: SET lpts_dialect = 'postgres';
//==============================================================================
enum class SqlDialect {
	DUCKDB,   ///< Default. Uses DuckDB-specific syntax (fully-qualified table refs, "ident" quoting, etc.)
	POSTGRES, ///< PostgreSQL-compatible syntax (unqualified table refs, "ident" quoting, etc.)
	SPARK     ///< Apache Spark SQL syntax (catalog.schema.table qualified, `ident` backtick quoting, ROWS/RANGE-only windows)
};

/// Parse a dialect string ("duckdb", "postgres", or "spark") into the enum.
/// Throws InvalidInputException on unrecognised values.
SqlDialect ParseSqlDialect(const string &value);

/// Quote an identifier in the given dialect.
/// - DUCKDB / POSTGRES: optionally `"name"` (DuckDB's `KeywordHelper::WriteOptionallyQuoted`).
/// - SPARK: always `` `name` `` (Spark identifier quoting; safe for reserved words and case).
string DialectQuoteIdent(const string &name, SqlDialect dialect);

} // namespace duckdb
