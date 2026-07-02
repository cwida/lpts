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
#include "duckdb/parser/parser_extension.hpp"
#include "duckdb/parser/simplified_token.hpp"
#include "duckdb/parser/statement/explain_statement.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_column_data_get.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"
#include "duckdb/planner/operator/logical_set_operation.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/common/enums/set_scope.hpp"
#include "duckdb/common/types/vector.hpp"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
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
// EXPLAIN (FORMAT SQL) <query> support.
//
// DuckDB has no "sql" EXPLAIN format. `EXPLAIN (FORMAT SQL) <q>` is not a libpg
// syntax error: libpg parses it, then the transform stage throws
// "Invalid Input Error: "sql" is not a valid FORMAT argument". That happens
// inside the parse-succeeded branch of Parser::ParseQuery, so the
// parse_function/plan_function parser-extension flow (which only fires when
// libpg parsing *fails*) never sees it.
//
// The hook that does run before libpg is parser_override (FALLBACK mode): it is
// tried first, and returning the default ParserOverrideResult() (DISPLAY_ORIGINAL_ERROR)
// makes parsing fall through to normal libpg unchanged. We claim only the exact
// `EXPLAIN ( FORMAT SQL ) <inner>` prefix and turn it into a *real* ExplainStatement, so
// that DuckDB and every client (CLI, JDBC, Python, ...) treat it exactly like a native
// EXPLAIN. The parser_override has no ClientContext (so it cannot run LPTS); it carries the
// inner query through a sentinel string constant, and a companion optimizer extension below
// runs LPTS and replaces the explain output with the generated SQL.
//------------------------------------------------------------------------------

// Detect the `EXPLAIN ( FORMAT SQL )` prefix using DuckDB's tokenizer so that
// comments and whitespace are handled the same way the real parser handles them.
// On a match, inner_out receives the query text following the closing ')'.
static bool MatchExplainFormatSql(const string &query, string &inner_out) {
	auto raw_tokens = Parser::Tokenize(query);

	// Collect non-comment tokens together with their recovered text. SimplifiedToken
	// only carries a type and a byte offset, so the text of token i is the substring
	// from its start up to the next token's start (or end of string), trimmed.
	struct Tok {
		SimplifiedTokenType type;
		idx_t start;
		string text;
	};
	vector<Tok> toks;
	for (idx_t i = 0; i < raw_tokens.size(); i++) {
		if (raw_tokens[i].type == SimplifiedTokenType::SIMPLIFIED_TOKEN_COMMENT) {
			continue;
		}
		idx_t start = raw_tokens[i].start;
		idx_t end = (i + 1 < raw_tokens.size()) ? raw_tokens[i + 1].start : query.size();
		string text = query.substr(start, end - start);
		StringUtil::Trim(text);
		toks.push_back({raw_tokens[i].type, start, std::move(text)});
	}

	if (toks.size() < 5) {
		return false;
	}

	auto is_word = [](SimplifiedTokenType t) {
		return t == SimplifiedTokenType::SIMPLIFIED_TOKEN_KEYWORD ||
		       t == SimplifiedTokenType::SIMPLIFIED_TOKEN_IDENTIFIER;
	};
	auto is_operator = [&](idx_t idx, char c) {
		return toks[idx].type == SimplifiedTokenType::SIMPLIFIED_TOKEN_OPERATOR && query[toks[idx].start] == c;
	};

	// Match: EXPLAIN ( FORMAT SQL )
	if (!is_word(toks[0].type) || !StringUtil::CIEquals(toks[0].text, "explain")) {
		return false;
	}
	if (!is_operator(1, '(')) {
		return false;
	}
	if (!is_word(toks[2].type) || !StringUtil::CIEquals(toks[2].text, "format")) {
		return false;
	}
	if (!is_word(toks[3].type) || !StringUtil::CIEquals(toks[3].text, "sql")) {
		return false;
	}
	if (!is_operator(4, ')')) {
		return false;
	}

	// Everything after the closing ')' is the query to explain.
	idx_t inner_start = toks[4].start + 1;
	if (inner_start >= query.size()) {
		return false;
	}
	string inner = query.substr(inner_start);
	StringUtil::Trim(inner);
	if (inner.empty()) {
		return false;
	}
	inner_out = std::move(inner);
	return true;
}

// Sentinel that tags an ExplainStatement as ours and carries the inner query. We embed
// `<sentinel><inner query>` as a string constant in the explain's inner statement; the
// optimizer extension below recognizes the sentinel, recovers the inner query, and replaces
// the explain output with the LPTS SQL. The 0x1F (unit separator) bytes make accidental
// collision with a user's own constant effectively impossible.
static const char *const LPTS_EXPLAIN_SQL_SENTINEL = "\x1F"
                                                     "LPTS_EXPLAIN_FORMAT_SQL"
                                                     "\x1F";

