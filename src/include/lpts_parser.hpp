#pragma once

#include "sql_dialect.hpp"

#include "duckdb.hpp"

namespace duckdb {

/// Read and validate the lpts_input_dialect setting.
SqlDialect ReadInputDialect(ClientContext &context);

/// Normalize source-dialect SQL into DuckDB SQL before parsing/planning.
string NormalizeInputSqlToDuckDB(const string &query, SqlDialect dialect);

} // namespace duckdb
