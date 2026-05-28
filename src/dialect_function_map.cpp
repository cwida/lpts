//==============================================================================
// dialect_function_map.cpp
//
// Implementation of `RemapFunctionNameForDialect`. See dialect_function_map.hpp
// for the rationale; new dialects extend the dispatch here.
//==============================================================================

#include "dialect_function_map.hpp"

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
	if (name == "string_split" || name == "str_split") {
		// PostgreSQL uses string_to_array(string, delimiter) for the same purpose.
		return "string_to_array";
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
	if (name == "strftime") {
		return "date_format";
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

} // namespace

string RemapFunctionNameForDialect(const string &duckdb_name, SqlDialect dialect) {
	switch (dialect) {
	case SqlDialect::POSTGRES:
		return RemapForPostgres(duckdb_name);
	case SqlDialect::SPARK:
		return RemapForSpark(duckdb_name);
	case SqlDialect::HIVE:
		return RemapForHive(duckdb_name);
	case SqlDialect::TRINO_PRESTO:
		return RemapForTrinoPresto(duckdb_name);
	case SqlDialect::DUCKDB:
	default:
		return duckdb_name;
	}
}

} // namespace duckdb
