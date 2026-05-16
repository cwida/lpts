#pragma once

#include "lpts_ast.hpp"
#include "cte_nodes.hpp"

namespace duckdb {

// (SqlDialect lives in sql_dialect.hpp — included via cte_nodes.hpp)

/// Phase 1: Convert a DuckDB LogicalOperator tree into a dialect-agnostic AST.
/// `dialect` is forwarded to expression serialization for dialect-specific function renaming.
unique_ptr<AstNode> LogicalPlanToAst(ClientContext &context, unique_ptr<LogicalOperator> &plan,
                                     SqlDialect dialect = SqlDialect::DUCKDB);

/// Phase 2: Convert an AST into a flat CTE list.
/// `dialect` controls dialect-specific SQL rendering (default: DuckDB).
unique_ptr<CteList> AstToCteList(const AstNode &root, SqlDialect dialect = SqlDialect::DUCKDB);

} // namespace duckdb
