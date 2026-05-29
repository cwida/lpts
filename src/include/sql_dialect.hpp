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
	SPARK,    ///< Apache Spark SQL syntax (catalog.schema.table qualified, `ident` backtick quoting, ROWS/RANGE-only
	          ///< windows)
	HIVE,     ///< Apache Hive SQL syntax (schema.table qualified, `ident` backtick quoting)
	TRINO_PRESTO, ///< Trino/Presto SQL syntax (catalog.schema.table qualified, "ident" quoting)
	SNOWFLAKE,    ///< Snowflake SQL syntax (catalog.schema.table qualified, "ident" quoting)
	BIGQUERY,     ///< BigQuery SQL syntax (`catalog.schema.table` path quoting)
	REDSHIFT,     ///< Redshift SQL syntax (unqualified table refs, "ident" quoting)
	MYSQL_MARIADB ///< MySQL/MariaDB SQL syntax (schema.table qualified, `ident` quoting)
};

/// Parse a dialect string into the enum.
/// Throws InvalidInputException on unrecognised values.
SqlDialect ParseSqlDialect(const string &value);

/// Parse a dialect string into the enum for a named setting.
/// Throws InvalidInputException on unrecognised values.
SqlDialect ParseSqlDialectSetting(const string &value, const string &setting_name);

/// Stable lower-case name for diagnostics.
string SqlDialectToString(SqlDialect dialect);

/// Return whether the dialect quotes identifiers with backticks instead of ANSI double quotes.
bool DialectUsesBacktickQuotedIdentifiers(SqlDialect dialect);

/// Return whether the dialect renders table names without catalog/schema qualification.
bool DialectUsesUnqualifiedTableNames(SqlDialect dialect);

/// Return whether the dialect renders table names as schema.table.
bool DialectUsesSchemaQualifiedTableNames(SqlDialect dialect);

/// Return whether the dialect renders catalog/schema/table as one quoted path.
bool DialectUsesSingleQuotedTablePath(SqlDialect dialect);

/// Quote an identifier in the given dialect.
/// - DUCKDB / POSTGRES: optionally `"name"` (DuckDB's `KeywordHelper::WriteOptionallyQuoted`).
/// - SPARK / HIVE / BIGQUERY / MYSQL_MARIADB: always `` `name` `` (safe for reserved words and case).
/// - TRINO_PRESTO / SNOWFLAKE / REDSHIFT: optionally `"name"` (ANSI identifier quoting).
string DialectQuoteIdent(const string &name, SqlDialect dialect);

} // namespace duckdb