// parser_override callback: intercept `EXPLAIN (FORMAT SQL) <inner>` and turn it into a real
// ExplainStatement, so the CLI renders it borderless and all clients see EXPLAIN's 2-column
// result. We cannot run LPTS here (no ClientContext), so we carry the inner query through the
// sentinel constant and do the conversion in the optimizer extension (which has a context).
static ParserOverrideResult LptsExplainSqlOverride(ParserExtensionInfo *info, const string &query,
                                                   ParserOptions &options) {
	string inner;
	try {
		if (!MatchExplainFormatSql(query, inner)) {
			// Not our query — fall through to the normal DuckDB parser.
			return ParserOverrideResult();
		}
	} catch (...) {
		// Tokenization failed; let the normal parser report the real error.
		return ParserOverrideResult();
	}

	try {
		LPTS_DEBUG_PRINT("[LPTS] EXPLAIN (FORMAT SQL) intercepted, inner query: " + inner);
		// Build a trivial inner statement whose single constant carries the sentinel + inner query.
		// EXPLAIN of this binds to a tiny plan that the optimizer extension replaces wholesale.
		string carrier = "SELECT '" + EscapeSingleQuotes(string(LPTS_EXPLAIN_SQL_SENTINEL) + inner) + "'";
		Parser parser(options);
		parser.ParseQuery(carrier);
		if (parser.statements.empty()) {
			return ParserOverrideResult();
		}
		auto explain = make_uniq<ExplainStatement>(std::move(parser.statements[0]), ExplainType::EXPLAIN_STANDARD);
		vector<unique_ptr<SQLStatement>> statements;
		statements.push_back(std::move(explain));
		return ParserOverrideResult(std::move(statements));
	} catch (std::exception &ex) {
		return ParserOverrideResult(ex);
	}
}

// Recursively search a logical operator subtree for a VARCHAR constant beginning with the
// LPTS sentinel. On success, returns true and sets inner_out to the carried inner query.
static bool FindLptsExplainSentinel(LogicalOperator &op, string &inner_out) {
	const string sentinel = LPTS_EXPLAIN_SQL_SENTINEL;
	bool found = false;
	for (auto &expr : op.expressions) {
		ExpressionIterator::EnumerateExpression(expr, [&](Expression &child) {
			if (found || child.type != ExpressionType::VALUE_CONSTANT) {
				return;
			}
			auto &constant = child.Cast<BoundConstantExpression>();
			if (constant.value.type().id() != LogicalTypeId::VARCHAR || constant.value.IsNull()) {
				return;
			}
			auto str = StringValue::Get(constant.value);
			if (StringUtil::StartsWith(str, sentinel)) {
				inner_out = str.substr(sentinel.size());
				found = true;
			}
		});
		if (found) {
			return true;
		}
	}
	for (auto &child : op.children) {
		if (FindLptsExplainSentinel(*child, inner_out)) {
			return true;
		}
	}
	return false;
}

// Optimizer extension: when the plan is an EXPLAIN carrying our sentinel, run the LPTS pipeline
// on the inner query and replace the whole plan with a single ("logical_plan", <SQL>) row. The
// statement type stays EXPLAIN_STATEMENT, so the CLI renders the value verbatim (borderless).
static void LptsExplainSqlOptimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	if (plan->type != LogicalOperatorType::LOGICAL_EXPLAIN) {
		return;
	}
	string inner;
	if (!FindLptsExplainSentinel(*plan, inner)) {
		// A normal EXPLAIN, not ours — leave it untouched.
		return;
	}
	LPTS_DEBUG_PRINT("[LPTS] EXPLAIN (FORMAT SQL) optimizer hook, inner query: " + inner);

	// Run the exact same pipeline as PRAGMA lpts. Any failure (catalog error, unsupported
	// operator, ...) propagates as a normal query error, just like the inner query would error.
	auto inner_plan = PlanQuery(input.context, inner);
	SqlDialect dialect = ReadDialect(input.context);
	auto ast = LogicalPlanToAst(input.context, inner_plan, dialect);
	auto cte_list = AstToCteList(*ast, dialect);
	string sql = cte_list->ToQuery(true);

	vector<LogicalType> types {LogicalType::VARCHAR, LogicalType::VARCHAR};
	auto collection = make_uniq<ColumnDataCollection>(input.context, types);
	DataChunk chunk;
	chunk.Initialize(Allocator::Get(input.context), types);
	// Key "logical_opt" makes the CLI print the "Optimized Logical Plan" caption, which is
	// accurate: LPTS serializes the optimized logical plan. Clients see it as explain_value.
	chunk.SetValue(0, 0, Value("logical_opt"));
	chunk.SetValue(1, 0, Value(sql));
	chunk.SetCardinality(1);
	collection->Append(chunk);

	idx_t table_index = input.optimizer.binder.GenerateTableIndex();
	plan = make_uniq<LogicalColumnDataGet>(table_index, std::move(types), std::move(collection));
}

