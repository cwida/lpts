#pragma once

#include "lpts_ast.hpp"
#include "cte_nodes.hpp"

namespace duckdb {

/// Phase 1: Convert a DuckDB LogicalOperator tree into a dialect-agnostic AST.
/// `dialect` is forwarded to expression serialization for dialect-specific function renaming.
unique_ptr<AstNode> LogicalPlanToAst(ClientContext &context, unique_ptr<LogicalOperator> &plan,
                                     SqlDialect dialect = SqlDialect::DUCKDB);

/// Phase 2: Convert an AST into a flat CTE list.
/// `dialect` controls dialect-specific SQL rendering (default: DuckDB).
/// `emit_spark_hints` emits Spark optimizer hints when the plan shape is known.
/// `merge_pipeline` fuses chains of single-child pipeline operators into one flat
/// SELECT (one CTE per query block) instead of emitting one CTE per operator.
unique_ptr<CteList> AstToCteList(const AstNode &root, SqlDialect dialect = SqlDialect::DUCKDB,
                                 bool emit_spark_hints = false, bool merge_pipeline = true);

} // namespace duckdb
