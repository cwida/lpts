#define DUCKDB_EXTENSION_MAIN

#include "lpts_extension.hpp"
#include "cte_nodes.hpp"
#include "lpts_ast.hpp"
#include "lpts_ast_renderer.hpp"
#include "lpts_pipeline.hpp"
#include "lpts_helpers.hpp"
#include "lpts_debug.hpp"
#include "lpts_parser.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/function/pragma_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/parsed_data/create_function_info.hpp"
#include "duckdb/parser/parsed_data/create_pragma_function_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/planner/planner.hpp"
#include "duckdb/common/enums/optimizer_type.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/optimizer/build_probe_side_optimizer.hpp"
#include "duckdb/optimizer/column_lifetime_analyzer.hpp"
#include "duckdb/optimizer/common_aggregate_optimizer.hpp"
#include "duckdb/optimizer/common_subplan_optimizer.hpp"
#include "duckdb/optimizer/cse_optimizer.hpp"
#include "duckdb/optimizer/cte_filter_pusher.hpp"
#include "duckdb/optimizer/cte_inlining.hpp"
#include "duckdb/optimizer/deliminator.hpp"
#include "duckdb/optimizer/empty_result_pullup.hpp"
#include "duckdb/optimizer/expression_heuristics.hpp"
#include "duckdb/optimizer/filter_pullup.hpp"
#include "duckdb/optimizer/filter_pushdown.hpp"
#include "duckdb/optimizer/in_clause_rewriter.hpp"
#include "duckdb/optimizer/join_elimination.hpp"
#include "duckdb/optimizer/join_filter_pushdown_optimizer.hpp"
#include "duckdb/optimizer/join_order/join_order_optimizer.hpp"
#include "duckdb/optimizer/late_materialization.hpp"
#include "duckdb/optimizer/limit_pushdown.hpp"
#include "duckdb/optimizer/regex_range_filter.hpp"
#include "duckdb/optimizer/remove_duplicate_groups.hpp"
#include "duckdb/optimizer/remove_unused_columns.hpp"
#include "duckdb/optimizer/row_group_pruner.hpp"
#include "duckdb/optimizer/sampling_pushdown.hpp"
#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/optimizer/sum_rewriter.hpp"
#include "duckdb/optimizer/topn_optimizer.hpp"
#include "duckdb/optimizer/topn_window_elimination.hpp"
#include "duckdb/optimizer/unnest_rewriter.hpp"
#include "duckdb/optimizer/window_self_join.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"