//------------------------------------------------------------------------------
// lpts_check mode: transparent round-trip verification.
//
// When `SET lpts_check = true`, the optimizer hook below rewrites every top-level
// SELECT into a UNION ALL of two sides:
//   - LptsCheckHash(original):  passes all original tuples through (so the query
//                               returns its normal result, in its normal order)
//                               while hashing every tuple.
//   - LptsCheckCmp(LPTS(query)): consumes the LPTS-rewritten result, emits ZERO
//                               tuples, and hashes every tuple.
// Each side hashes its tuples order-independently (additive combination of per-row
// hashes) and publishes the final hash to a per-connection record on the
// ClientContext at OperatorFinalize (a single, post-all-threads, global call). The
// first side to finish stores its hash; the second compares and raises an error iff
// the multisets differ. The original result is returned untouched, so a test's
// expected output never changes — only a genuine LPTS round-trip divergence errors.
//------------------------------------------------------------------------------

// `SET lpts_check = true` turns on transparent round-trip verification of every top-level SELECT. The
// behavior is chosen by the LPTS_CHECK_LOG environment variable (set by the suite driver):
//   STRICT (env unset) — the query returns its normal result; a round-trip mismatch raises
//       "LPTS check failed", and a query LPTS cannot rewrite raises a specific "unsupported" error. Both
//       are deterministic messages that a sqllogictest `statement error` can match.
//   LOG (env set to a path) — lenient: never raises. For each checked SELECT it appends one line to that
//       path — "<query_number> UNSUPPORTED" (deliberate LPTS_<CODE> refusal), "<query_number> FAIL"
//       (could not rewrite, non-LPTS error — a bug), "<query_number> OK" (rewrote, bags
//       matched), "<query_number> WRONG" (rewrote, bags differed), or "<query_number> NONDETERMINISTIC:
//       <reason>" (rewrote, but the query is nondeterministic so the comparison is not trusted; <reason>
//       is the heuristic's explanation). The original rows are returned so the test run continues. The
//       driver runs one .test file per process and writes a "# <filename>" header, so the combined log
//       attributes each query to its file.
enum class LptsCheckVerdict : uint8_t { STRICT, LOG };

static const char *LptsCheckLogPath() {
	return std::getenv("LPTS_CHECK_LOG");
}

static void LptsCheckAppendLog(const string &path, idx_t qnum, const string &status) {
	std::ofstream f(path, std::ios::app);
	if (f) {
		f << qnum << " " << status << "\n";
	}
}

class LptsCheckModeState : public ClientContextState {
public:
	bool enabled = false; // SET lpts_check = true
	bool in_rewrite = false;
	idx_t log_qnum = 0; // per-connection 1-based SELECT counter, for LOG-mode line numbers
};

// Per-connection, per-query coordination record. The hash (original) and cmp (rewritten) operators
// fetch_add into the two accumulators during Execute — correct regardless of how many pipelines/threads
// each side spans. The verdict is acted on once both sides are done ("last one out").
class LptsCheckRunState : public ClientContextState {
public:
	std::atomic<uint64_t> hash_sum {0};
	std::atomic<uint64_t> cmp_sum {0};
	std::atomic<int> sides_remaining {0}; // "last one out" counter (init 2 per query)
	bool suppress = false;                // query is likely nondeterministic → a diff is not a failure
	string suppress_reason;               // why it was judged nondeterministic (logged with NONDETERMINISTIC)
	// Like `suppress`, but only applied when the two sides actually DIFFER: the query is nondeterministic
	// *conditionally* (e.g. a scalar subquery that returns an arbitrary row only when it matches >1 row). A
	// deterministic run of such a query still matches and logs OK; only a genuine divergence is excused.
	bool suppress_on_mismatch = false;
	string suppress_on_mismatch_reason;
	LptsCheckVerdict behavior = LptsCheckVerdict::STRICT;
	idx_t qnum = 0;  // this SELECT's 1-based number (LOG mode)
	string log_path; // LOG mode: where to append the per-query result line

	void ResetQuery(LptsCheckVerdict behavior_p, idx_t qnum_p, string log_path_p) {
		hash_sum.store(0);
		cmp_sum.store(0);
		sides_remaining.store(0);
		suppress = false;
		suppress_reason.clear();
		suppress_on_mismatch = false;
		suppress_on_mismatch_reason.clear();
		behavior = behavior_p;
		qnum = qnum_p;
		log_path = std::move(log_path_p);
	}

