#pragma once

#include "duckdb.hpp"
#include "sql_dialect.hpp"

namespace duckdb {

//==============================================================================
// dialect_function_map.hpp
//
// Dialect-specific function-name remapping for `BoundFunctionExpression`
// serialization. Centralises the mapping table that, for each non-DuckDB
// dialect, takes a DuckDB function name and returns the closest semantic
// equivalent in that dialect. Functions a dialect already supports with the
// same name (or that have no widely-agreed equivalent) pass through unchanged.
//
// Adding a new dialect: extend `RemapFunctionNameForDialect` with another
// `else if` branch (or a dedicated helper). This is the single source of
// truth — no other file should hard-code DuckDB→<dialect> function names.
//==============================================================================

/// Return the dialect-equivalent function name for `duckdb_name`, or
/// `duckdb_name` unchanged when no remap is needed for this dialect.
///
/// DUCKDB and any dialect that already shares DuckDB's function name pass
/// through. The mapping is intentionally conservative — only functions
/// OpenIVM-emitted plans actually exercise are remapped. Unsupported
/// functions surface as a target-side error rather than being silently
/// mistranslated.
string RemapFunctionNameForDialect(const string &duckdb_name, SqlDialect dialect);

} // namespace duckdb