namespace duckdb {

//------------------------------------------------------------------------------
// Helper: read lpts_dialect from the session settings.
//   Defaults to DuckDB if not set.
//------------------------------------------------------------------------------
static SqlDialect ReadDialect(ClientContext &context) {
	Value dialect_val;
	if (context.TryGetCurrentSetting("lpts_dialect", dialect_val)) {
		return ParseSqlDialect(dialect_val.GetValue<string>());
	}
	return SqlDialect::DUCKDB;
}

// Helper: read lpts_merge_pipeline from the session settings (defaults to true).
static bool ReadMergePipeline(ClientContext &context) {
	Value setting;
	if (context.TryGetCurrentSetting("lpts_merge_pipeline", setting)) {
		return setting.GetValue<bool>();
	}
	return true;
}

static bool EnableDataDependentOptimizers(ClientContext &context) {
	Value setting;
	if (context.TryGetCurrentSetting("lpts_enable_data_dependent_optimizers", setting)) {
		return setting.GetValue<bool>();
	}
	return false;
}

static const set<OptimizerType> &DataDependentOptimizers() {
	static const set<OptimizerType> data_dependent_optimizers = {
	    OptimizerType::JOIN_ORDER,
	    OptimizerType::STATISTICS_PROPAGATION,
	    OptimizerType::ROW_GROUP_PRUNER,
	    OptimizerType::TOP_N,
	    OptimizerType::TOP_N_WINDOW_ELIMINATION,
	    OptimizerType::BUILD_SIDE_PROBE_SIDE,
	    OptimizerType::COMPRESSED_MATERIALIZATION,
	    OptimizerType::JOIN_FILTER_PUSHDOWN,
	    OptimizerType::EXTENSION,
	    OptimizerType::COMMON_SUBPLAN,
	};
	return data_dependent_optimizers;
}

static bool LptsOptimizerDisabled(ClientContext &context, OptimizerType type) {
	if (DataDependentOptimizers().find(type) != DataDependentOptimizers().end()) {
		return true;
	}
	return Optimizer::OptimizerDisabled(context, type);
}

static void LptsRunOptimizer(ClientContext &context, OptimizerType type, const std::function<void()> &callback) {
	if (context.IsInterrupted()) {
		throw InterruptException();
	}
	if (LptsOptimizerDisabled(context, type)) {
		return;
	}
	callback();
}

static unique_ptr<LogicalOperator> LptsOptimizeConservative(ClientContext &context, Binder &binder,
                                                            unique_ptr<LogicalOperator> plan) {
	switch (plan->type) {
	case LogicalOperatorType::LOGICAL_TRANSACTION:
	case LogicalOperatorType::LOGICAL_PRAGMA:
	case LogicalOperatorType::LOGICAL_SET:
	case LogicalOperatorType::LOGICAL_ATTACH:
	case LogicalOperatorType::LOGICAL_UPDATE_EXTENSIONS:
	case LogicalOperatorType::LOGICAL_CREATE_SECRET:
	case LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR:
		if (plan->children.empty()) {
			return plan;
		}
		break;
	default:
		break;
	}

	Optimizer optimizer(binder, context);
	LptsRunOptimizer(context, OptimizerType::EXPRESSION_REWRITER, [&]() { optimizer.rewriter.VisitOperator(*plan); });

	LptsRunOptimizer(context, OptimizerType::CTE_INLINING, [&]() {
		CTEInlining cte_inlining(optimizer);
		plan = cte_inlining.Optimize(std::move(plan));
	});

	LptsRunOptimizer(context, OptimizerType::SUM_REWRITER, [&]() {
		SumRewriterOptimizer sum_rewriter(optimizer);
		sum_rewriter.Optimize(plan);
	});

	LptsRunOptimizer(context, OptimizerType::FILTER_PULLUP, [&]() {
		FilterPullup filter_pullup;
		plan = filter_pullup.Rewrite(std::move(plan));
	});

	LptsRunOptimizer(context, OptimizerType::FILTER_PUSHDOWN, [&]() {
		FilterPushdown filter_pushdown(optimizer);
		unordered_set<idx_t> top_bindings;
		filter_pushdown.CheckMarkToSemi(*plan, top_bindings);
		plan = filter_pushdown.Rewrite(std::move(plan));
	});

	LptsRunOptimizer(context, OptimizerType::CTE_FILTER_PUSHER, [&]() {
		CTEFilterPusher cte_filter_pusher(optimizer);
		plan = cte_filter_pusher.Optimize(std::move(plan));
	});

	LptsRunOptimizer(context, OptimizerType::REGEX_RANGE, [&]() {
		RegexRangeFilter regex_range;
		plan = regex_range.Rewrite(std::move(plan));
	});

	LptsRunOptimizer(context, OptimizerType::IN_CLAUSE, [&]() {
		InClauseRewriter in_clause(context, optimizer);
		plan = in_clause.Rewrite(std::move(plan));
	});

	LptsRunOptimizer(context, OptimizerType::DELIMINATOR, [&]() {
		Deliminator deliminator;
		plan = deliminator.Optimize(std::move(plan));
	});

	LptsRunOptimizer(context, OptimizerType::CTE_INLINING, [&]() {
		CTEInlining cte_inlining(optimizer);
		plan = cte_inlining.Optimize(std::move(plan));
	});

	LptsRunOptimizer(context, OptimizerType::EMPTY_RESULT_PULLUP, [&]() {
		EmptyResultPullup empty_result_pullup;
		plan = empty_result_pullup.Optimize(std::move(plan));
	});

	LptsRunOptimizer(context, OptimizerType::WINDOW_SELF_JOIN, [&]() {
		WindowSelfJoinOptimizer window_self_join(optimizer);
		plan = window_self_join.Optimize(std::move(plan));
	});

	LptsRunOptimizer(context, OptimizerType::JOIN_ORDER, [&]() {
		JoinOrderOptimizer join_order(context);
		plan = join_order.Optimize(std::move(plan));
	});

	LptsRunOptimizer(context, OptimizerType::JOIN_ELIMINATION, [&]() {
		JoinElimination join_elimination;
		plan = join_elimination.Optimize(std::move(plan));
	});

	LptsRunOptimizer(context, OptimizerType::UNNEST_REWRITER, [&]() {
		UnnestRewriter unnest_rewriter;
		plan = unnest_rewriter.Optimize(std::move(plan));
	});

	LptsRunOptimizer(context, OptimizerType::UNUSED_COLUMNS, [&]() {
		RemoveUnusedColumns unused(optimizer);
		unused.VisitOperator(*plan);
	});

	LptsRunOptimizer(context, OptimizerType::DUPLICATE_GROUPS, [&]() {
		RemoveDuplicateGroups duplicate_groups;
		duplicate_groups.VisitOperator(*plan);
	});

	LptsRunOptimizer(context, OptimizerType::COMMON_SUBEXPRESSIONS, [&]() {
		CommonSubExpressionOptimizer cse(binder);
		cse.VisitOperator(*plan);
	});

	LptsRunOptimizer(context, OptimizerType::COLUMN_LIFETIME, [&]() {
		ColumnLifetimeAnalyzer column_lifetime(optimizer, *plan, true);
		column_lifetime.VisitOperator(*plan);
	});

	LptsRunOptimizer(context, OptimizerType::BUILD_SIDE_PROBE_SIDE, [&]() {
		BuildProbeSideOptimizer build_probe_side(context, *plan);
		build_probe_side.VisitOperator(*plan);
	});

	LptsRunOptimizer(context, OptimizerType::COMMON_SUBPLAN, [&]() {
		CommonSubplanOptimizer common_subplan(optimizer);
		plan = common_subplan.Optimize(std::move(plan));
	});

	LptsRunOptimizer(context, OptimizerType::LIMIT_PUSHDOWN, [&]() {
		LimitPushdown limit_pushdown;
		plan = limit_pushdown.Optimize(std::move(plan));
	});

	LptsRunOptimizer(context, OptimizerType::ROW_GROUP_PRUNER, [&]() {
		RowGroupPruner row_group_pruner(context);
		plan = row_group_pruner.Optimize(std::move(plan));
	});

	LptsRunOptimizer(context, OptimizerType::SAMPLING_PUSHDOWN, [&]() {
		SamplingPushdown sampling_pushdown;
		plan = sampling_pushdown.Optimize(std::move(plan));
	});

	LptsRunOptimizer(context, OptimizerType::TOP_N, [&]() {
		TopN top_n(context);
		plan = top_n.Optimize(std::move(plan));
	});

	LptsRunOptimizer(context, OptimizerType::LATE_MATERIALIZATION, [&]() {
		LateMaterialization late_materialization(optimizer);
		plan = late_materialization.Optimize(std::move(plan));
	});

	column_binding_map_t<unique_ptr<BaseStatistics>> statistics_map;
	LptsRunOptimizer(context, OptimizerType::STATISTICS_PROPAGATION, [&]() {
		StatisticsPropagator propagator(optimizer, *plan);
		propagator.PropagateStatistics(plan);
		statistics_map = propagator.GetStatisticsMap();
	});

	LptsRunOptimizer(context, OptimizerType::TOP_N_WINDOW_ELIMINATION, [&]() {
		TopNWindowElimination top_n_window(context, optimizer, &statistics_map);
		plan = top_n_window.Optimize(std::move(plan));
	});

	LptsRunOptimizer(context, OptimizerType::COMMON_AGGREGATE, [&]() {
		CommonAggregateOptimizer common_aggregate;
		common_aggregate.VisitOperator(*plan);
	});

	LptsRunOptimizer(context, OptimizerType::COLUMN_LIFETIME, [&]() {
		ColumnLifetimeAnalyzer column_lifetime(optimizer, *plan, true);
		column_lifetime.VisitOperator(*plan);
	});

	LptsRunOptimizer(context, OptimizerType::REORDER_FILTER, [&]() {
		ExpressionHeuristics reorder_filter(optimizer);
		plan = reorder_filter.Rewrite(std::move(plan));
	});

	LptsRunOptimizer(context, OptimizerType::JOIN_FILTER_PUSHDOWN, [&]() {
		JoinFilterPushdownOptimizer join_filter_pushdown(optimizer);
		join_filter_pushdown.VisitOperator(*plan);
	});

	Planner::VerifyPlan(context, plan);
	return plan;
}

static unique_ptr<LogicalOperator> LptsOptimizePlan(ClientContext &context, Binder &binder,
                                                    unique_ptr<LogicalOperator> plan) {
	if (EnableDataDependentOptimizers(context)) {
		Optimizer optimizer(binder, context);
		return optimizer.Optimize(std::move(plan));
	}
	return LptsOptimizeConservative(context, binder, std::move(plan));
}

/// Plan a query and run it through the optimizer, returning the optimized
/// logical plan. This ensures LPTS sees the same plan DuckDB would execute.
///
/// Data-independent optimizers are enabled by default. Data-dependent optimizers
/// are disabled unless lpts_enable_data_dependent_optimizers is true. Key notes:
///   - CTE_INLINING: inlines CTEs into the query body; produces ordinary LogicalProjection
///     nodes (no new node types needed).
///   - MATERIALIZED_CTE: converts default CTEs to LogicalMaterializedCTE + LogicalCTERef;
///     both are handled by AstMaterializedCteNode / AstCteRefNode.
///   - COMMON_SUBPLAN: detects identical subplans and materializes them as
///     LogicalMaterializedCTE + LogicalCTERef when data-dependent optimizers are enabled;
///     handled by the same nodes above.
///   - STATISTICS_PROPAGATION: when data-dependent optimizers are enabled, LPTS handles
///     LOGICAL_DUMMY_SCAN, LOGICAL_EMPTY_RESULT, and the LogicalExpressionGet+DummyScan
///     pattern emitted by TryExecuteAggregates.
///   - COMPRESSED_MATERIALIZATION: when data-dependent optimizers are enabled, this sub-pass
///     of STATISTICS_PROPAGATION injects __internal_compress_* / __internal_decompress_*
///     function calls. ExpressionToAliasedString() strips these wrappers transparently.
///   - COLUMN_LIFETIME: sets projection_map on LogicalFilter, LogicalOrder, and
///     LogicalComparisonJoin to prune columns no longer referenced above those nodes.
///     FilterNode and OrderNode handle this by using an explicit SELECT column list
///     instead of SELECT *, so the CTE header column count always matches the body.
///   - REORDER_FILTER: only reorders expressions inside LogicalFilter nodes — safe.
///   - JOIN_FILTER_PUSHDOWN: attaches runtime dynamic filters to join/scan nodes; disabled
///     by default because LPTS serializes a static SQL string.
static unique_ptr<LogicalOperator> PlanQuery(ClientContext &context, const string &query) {
	SqlDialect input_dialect = ReadInputDialect(context);
	string normalized = NormalizeInputSqlToDuckDB(query, input_dialect);
	Parser parser(context.GetParserOptions());
	parser.ParseQuery(normalized);
	if (parser.statements.empty()) {
		throw ParserException("Failed to parse query: %s", normalized);
	}
	Planner planner(context);
	planner.CreatePlan(parser.statements[0]->Copy());

	auto result = LptsOptimizePlan(context, *planner.binder, std::move(planner.plan));

#if LPTS_DEBUG
	LPTS_DEBUG_PRINT("[LPTS] ===== Optimized logical plan =====");
	result->Print();
	LPTS_DEBUG_PRINT("[LPTS] ===== end plan =====");
#endif

	return result;
}

static string StripTrailingSemicolon(string sql) {
	while (!sql.empty() &&
	       (sql.back() == ';' || sql.back() == ' ' || sql.back() == '\n' || sql.back() == '\r' || sql.back() == '\t')) {
		sql.pop_back();
	}
	return sql;
}

static string FirstStatementSqlForSubquery(ClientContext &context, const string &query) {
	SqlDialect input_dialect = ReadInputDialect(context);
	string normalized = NormalizeInputSqlToDuckDB(query, input_dialect);
	Parser parser(context.GetParserOptions());
	parser.ParseQuery(normalized);
	if (parser.statements.empty()) {
		throw ParserException("Failed to parse query: %s", normalized);
	}
	return StripTrailingSemicolon(parser.statements[0]->ToString());
}

//------------------------------------------------------------------------------
// PRAGMA lpts('query') — converts a SQL query's logical plan to a SQL string.
//
// Uses DuckDB's pragma_query_t mechanism: the function returns a substitute SQL
// query that DuckDB then executes. So we return "SELECT '<result>' AS sql;"
// which displays the converted SQL string to the user.
//------------------------------------------------------------------------------

static string LptsPragmaFunction(ClientContext &context, const FunctionParameters &parameters) {
	auto query = StringValue::Get(parameters.values[0]);
	auto plan = PlanQuery(context, query);

#if LPTS_DEBUG
	LPTS_DEBUG_PRINT("[LPTS] Logical plan for: " + query);
	plan->Print();
#endif

	SqlDialect dialect = ReadDialect(context);
	auto ast = LogicalPlanToAst(context, plan, dialect);
	auto cte_list = AstToCteList(*ast, dialect, ReadMergePipeline(context));
	string result_sql = cte_list->ToQuery(true);

	// Return a substitute query that displays the result
	string escaped = EscapeSingleQuotes(result_sql);
	return "SELECT '" + escaped + "' AS sql;";
}

//------------------------------------------------------------------------------
// Table function lpts_query('query') — for programmatic use.
//
// Unlike the PRAGMA, this returns the result as a proper table row, making it
// usable in SELECT queries and sqllogictest files:
//   SELECT * FROM lpts_query('SELECT ...');
//------------------------------------------------------------------------------

struct LptsBindData : public TableFunctionData {
	string result_sql;
};

struct LptsGlobalState : public GlobalTableFunctionState {
	bool done = false;
};

static unique_ptr<FunctionData> BindSingleSqlResult(string result_sql, vector<LogicalType> &return_types,
                                                    vector<string> &names) {
	auto result = make_uniq<LptsBindData>();
	result->result_sql = std::move(result_sql);

	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("sql");

	return std::move(result);
}

static unique_ptr<FunctionData> LptsTableBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	auto query = StringValue::Get(input.inputs[0]);
	auto plan = PlanQuery(context, query);

#if LPTS_DEBUG
	LPTS_DEBUG_PRINT("[LPTS] Logical plan for: " + query);
	plan->Print();
#endif

