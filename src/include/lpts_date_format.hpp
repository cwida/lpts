#pragma once

#include "sql_dialect.hpp"

#include "duckdb.hpp"

namespace duckdb {

enum class InputDateFormatStyle : uint8_t { MYSQL_PERCENT, BIGQUERY_PERCENT, JAVA, POSTGRES, SNOWFLAKE };

string ConvertInputDateFormatToDuckDB(const string &format, SqlDialect dialect, InputDateFormatStyle style);
bool TryConvertDuckDBDateFormatForDialect(const string &format, SqlDialect dialect, string &result);

} // namespace duckdb
