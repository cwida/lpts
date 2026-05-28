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
	DUCKDB,      ///< Default. Uses DuckDB-specific syntax (fully-qualified table refs, "ident" quoting, etc.)
	POSTGRES,    ///< PostgreSQL-compatible syntax (unqualified table refs, "ident" quoting, etc.)
	SPARK,       ///< Apache Spark SQL syntax (catalog.schema.table qualified, `ident` backtick quoting, ROWS/RANGE-only
	             ///< windows)
	HIVE,        ///< Apache Hive SQL syntax (schema.table qualified, `ident` backtick quoting)
	TRINO_PRESTO ///< Trino/Presto SQL syntax (catalog.schema.table qualified, "ident" quoting)
};

/// Parse a dialect string ("duckdb", "postgres", "spark", "hive", "trino", or "presto") into the enum.
/// Throws InvalidInputException on unrecognised values.
SqlDialect ParseSqlDialect(const string &value);

/// Quote an identifier in the given dialect.
/// - DUCKDB / POSTGRES: optionally `"name"` (DuckDB's `KeywordHelper::WriteOptionallyQuoted`).
/// - SPARK / HIVE: always `` `name` `` (safe for reserved words and case).
/// - TRINO_PRESTO: optionally `"name"` (ANSI identifier quoting).
string DialectQuoteIdent(const string &name, SqlDialect dialect);

} // namespace duckdb