	SqlDialect dialect = ReadDialect(context);
	auto ast = LogicalPlanToAst(context, plan, dialect);
	auto cte_list = AstToCteList(*ast, dialect, ReadMergePipeline(context));

	return BindSingleSqlResult(cte_list->ToQuery(true), return_types, names);
}

static unique_ptr<FunctionData> LptsNormalizeTableBind(ClientContext &context, TableFunctionBindInput &input,
                                                       vector<LogicalType> &return_types, vector<string> &names) {
	auto query = StringValue::Get(input.inputs[0]);
	SqlDialect input_dialect = ReadInputDialect(context);

	return BindSingleSqlResult(NormalizeInputSqlToDuckDB(query, input_dialect), return_types, names);
}

static unique_ptr<GlobalTableFunctionState> LptsTableInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<LptsGlobalState>();
}

static void LptsTableFunc(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = dynamic_cast<LptsGlobalState &>(*data_p.global_state);
	if (state.done) {
		return;
	}
	auto &bind_data = dynamic_cast<const LptsBindData &>(*data_p.bind_data);
	output.SetCardinality(1);
	output.SetValue(0, 0, Value(bind_data.result_sql));
	state.done = true;
}

//------------------------------------------------------------------------------
// PRAGMA lpts_exec('query') — converts a SQL query via LPTS then executes it.
//
// Runs the query through the full LPTS pipeline (plan → AST → SQL) and then
// executes the generated SQL, returning its results directly.
//------------------------------------------------------------------------------