	void Add(bool hash_side, uint64_t v) {
		(hash_side ? hash_sum : cmp_sum).fetch_add(v);
	}

	// Acted on once, by the last side out (both hashes complete). STRICT raises on a real mismatch
	// (a nondeterministic divergence is suppressed — the check is lenient). LOG never raises; it records
	// the outcome for a rewritten query: "OK" (bags matched), "WRONG" (bags differed), or
	// "NONDETERMINISTIC: <reason>" (query is nondeterministic, so the comparison is not trusted; the reason
	// is the heuristic's explanation, e.g. an order-sensitive aggregate or ORDER BY with LIMIT).
	void ActOnVerdict() {
		bool hashes_equal = (hash_sum.load() == cmp_sum.load());
		// A conditional-nondeterminism excuse only applies when the sides actually diverged.
		bool mismatch_excused = !hashes_equal && suppress_on_mismatch;
		bool match = hashes_equal || suppress || mismatch_excused;
		if (behavior == LptsCheckVerdict::STRICT) {
			if (!match) {
				throw InvalidInputException(
				    "LPTS check failed: the LPTS-rewritten query returned a different bag of rows than the "
				    "original (hash %llu vs %llu)",
				    static_cast<unsigned long long>(hash_sum.load()), static_cast<unsigned long long>(cmp_sum.load()));
			}
		} else if (suppress) {
			string status = suppress_reason.empty() ? "NONDETERMINISTIC" : "NONDETERMINISTIC: " + suppress_reason;
			LptsCheckAppendLog(log_path, qnum, status);
		} else if (hashes_equal) {
			LptsCheckAppendLog(log_path, qnum, "OK");
		} else if (mismatch_excused) {
			string status = suppress_on_mismatch_reason.empty() ? "NONDETERMINISTIC"
			                                                    : "NONDETERMINISTIC: " + suppress_on_mismatch_reason;
			LptsCheckAppendLog(log_path, qnum, status);
		} else {
			LptsCheckAppendLog(log_path, qnum, "WRONG");
		}
	}

	// "Last one out switches off the lights": each side calls this when fully done. The last caller
	// (whichever finishes — order-independent) computes the verdict with both hashes complete.
	void OnSideDone() {
		if (sides_remaining.fetch_sub(1) == 1) {
			ActOnVerdict();
		}
	}
};

static LptsCheckModeState &GetCheckMode(ClientContext &context) {
	return *context.registered_state->GetOrCreate<LptsCheckModeState>("lpts_check_mode");
}

static LptsCheckRunState &GetCheckRunState(ClientContext &context) {
	return *context.registered_state->GetOrCreate<LptsCheckRunState>("lpts_check_run_state");
}

static void LptsCheckSetCallback(ClientContext &context, SetScope scope, Value &parameter) {
	GetCheckMode(context).enabled = !parameter.IsNull() && parameter.GetValue<bool>();
}

// Hash all rows of `chunk` order-independently into `running_sum`. DataChunk::Hash gives a
// per-row hash that is NULL-aware and column-position-sensitive; summing the per-row hashes is
// commutative (so tuple order is irrelevant) and duplicate-sensitive (so it is true bag equality).
static void AccumulateChunkHash(DataChunk &chunk, uint64_t &running_sum) {
	const idx_t n = chunk.size();
	if (n == 0) {
		return;
	}
	if (chunk.ColumnCount() == 0) {
		running_sum += static_cast<uint64_t>(n);
		return;
	}
	Vector hashes(LogicalType::HASH);
	chunk.Hash(hashes);
	hashes.Flatten(n);
	auto hash_data = FlatVector::GetData<hash_t>(hashes);
	uint64_t local = 0;
	for (idx_t i = 0; i < n; i++) {
		local += static_cast<uint64_t>(hash_data[i]);
	}
	running_sum += local;
}

// Hash side for the streaming SET/LOG path: a pass-through operator that streams the original rows up to
// the UNION (no buffering) while accumulating their order-independent hash (atomic — correct across any
// number of pipelines). It lives in the UNION's base pipeline, which gets a PipelineFinishEvent, so its
// OperatorFinalize is the hash side's reliable "I'm done" signal for last-one-out.
class PhysicalLptsCheck : public PhysicalOperator {
public:
	bool passthrough;
	bool last_one_out; // streaming SET/LOG hash side: signal OnSideDone via OperatorFinalize

