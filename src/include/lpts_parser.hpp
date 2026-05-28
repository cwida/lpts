#pragma once

#include "sql_dialect.hpp"

#include "duckdb.hpp"
#include "duckdb/parser/parser_extension.hpp"

namespace duckdb {

/// Read and validate the lpts_input_dialect setting.
SqlDialect ReadInputDialect(ClientContext &context);

/// Normalize source-dialect SQL into DuckDB SQL before parsing/planning.
string NormalizeInputSqlToDuckDB(const string &query, SqlDialect dialect);

/// Thread-local parser override scope used while LPTS parses user-provided query strings.
class ScopedInputDialect {
public:
	explicit ScopedInputDialect(SqlDialect dialect);
	~ScopedInputDialect();

private:
	bool old_active;
	SqlDialect old_dialect;
};

/// Parser override that lets LPTS normalize non-DuckDB input SQL into ordinary DuckDB SQLStatements.
class LptsInputDialectParserExtension : public ParserExtension {
public:
	LptsInputDialectParserExtension();

	static ParserOverrideResult ParserOverride(ParserExtensionInfo *info, const string &query, ParserOptions &options);
};

} // namespace duckdb