static string LptsExecPragmaFunction(ClientContext &context, const FunctionParameters &parameters) {
	auto query = StringValue::Get(parameters.values[0]);
	auto plan = PlanQuery(context, query);

	SqlDialect dialect = ReadDialect(context);
	auto ast = LogicalPlanToAst(context, plan, dialect);
	auto cte_list = AstToCteList(*ast, dialect, ReadMergePipeline(context));
	return cte_list->ToQuery(true);
}

//------------------------------------------------------------------------------
// PRAGMA lpts_check('query') — round-trip correctness check.
//
// Runs the original query and the LPTS-generated query, then compares results
// using EXCEPT ALL in both directions. Returns a single boolean column "match".
//
// A false result means strict bag equality failed. This can be a real LPTS bug,
// but it can also happen for SQL with nondeterministic result values, e.g.
// unordered string_agg/list aggregates, row_number() over tied ORDER BY keys, or
// ORDER BY ... LIMIT with tied boundary rows.
//------------------------------------------------------------------------------

static string LptsCheckPragmaFunction(ClientContext &context, const FunctionParameters &parameters) {
	auto query = StringValue::Get(parameters.values[0]);
	auto plan = PlanQuery(context, query);

	SqlDialect dialect = ReadDialect(context);
	auto ast = LogicalPlanToAst(context, plan, dialect);
	auto cte_list = AstToCteList(*ast, dialect, ReadMergePipeline(context));
	string lpts_sql = cte_list->ToQuery(true);

	// Normalize the original query to DuckDB's first parsed statement before embedding
	// it as a subquery. Raw SQLStorm inputs often end with "LIMIT ...; -- comment",
	// where simply trimming the last character leaves a semicolon inside the subquery.
	string orig = FirstStatementSqlForSubquery(context, query);
	lpts_sql = StripTrailingSemicolon(std::move(lpts_sql));

	// Compare: no rows in (A EXCEPT ALL B) AND no rows in (B EXCEPT ALL A)
	return "SELECT "
	       "(SELECT count(*) FROM ((" +
	       orig + ") EXCEPT ALL (" + lpts_sql +
	       "))) = 0 AND "
	       "(SELECT count(*) FROM ((" +
	       lpts_sql + ") EXCEPT ALL (" + orig + "))) = 0 AS match;";
}