	PhysicalLptsCheck(PhysicalPlan &physical_plan, vector<LogicalType> types_p, idx_t estimated_cardinality,
	                  bool passthrough_p, bool last_one_out_p)
	    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, std::move(types_p), estimated_cardinality),
	      passthrough(passthrough_p), last_one_out(last_one_out_p) {
	}

	bool ParallelOperator() const override {
		return true;
	}

	OperatorResultType Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk, GlobalOperatorState &,
	                           OperatorState &) const override {
		uint64_t partial = 0;
		AccumulateChunkHash(input, partial);
		GetCheckRunState(context.client).Add(/*hash_side=*/passthrough, partial);
		if (passthrough) {
			chunk.Reference(input);
		} else {
			chunk.SetCardinality(0);
		}
		return OperatorResultType::NEED_MORE_INPUT;
	}

	// The streaming hash side lives in the UNION's base pipeline, which gets a PipelineFinishEvent ⇒ this
	// fires once after all of its input. It is the hash side's "I'm done" signal for last-one-out.
	bool RequiresOperatorFinalize() const override {
		return last_one_out;
	}
	OperatorFinalResultType OperatorFinalize(Pipeline &, Event &, ClientContext &context,
	                                         OperatorFinalizeInput &) const override {
		GetCheckRunState(context).OnSideDone();
		return OperatorFinalResultType::FINISHED;
	}

	string GetName() const override {
		return passthrough ? "LPTS_CHECK_HASH" : "LPTS_CHECK_CMP";
	}
};

// Hash side: passes the original rows through unchanged while hashing them.
class LogicalLptsCheckHash : public LogicalExtensionOperator {
public:
	bool last_one_out;

	LogicalLptsCheckHash(unique_ptr<LogicalOperator> child, bool last_one_out_p) : last_one_out(last_one_out_p) {
		children.push_back(std::move(child));
	}

	void ResolveTypes() override {
		types = children[0]->types;
	}
	vector<ColumnBinding> GetColumnBindings() override {
		return children[0]->GetColumnBindings();
	}
	string GetExtensionName() const override {
		return "lpts_check_hash";
	}

	PhysicalOperator &CreatePlan(ClientContext &, PhysicalPlanGenerator &planner) override {
		auto &child = planner.CreatePlan(*children[0]);
		auto &op = planner.Make<PhysicalLptsCheck>(child.GetTypes(), children[0]->estimated_cardinality,
		                                           /*passthrough=*/true, last_one_out);
		op.children.push_back(child);
		return op;
	}
};

// Cmp side: a SINK that consumes the rewritten rows (hashing them into cmp_sum) and a SOURCE that emits
// nothing. Its sink Finalize — reliable, fires after the whole rewritten subplan (inner UNIONs/joins
// included) — is the cmp side's "I'm done" signal for last-one-out. It declares the original (left) types
// so the hand-built UNION type-checks; it emits 0 rows, so those declared types never carry data.
class LptsCheckSinkGlobalState : public GlobalSinkState {};
class LptsCheckSinkSourceState : public GlobalSourceState {};

class PhysicalLptsCheckSink : public PhysicalOperator {
public:
	PhysicalLptsCheckSink(PhysicalPlan &physical_plan, vector<LogicalType> types_p, idx_t estimated_cardinality)
	    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, std::move(types_p), estimated_cardinality) {
	}

	bool IsSink() const override {
		return true;
	}
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &) const override {
		return make_uniq<LptsCheckSinkGlobalState>();
	}
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &) const override {
		uint64_t partial = 0;
		AccumulateChunkHash(chunk, partial);
		GetCheckRunState(context.client).Add(/*hash_side=*/false, partial);
		return SinkResultType::NEED_MORE_INPUT;
	}
	SinkFinalizeType Finalize(Pipeline &, Event &, ClientContext &context, OperatorSinkFinalizeInput &) const override {
		GetCheckRunState(context).OnSideDone();
		return SinkFinalizeType::READY;
	}

	bool IsSource() const override {
		return true;
	}
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &) const override {
		return make_uniq<LptsCheckSinkSourceState>();
	}
	SourceResultType GetDataInternal(ExecutionContext &, DataChunk &, OperatorSourceInput &) const override {
		return SourceResultType::FINISHED; // emits no rows
	}

	string GetName() const override {
		return "LPTS_CHECK_CMP";
	}
};

class LogicalLptsCheckSink : public LogicalExtensionOperator {
public:
	vector<LogicalType> forced_types;

	LogicalLptsCheckSink(unique_ptr<LogicalOperator> child, vector<LogicalType> forced_types_p)
	    : forced_types(std::move(forced_types_p)) {
		children.push_back(std::move(child));
	}

