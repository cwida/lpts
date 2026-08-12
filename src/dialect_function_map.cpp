//==============================================================================
// dialect_function_map.cpp
//
// Implementation of `RemapFunctionNameForDialect`. See dialect_function_map.hpp
// for the rationale; new dialects extend the dispatch here.
//==============================================================================

#include "dialect_function_map.hpp"
#include "lpts_helpers.hpp"

namespace duckdb {

namespace {

/// Postgres equivalents for DuckDB function names that OpenIVM-emitted plans
/// actually exercise. Functions with identical names/signatures in both DuckDB
/// and PostgreSQL pass through unchanged.
string RemapForPostgres(const string &name) {
	if (name == "strptime") {
		return "to_timestamp";
	}
	if (name == "strftime") {
		return "to_char";
	}
	// PostgreSQL has no arg_min/arg_max, and no rewrite that preserves the
	// aggregate's semantics in one expression. Refuse explicitly so the query is
	// reported as inexpressible in this dialect, rather than emitting a call the
	// target will reject as an unknown function — which reads like an engine
	// limitation instead of a translation one.
	if (name == "arg_min" || name == "argmin" || name == "arg_max" || name == "argmax") {
		ThrowLptsNotImplemented("LPTS_UNSUPPORTED_FUNCTION", SqlDialect::POSTGRES, "aggregate", name, "AGGREGATE",
		                        "postgres has no arg_min/arg_max equivalent");
	}
	if (name == "string_split" || name == "str_split") {
		// PostgreSQL uses string_to_array(string, delimiter) for the same purpose.
		return "string_to_array";
	}
	return name;
}

/// Feldera uses Calcite's SQL surface. Its date formatting functions use the
/// BigQuery-style names and argument order, while string splitting uses SPLIT.
/// ARG_MIN/ARG_MAX are native Feldera aggregates and must not take PostgreSQL's
/// unsupported-function path.
string RemapForFeldera(const string &name) {
	if (name == "strftime") {
		return "FORMAT_TIMESTAMP";
	}
	if (name == "strptime") {
		return "PARSE_TIMESTAMP";
	}
	if (name == "string_split" || name == "str_split") {
		return "SPLIT";
	}
	return name;
}

/// Spark SQL equivalents for DuckDB function names that OpenIVM-emitted plans
/// actually exercise. Functions Spark already has with identical signatures
/// (e.g. `coalesce`, `greatest`, `least`, arithmetic operators, `length`,
/// `lower`, `upper`, `cast`, etc.) pass through unchanged. Unsupported
/// functions surface as a Spark-side error rather than being silently
/// mistranslated.
string RemapForSpark(const string &name) {
	if (name == "get_current_timestamp") {
		return "current_timestamp";
	}
	if (name == "unnest") {
		return "explode";
	}
	if (name == "~~") {
		return "LIKE";
	}
	if (name == "~~*") {
		return "ILIKE";
	}
	if (name == "!~~") {
		return "NOT LIKE";
	}
	if (name == "!~~*") {
		return "NOT ILIKE";
	}
	if (name == "strftime") {
		return "date_format";
	}
	// DuckDB arg_min(arg, val) -> Spark min_by(arg, val): same argument order,
	// same semantics (the arg at the extreme of val).
	if (name == "arg_min" || name == "argmin") {
		return "min_by";
	}
	if (name == "arg_max" || name == "argmax") {
		return "max_by";
	}
	if (name == "strptime") {
		return "to_timestamp";
	}
	if (name == "list_transform" || name == "array_transform") {
		return "transform";
	}
	if (name == "list_aggregate" || name == "array_aggregate") {
		return "aggregate";
	}
	if (name == "list_filter" || name == "array_filter") {
		return "filter";
	}
	if (name == "list_value") {
		return "array";
	}
	if (name == "list_contains" || name == "array_contains") {
		return "array_contains";
	}
	if (name == "list_extract" || name == "array_extract") {
		// Spark's element_at is 1-indexed (matches DuckDB list semantics).
		return "element_at";
	}
	return name;
}

/// Hive SQL equivalents for DuckDB function names that are common in Coral-style
/// Spark/Hive/Trino translation workloads. Keep this conservative: only rename
/// functions with clear same-arity equivalents.
string RemapForHive(const string &name) {
	if (name == "strftime") {
		return "date_format";
	}
	if (name == "strptime") {
		return "to_timestamp";
	}
	if (name == "string_split" || name == "str_split") {
		return "split";
	}
	if (name == "list_transform" || name == "array_transform") {
		return "transform";
	}
	if (name == "list_filter" || name == "array_filter") {
		return "filter";
	}
	if (name == "list_value") {
		return "array";
	}
	if (name == "list_contains" || name == "array_contains") {
		return "array_contains";
	}
	if (name == "list_extract" || name == "array_extract") {
		return "element_at";
	}
	return name;
}

/// Trino and Presto share most function names for the functions we currently
/// serialize. Divergences should split out of this helper only when we need a
/// concrete target-specific mapping.
string RemapForTrinoPresto(const string &name) {
	if (name == "strftime") {
		return "date_format";
	}
	if (name == "strptime") {
		return "date_parse";
	}
	if (name == "string_split" || name == "str_split") {
		return "split";
	}
	if (name == "list_transform" || name == "array_transform") {
		return "transform";
	}
	if (name == "list_filter" || name == "array_filter") {
		return "filter";
	}
	if (name == "list_contains" || name == "array_contains") {
		return "contains";
	}
	if (name == "list_extract" || name == "array_extract") {
		return "element_at";
	}
	return name;
}

string RemapForSnowflake(const string &name) {
	if (name == "strftime") {
		return "TO_CHAR";
	}
	if (name == "strptime") {
		return "TO_TIMESTAMP";
	}
	if (name == "string_split" || name == "str_split") {
		return "SPLIT";
	}
	return name;
}

string RemapForBigQuery(const string &name) {
	if (name == "strftime") {
		return "FORMAT_TIMESTAMP";
	}
	if (name == "strptime") {
		return "PARSE_TIMESTAMP";
	}
	if (name == "string_split" || name == "str_split") {
		return "SPLIT";
	}
	return name;
}

string RemapForRedshift(const string &name) {
	if (name == "strftime") {
		return "TO_CHAR";
	}
	if (name == "strptime") {
		return "TO_TIMESTAMP";
	}
	if (name == "string_split" || name == "str_split") {
		return "SPLIT_TO_ARRAY";
	}
	return name;
}

string RemapForMySQLMariaDB(const string &name) {
	if (name == "strftime") {
		return "DATE_FORMAT";
	}
	if (name == "strptime") {
		return "STR_TO_DATE";
	}
	return name;
}

} // namespace

string RemapFunctionNameForDialect(const string &duckdb_name, SqlDialect dialect) {
	switch (dialect) {
	case SqlDialect::POSTGRES:
		return RemapForPostgres(duckdb_name);
	case SqlDialect::FELDERA:
		return RemapForFeldera(duckdb_name);
	case SqlDialect::SPARK:
		return RemapForSpark(duckdb_name);
	case SqlDialect::HIVE:
		return RemapForHive(duckdb_name);
	case SqlDialect::TRINO_PRESTO:
		return RemapForTrinoPresto(duckdb_name);
	case SqlDialect::SNOWFLAKE:
		return RemapForSnowflake(duckdb_name);
	case SqlDialect::BIGQUERY:
		return RemapForBigQuery(duckdb_name);
	case SqlDialect::REDSHIFT:
		return RemapForRedshift(duckdb_name);
	case SqlDialect::MYSQL_MARIADB:
		return RemapForMySQLMariaDB(duckdb_name);
	case SqlDialect::DUCKDB:
	default:
		return duckdb_name;
	}
}

} // namespace duckdb