//------------------------------------------------------------------------------
// PRAGMA print_ast('query') — shows the AST tree for a SQL query.
//------------------------------------------------------------------------------

static void PrintAstPragmaFunction(ClientContext &context, const FunctionParameters &parameters) {
	auto query = StringValue::Get(parameters.values[0]);
	auto plan = PlanQuery(context, query);

	auto ast = LogicalPlanToAst(context, plan);
	string rendered = RenderAstTree(*ast);
	Printer::RawPrint(OutputStream::STREAM_STDOUT, rendered);
}

//------------------------------------------------------------------------------
// Table function print_ast_query('query') — for programmatic use.
//------------------------------------------------------------------------------

struct PrintAstBindData : public TableFunctionData {
	string rendered;
};

static unique_ptr<FunctionData> PrintAstTableBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<string> &names) {
	auto query = StringValue::Get(input.inputs[0]);
	auto plan = PlanQuery(context, query);

	auto ast = LogicalPlanToAst(context, plan);

	auto result = make_uniq<PrintAstBindData>();
	result->rendered = RenderAstTree(*ast);

	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("ast");

	return std::move(result);
}

static void PrintAstTableFunc(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = dynamic_cast<LptsGlobalState &>(*data_p.global_state);
	if (state.done) {
		return;
	}
	auto &bind_data = dynamic_cast<const PrintAstBindData &>(*data_p.bind_data);
	output.SetCardinality(1);
	output.SetValue(0, 0, Value(bind_data.rendered));
	state.done = true;
}