	void ResolveTypes() override {
		types = forced_types;
	}
	vector<ColumnBinding> GetColumnBindings() override {
		return children[0]->GetColumnBindings();
	}
	string GetExtensionName() const override {
		return "lpts_check_cmp";
	}

	PhysicalOperator &CreatePlan(ClientContext &, PhysicalPlanGenerator &planner) override {
		auto &child = planner.CreatePlan(*children[0]);
		auto &op = planner.Make<PhysicalLptsCheckSink>(forced_types, children[0]->estimated_cardinality);
		op.children.push_back(child);
		return op;
	}
};

// Sequence functions read/advance sequence state, so running the original and the rewritten query in
// succession sees different values — the round-trip cannot be verified even though the translation is
// correct. The text-based nondeterminism heuristic misses these when they are hidden inside a macro
// (the query text shows `my_macro(...)`, not `nextval(...)`), so also scan the bound plan, where macros
// are already expanded.
static bool ExpressionUsesSequenceFunction(const Expression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		const auto &name = expr.Cast<BoundFunctionExpression>().function.name;
		if (name == "nextval" || name == "currval" || name == "lastval") {
			return true;
		}
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (!found && ExpressionUsesSequenceFunction(child)) {
			found = true;
		}
	});
	return found;
}

static bool PlanUsesSequenceFunction(const LogicalOperator &op) {
	for (const auto &expr : op.expressions) {
		if (ExpressionUsesSequenceFunction(*expr)) {
			return true;
		}
	}
	for (const auto &child : op.children) {
		if (PlanUsesSequenceFunction(*child)) {
			return true;
		}
	}
	return false;
}

// Does this subtree guarantee at most one row (so a scalar-subquery SINGLE join over it is well-defined)?
// An aggregate collapses to ≤1 row per group; a LIMIT/TOP_N caps the count. Row-preserving single-child
// operators (projection/filter/order) are transparent; anything else (scan, join, set-op, ...) can be
// multi-row.
static bool SubtreeGuaranteesSingleRow(const LogicalOperator &op) {
	const LogicalOperator *cur = &op;
	while (cur) {
		switch (cur->type) {
		case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:
			// Only an UNGROUPED aggregate yields exactly one row. A grouped aggregate yields ≤1 row per
			// group, but a SINGLE join's correlation can match several groups per outer row (e.g. an
			// `(ba = ra OR ra IS NULL)` link), so it is still multi-row-capable.
			return cur->Cast<LogicalAggregate>().groups.empty();
		case LogicalOperatorType::LOGICAL_LIMIT:
		case LogicalOperatorType::LOGICAL_TOP_N:
			return true;
		case LogicalOperatorType::LOGICAL_PROJECTION:
		case LogicalOperatorType::LOGICAL_FILTER:
		case LogicalOperatorType::LOGICAL_ORDER_BY:
			if (cur->children.size() != 1) {
				return false;
			}
			cur = cur->children[0].get();
			continue;
		default:
			return false;
		}
	}
	return false;
}

// A SINGLE join implements a scalar subquery (≤1 row per outer). DuckDB enforces that at runtime: with the
// default it raises on >1 row; with scalar_subquery_error_on_multiple_rows=false it returns an ARBITRARY
// row (nondeterministic). LPTS decorrelates it to `LEFT JOIN (SELECT DISTINCT *)`, which is only faithful
// when the RHS yields ≤1 row per key — otherwise it fans out / picks a different arbitrary row. Detecting a
// SINGLE join whose subquery side is not row-count-bounded lets the check treat a *divergence* as
// nondeterminism rather than a translation bug. Returns true if such a join exists.
static bool PlanHasMultiRowSingleJoin(const LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN || op.type == LogicalOperatorType::LOGICAL_DELIM_JOIN) {
		const auto &join = op.Cast<LogicalComparisonJoin>();
		if (join.join_type == JoinType::SINGLE && op.children.size() == 2) {
			const idx_t subquery_child = join.delim_flipped ? 0 : 1;
			if (!SubtreeGuaranteesSingleRow(*op.children[subquery_child])) {
				return true;
			}
		}
	}
	for (const auto &child : op.children) {
		if (PlanHasMultiRowSingleJoin(*child)) {
			return true;
		}
	}
	return false;
}

