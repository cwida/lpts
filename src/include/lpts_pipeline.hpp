#pragma once

#include "lpts_ast.hpp"
#include "cte_nodes.hpp"
#include <functional>

namespace duckdb {

class TableCatalogEntry;
class AtClause;
class Binder;
// Called by the existing GET visit with the original catalog entry, unaffected by output qualification overrides.
using SnapshotResolver = std::function<unique_ptr<AtClause>(const TableCatalogEntry &)>;

enum class CommonSubplanCompactionMode : uint8_t {
	COST_BASED,
	SERIALIZATION_SIZE,
};

enum class CommonSubplanReusePolicy : uint8_t {
	ALLOW,
	PRESERVE_SOURCE_OCCURRENCES,
};

/// Factor repeated logical subplans into materialized CTEs before SQL serialization.
/// This keeps the emitted CTE graph proportional to the source plan instead of
/// duplicating an inlined subtree at every reference. The caller retains control
/// over when compaction is appropriate because DuckDB may use cardinality estimates
/// when choosing multi-table candidates.
unique_ptr<LogicalOperator> CompactCommonSubplans(ClientContext &context, Binder &binder,
                                                  unique_ptr<LogicalOperator> plan, CommonSubplanCompactionMode mode);

/// Phase 1: Convert a DuckDB LogicalOperator tree into a dialect-agnostic AST.
/// `dialect` is forwarded to expression serialization for dialect-specific function renaming.
unique_ptr<AstNode> LogicalPlanToAst(ClientContext &context, unique_ptr<LogicalOperator> &plan,
                                     SqlDialect dialect = SqlDialect::DUCKDB,
                                     const SnapshotResolver &snapshot_resolver = {},
                                     CommonSubplanReusePolicy reuse_policy = CommonSubplanReusePolicy::ALLOW);

/// Phase 2: Convert an AST into a flat CTE list.
/// `dialect` controls dialect-specific SQL rendering (default: DuckDB).
/// `emit_spark_hints` emits Spark optimizer hints when the plan shape is known.
/// `merge_pipeline` fuses chains of single-child pipeline operators into one flat
/// SELECT (one CTE per query block) instead of emitting one CTE per operator.
unique_ptr<CteList> AstToCteList(const AstNode &root, SqlDialect dialect = SqlDialect::DUCKDB,
                                 bool emit_spark_hints = false, bool merge_pipeline = true);

} // namespace duckdb