static FunctionDescription LptsFunctionDescription(string description, vector<string> examples) {
	FunctionDescription result;
	result.description = std::move(description);
	result.examples = std::move(examples);
	result.categories = {"LPTS"};
	return result;
}

static void RegisterTableFunctionWithDescription(ExtensionLoader &loader, TableFunction function, string description,
                                                 vector<string> examples) {
	CreateTableFunctionInfo info(std::move(function));
	info.descriptions.push_back(LptsFunctionDescription(std::move(description), std::move(examples)));
	loader.RegisterFunction(std::move(info));
}

static void RegisterPragmaFunctionWithDescription(ExtensionLoader &loader, PragmaFunction function, string description,
                                                  vector<string> examples) {
	CreatePragmaFunctionInfo info(std::move(function));
	info.descriptions.push_back(LptsFunctionDescription(std::move(description), std::move(examples)));

	auto &system_catalog = Catalog::GetSystemCatalog(loader.GetDatabaseInstance());
	auto transaction = CatalogTransaction::GetSystemTransaction(loader.GetDatabaseInstance());
	system_catalog.CreatePragmaFunction(transaction, info);
}

//------------------------------------------------------------------------------
// Extension loading
//------------------------------------------------------------------------------

static void LoadInternal(ExtensionLoader &loader) {
	// Register the lpts_dialect session setting.
	// Users can change it with: SET lpts_dialect = 'postgres';
	DBConfig &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption("lpts_dialect",
	                          "SQL dialect for lpts output. Valid values: 'duckdb' (default), 'postgres', 'spark', "
	                          "'hive', 'trino', 'presto', 'snowflake', 'bigquery', 'redshift', 'mysql', 'mariadb'",
	                          LogicalType::VARCHAR, Value("duckdb"));
	config.AddExtensionOption("lpts_input_dialect",
	                          "SQL dialect for lpts input normalization. Valid values: 'duckdb' (default), "
	                          "'postgres', 'spark', 'hive', 'trino', 'presto', 'snowflake', 'bigquery', 'redshift', "
	                          "'mysql', 'mariadb'",
	                          LogicalType::VARCHAR, Value("duckdb"));
	config.AddExtensionOption("lpts_enable_data_dependent_optimizers",
	                          "Enable LPTS planning optimizers that depend on current data, statistics, "
	                          "cardinality estimates, row groups, or runtime dynamic filters.",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(false));
	config.AddExtensionOption("lpts_merge_pipeline",
	                          "Fuse chains of single-child pipeline operators (Limit/OrderBy/Project/Aggregate/"
	                          "Filter, and pushdown-free base-table scans) into one flat SELECT per query block "
	                          "instead of one CTE per operator.",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(true));

	// Register PRAGMA lpts('query')
	auto pragma = PragmaFunction::PragmaCall("lpts", LptsPragmaFunction, {LogicalType::VARCHAR});
	RegisterPragmaFunctionWithDescription(loader, std::move(pragma),
	                                      "Return the optimized logical plan for a query as equivalent CTE SQL.",
	                                      {"PRAGMA lpts('SELECT name FROM users WHERE age > 25')"});

	// Register table function lpts_query('query') for SELECT * FROM lpts_query(...)
	TableFunction table_func("lpts_query", {LogicalType::VARCHAR}, LptsTableFunc, LptsTableBind, LptsTableInit);
	RegisterTableFunctionWithDescription(
	    loader, std::move(table_func),
	    "Table-function form of PRAGMA lpts. Returns the generated CTE SQL as a single sql column.",
	    {"SELECT sql FROM lpts_query('SELECT name FROM users WHERE age > 25')"});

	// Register table function lpts_normalize_query('query') for input dialect golden tests/debugging
	TableFunction normalize_table_func("lpts_normalize_query", {LogicalType::VARCHAR}, LptsTableFunc,
	                                   LptsNormalizeTableBind, LptsTableInit);
	RegisterTableFunctionWithDescription(
	    loader, std::move(normalize_table_func),
	    "Normalize SQL from lpts_input_dialect into DuckDB SQL without planning it.",
	    {"SET lpts_input_dialect = 'mysql'; SELECT sql FROM lpts_normalize_query('SELECT `order` FROM users LIMIT 1, "
	     "2')"});

	// Register PRAGMA lpts_exec('query') — round-trip: plan → SQL → execute
	auto lpts_exec_pragma = PragmaFunction::PragmaCall("lpts_exec", LptsExecPragmaFunction, {LogicalType::VARCHAR});
	RegisterPragmaFunctionWithDescription(loader, std::move(lpts_exec_pragma),
	                                      "Execute the SQL generated by LPTS and return its result rows.",
	                                      {"PRAGMA lpts_exec('SELECT name FROM users WHERE age > 25')"});

	// Register PRAGMA lpts_check('query') — strict bag round-trip check.
	// It can return false for semantically equivalent SQL when the query result
	// is nondeterministic, e.g. unordered aggregates, LIMIT ties, NULL ties, or
	// window functions with non-unique ordering keys.
	auto lpts_check_pragma = PragmaFunction::PragmaCall("lpts_check", LptsCheckPragmaFunction, {LogicalType::VARCHAR});
	RegisterPragmaFunctionWithDescription(
	    loader, std::move(lpts_check_pragma),
	    "Compare the original query and the LPTS-generated query using EXCEPT ALL in both directions.",
	    {"PRAGMA lpts_check('SELECT name FROM users WHERE age > 25')"});

	// Register PRAGMA print_ast('query')
	auto print_ast_pragma = PragmaFunction::PragmaCall("print_ast", PrintAstPragmaFunction, {LogicalType::VARCHAR});
	RegisterPragmaFunctionWithDescription(loader, std::move(print_ast_pragma),
	                                      "Print the LPTS AST tree for a query to stdout.",
	                                      {"PRAGMA print_ast('SELECT name FROM users WHERE age > 25')"});

	// Register table function print_ast_query('query')
	TableFunction print_ast_table("print_ast_query", {LogicalType::VARCHAR}, PrintAstTableFunc, PrintAstTableBind,
	                              LptsTableInit);
	RegisterTableFunctionWithDescription(
	    loader, std::move(print_ast_table),
	    "Table-function form of PRAGMA print_ast. Returns the rendered AST tree as a single ast column.",
	    {"SELECT ast FROM print_ast_query('SELECT name FROM users WHERE age > 25')"});
}

void LptsExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string LptsExtension::Name() {
	return "lpts";
}

std::string LptsExtension::Version() const {
#ifdef EXT_VERSION_LPTS
	return EXT_VERSION_LPTS;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(lpts, loader) {
	duckdb::LoadInternal(loader);
}
}