// Optimizer hook (post-optimize): when lpts_check mode is on, replace a top-level SELECT's plan
// with UNION ALL(LptsCheckHash(original), LptsCheckCmp(LPTS(original))).
static void LptsCheckOptimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	auto &mode = GetCheckMode(input.context);
	if (mode.in_rewrite) {
		return; // a nested PlanQuery from our own rewrite — leave it alone.
	}
	if (!mode.enabled) {
		return;
	}
	if (plan->type == LogicalOperatorType::LOGICAL_EXPLAIN) {
		return; // EXPLAIN (FORMAT SQL) is owned by the explain hook.
	}
	auto &client_config = ClientConfig::GetConfig(input.context);
	if (client_config.AnyVerification() || client_config.verify_parallelism) {
		// Statement verification (e.g. PRAGMA enable_verification / verify_parallelism, common in DuckDB's
		// own test corpus) re-runs the plan multiple times (serialization round-trip, parallel re-execution).
		// Our injected extension operators (lpts_check_hash / lpts_check_sink) have no deserialization method,
		// and repeated execution would double-count into the hash accumulators — so we cannot check a query
		// while any verification is active.
		//
		// In LOG mode (the suite driver) more than half of DuckDB's .test files self-enable verification,
		// which would otherwise leave them entirely unchecked. Since LOG mode never alters a query's
		// result and we only read LPTS's own log, neutralize verification on this connection so that every
		// *subsequent* query in the file is checkable. We still skip the current query (it may already be
		// mid-verification), so a file loses at most its first post-PRAGMA query from coverage. Strict mode
		// (interactive / SQLStorm) keeps the conservative skip and never touches the user's config.
		if (LptsCheckLogPath()) {
			client_config.query_verification_enabled = false;
			client_config.verify_external = false;
			client_config.verify_serializer = false;
			client_config.verify_fetch_row = false;
			client_config.verify_parallelism = false;
		}
		return;
	}

	// Only intercept a single top-level SELECT. CREATE/INSERT/UPDATE/PRAGMA/... pass through untouched.
	const string &query = input.context.GetCurrentQuery();
	{
		Parser parser(input.context.GetParserOptions());
		try {
			parser.ParseQuery(query);
		} catch (...) {
			return;
		}
		if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
			return;
		}
	}
	// Don't check queries that read LPTS's own inspector table functions (lpts_query / print_ast_query /
	// lpts_normalize_query): they wrap LPTS output and can't themselves be round-tripped — let them run as-is.
	{
		string lower_query = StringUtil::Lower(query);
		if (lower_query.find("lpts_query") != string::npos || lower_query.find("print_ast_query") != string::npos ||
		    lower_query.find("lpts_normalize_query") != string::npos) {
			return;
		}
	}

	struct RewriteGuard {
		LptsCheckModeState &m;
		explicit RewriteGuard(LptsCheckModeState &m_) : m(m_) {
			m.in_rewrite = true;
		}
		~RewriteGuard() {
			m.in_rewrite = false;
		}
	} guard(mode);

	// LOG when the driver set LPTS_CHECK_LOG (lenient + per-query logging); STRICT otherwise (raise).
	const char *log_path = LptsCheckLogPath();
	LptsCheckVerdict behavior = log_path ? LptsCheckVerdict::LOG : LptsCheckVerdict::STRICT;
	idx_t qnum = (behavior == LptsCheckVerdict::LOG) ? ++mode.log_qnum : 0;

	auto &run = GetCheckRunState(input.context);
	run.ResetQuery(behavior, qnum, log_path ? string(log_path) : string());
	string reason;
	run.suppress = IsLikelyNondeterministicSQL(query, reason); // a nondeterministic diff is not a failure
	run.suppress_reason = run.suppress ? reason : string();
	run.sides_remaining.store(2);

	SqlDialect dialect = ReadDialect(input.context);
	unique_ptr<LogicalOperator> original = std::move(plan);

	// Plan-level nondeterminism: a sequence function (nextval/currval/lastval) may be hidden inside a macro
	// that the text heuristic can't see. Macros are expanded in the bound plan, so scan it here.
	if (!run.suppress && PlanUsesSequenceFunction(*original)) {
		run.suppress = true;
		run.suppress_reason = "sequence function (nextval/currval/lastval) advances/reads sequence state";
	}

	// A scalar-subquery SINGLE join over a subquery that can return >1 row is nondeterministic when
	// scalar_subquery_error_on_multiple_rows=false (DuckDB returns an arbitrary row; by default it errors).
	// LPTS's decorrelation can't reproduce that arbitrary choice, so a divergence here is nondeterminism,
	// not a translation bug — excuse it, but only if the sides actually differ (a genuinely single-row run
	// still matches and logs OK).
	if (!run.suppress && PlanHasMultiRowSingleJoin(*original)) {
		run.suppress_on_mismatch = true;
		run.suppress_on_mismatch_reason = "scalar subquery may return >1 row (arbitrary row / error semantics)";
	}

	// Build UNION(hash pass-through, cmp sink). The original rows stream through unbuffered; both sides
	// signal done (hash via OperatorFinalize, cmp via sink Finalize) and the last one out takes the
	// verdict. If LPTS cannot rewrite the query: LOG logs UNSUPPORTED (a deliberate LPTS_<CODE> refusal)
	// or FAIL (any other error — a translation bug) and runs it unchecked; STRICT raises a
	// single, matchable "unsupported" error.
	try {
		original->ResolveOperatorTypes();
		auto target_types = original->types;
		idx_t col_count = target_types.size();

		auto orig_for_lpts = PlanQuery(input.context, query);
		auto ast = LogicalPlanToAst(input.context, orig_for_lpts, dialect);
		auto cte_list = AstToCteList(*ast, dialect);
		string sql = cte_list->ToQuery(true);
		auto rewritten = PlanQuery(input.context, sql);
		rewritten->ResolveOperatorTypes();
		if (rewritten->types.size() != col_count) {
			throw InvalidInputException("LPTS check: rewritten query has %llu columns but the original has %llu",
			                            static_cast<unsigned long long>(rewritten->types.size()),
			                            static_cast<unsigned long long>(col_count));
		}

		vector<unique_ptr<LogicalOperator>> children;
		children.push_back(make_uniq<LogicalLptsCheckHash>(std::move(original), /*last_one_out=*/true));
		children.push_back(make_uniq<LogicalLptsCheckSink>(std::move(rewritten), target_types));
		idx_t union_index = input.optimizer.binder.GenerateTableIndex();
		auto union_op = make_uniq<LogicalSetOperation>(union_index, col_count, std::move(children),
		                                               LogicalOperatorType::LOGICAL_UNION,
		                                               /*setop_all=*/true, /*allow_out_of_order=*/false);
		union_op->ResolveOperatorTypes();
		plan = std::move(union_op);
	} catch (std::exception &e) {
		if (behavior == LptsCheckVerdict::STRICT) {
			throw InvalidInputException("LPTS check: unsupported query (LPTS could not check it): %s", e.what());
		}
		// LOG: record + run the query unchecked. A non-rewritable query is only ACCEPTABLE when the error
		// is a deliberate, LPTS-formatted "not supported" refusal (ThrowLptsNotImplemented codes
		// "LPTS_<CODE>: ...") — logged as UNSUPPORTED. Any other error means LPTS emitted SQL that did not
		// parse/bind — a translation BUG — logged as FAIL so the coverage gate treats it like WRONG.
		const string error_text = e.what();
		const bool intentional_lpts_refusal = error_text.find("\"exception_message\":\"LPTS_") != string::npos;
		LptsCheckAppendLog(run.log_path, run.qnum, intentional_lpts_refusal ? "UNSUPPORTED" : "FAIL");
		if (!plan) {
			plan = std::move(original);
		}
	}
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
	config.AddExtensionOption(
	    "lpts_check",
	    "When true, transparently verify every top-level SELECT: the query returns its "
	    "normal result, but LPTS rewrites it and, on a different result, raises 'LPTS check "
	    "failed' (an unsupported query raises a specific 'LPTS check: unsupported' error). If "
	    "the LPTS_CHECK_LOG environment variable points to a file, it instead logs one "
	    "'<query#> OK|WRONG|UNSUPPORTED|FAIL' line per SELECT and never raises (suite-coverage mode).",
	    LogicalType::BOOLEAN, Value::BOOLEAN(false), LptsCheckSetCallback);

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

	// Enable `EXPLAIN (FORMAT SQL) <query>`. Two cooperating hooks, no submodule changes:
	//   1. A parser override (FALLBACK mode: consulted before normal parsing, declines everything
	//      except our exact prefix) rewrites the query into a real ExplainStatement carrying the
	//      inner query via a sentinel constant — so it is a genuine EXPLAIN for every client.
	//   2. An optimizer extension recognizes that sentinel, runs LPTS, and replaces the explain
	//      output with the generated SQL.
	ParserExtension explain_sql_ext;
	explain_sql_ext.parser_override = LptsExplainSqlOverride;
	ParserExtension::Register(config, std::move(explain_sql_ext));
	config.SetOptionByName("allow_parser_override_extension", Value("fallback"));

	OptimizerExtension explain_sql_opt;
	explain_sql_opt.pre_optimize_function = LptsExplainSqlOptimize;
	OptimizerExtension::Register(config, std::move(explain_sql_opt));

	// lpts_check verification mode: rewrite each top-level SELECT (when enabled) into a UNION ALL of
	// the original result and the LPTS-rewritten result, hashing both and raising on divergence. Runs
	// as a post-optimize hook so the hand-built UNION + extension operators are not further rewritten.
	OptimizerExtension check_opt;
	check_opt.optimize_function = LptsCheckOptimize;
	OptimizerExtension::Register(config, std::move(check_opt));
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
