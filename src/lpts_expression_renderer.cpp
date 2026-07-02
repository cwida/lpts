#include "lpts_expression_renderer.hpp"
#include "lpts_helpers.hpp"
#include "lpts_date_format.hpp"
#include "dialect_function_map.hpp"

#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_lambda_expression.hpp"
#include "duckdb/planner/expression/bound_lambdaref_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_unnest_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/function/lambda_functions.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/optional_filter.hpp"

namespace duckdb {

LptsExpressionRenderer::LptsExpressionRenderer(SqlDialect _dialect, BindingResolver _binding_resolver)
    : dialect(_dialect), binding_resolver(std::move(_binding_resolver)) {
}

static bool IsDateFormatFunction(const string &function_name) {
	return function_name == "strftime" || function_name == "strptime";
}

static bool UsesBigQueryDateFunctionArgumentOrder(const string &function_name, SqlDialect dialect) {
	return dialect == SqlDialect::BIGQUERY && IsDateFormatFunction(function_name);
}

static bool IsDuckDBListFunction(const string &name) {
	return name == "list_transform" || name == "array_transform" || name == "list_filter" || name == "array_filter" ||
	       name == "list_aggregate" || name == "array_aggregate" || name == "list_contains" ||
	       name == "array_contains" || name == "list_extract" || name == "array_extract" || name == "list_value";
}

static bool IsStringSplitFunction(const string &name) {
	return name == "string_split" || name == "str_split";
}

static bool DialectSupportsListFunction(SqlDialect dialect) {
	return dialect == SqlDialect::DUCKDB || dialect == SqlDialect::SPARK || dialect == SqlDialect::HIVE ||
	       dialect == SqlDialect::TRINO_PRESTO;
}

static bool DialectSupportsLambdaFunction(SqlDialect dialect) {
	return dialect == SqlDialect::DUCKDB || dialect == SqlDialect::SPARK || dialect == SqlDialect::HIVE ||
	       dialect == SqlDialect::TRINO_PRESTO;
}

static bool IsDuckDBDialect(SqlDialect dialect) {
	return dialect == SqlDialect::DUCKDB;
}

static void ValidateFunctionForDialect(const BoundFunctionExpression &func_expr, SqlDialect dialect) {
	if (IsDuckDBDialect(dialect)) {
		return;
	}
	const string &name = func_expr.function.name;
	if (func_expr.function.bind_lambda != nullptr && !DialectSupportsLambdaFunction(dialect)) {
		ThrowLptsNotImplemented("LPTS_UNSUPPORTED_FUNCTION", dialect, "function", name, "BOUND_FUNCTION",
		                        "no safe lambda syntax or list semantics for target dialect");
	}
	if (IsDuckDBListFunction(name) && !DialectSupportsListFunction(dialect)) {
		ThrowLptsNotImplemented("LPTS_UNSUPPORTED_FUNCTION", dialect, "function", name, "BOUND_FUNCTION",
		                        "no verified list/array equivalent for target dialect");
	}
	if (dialect == SqlDialect::MYSQL_MARIADB && IsStringSplitFunction(name)) {
		ThrowLptsNotImplemented("LPTS_UNSUPPORTED_FUNCTION", dialect, "function", name, "BOUND_FUNCTION",
		                        "no safe array-returning string split equivalent");
	}
}

static bool TryRenderConvertedDateFormat(const unique_ptr<Expression> &expression, SqlDialect dialect, string &result) {
	if (expression->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return false;
	}
	const auto &constant = expression->Cast<BoundConstantExpression>();
	if (constant.value.IsNull() || constant.value.type().id() != LogicalTypeId::VARCHAR) {
		return false;
	}
	string format = constant.value.GetValue<string>();
	if (!TryConvertDuckDBDateFormatForDialect(format, dialect, format)) {
		return false;
	}
	result = "'" + EscapeSingleQuotes(format) + "'";
	return true;
}

static bool UsesArrowLambdaSyntax(SqlDialect dialect) {
	return dialect == SqlDialect::SPARK || dialect == SqlDialect::HIVE || dialect == SqlDialect::TRINO_PRESTO;
}

static string RenderCastTargetType(const LogicalType &type, SqlDialect dialect) {
	if (IsDuckDBDialect(dialect)) {
		return type.ToString();
	}
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return "BOOLEAN";
	case LogicalTypeId::TINYINT:
		if (dialect == SqlDialect::POSTGRES || dialect == SqlDialect::BIGQUERY || dialect == SqlDialect::REDSHIFT) {
			ThrowLptsNotImplemented("LPTS_UNSUPPORTED_TYPE", dialect, "type", type.ToString(), "BOUND_CAST",
			                        "target dialect does not support TINYINT cast syntax");
		}
		return "TINYINT";
	case LogicalTypeId::SMALLINT:
		if (dialect == SqlDialect::BIGQUERY) {
			ThrowLptsNotImplemented("LPTS_UNSUPPORTED_TYPE", dialect, "type", type.ToString(), "BOUND_CAST",
			                        "target dialect does not support SMALLINT cast syntax");
		}
		return "SMALLINT";
	case LogicalTypeId::INTEGER:
		return "INTEGER";
	case LogicalTypeId::BIGINT:
		return "BIGINT";
	case LogicalTypeId::FLOAT:
		return "FLOAT";
	case LogicalTypeId::DOUBLE:
		return "DOUBLE";
	case LogicalTypeId::DECIMAL:
		return type.ToString();
	case LogicalTypeId::VARCHAR:
		return "VARCHAR";
	case LogicalTypeId::DATE:
		return "DATE";
	case LogicalTypeId::TIME:
		return "TIME";
	case LogicalTypeId::TIMESTAMP:
		return "TIMESTAMP";
	default:
		ThrowLptsNotImplemented("LPTS_UNSUPPORTED_TYPE", dialect, "type", type.ToString(), "BOUND_CAST",
		                        "no verified target dialect cast type mapping");
		return type.ToString();
	}
}

static string RenderConstantForDialect(const BoundConstantExpression &constant, SqlDialect dialect) {
	if (IsDuckDBDialect(dialect)) {
		return constant.ToString();
	}
	const auto &value = constant.value;
	if (value.IsNull()) {
		return "CAST(NULL AS " + RenderCastTargetType(value.type(), dialect) + ")";
	}
	switch (value.type().id()) {
	case LogicalTypeId::DATE:
		return "DATE '" + EscapeSingleQuotes(value.ToString()) + "'";
	case LogicalTypeId::TIMESTAMP:
		return "TIMESTAMP '" + EscapeSingleQuotes(value.ToString()) + "'";
	case LogicalTypeId::VARCHAR:
		return "'" + EscapeSingleQuotes(value.GetValue<string>()) + "'";
	default:
		return value.ToSQLString();
	}
}

static string RenderComparisonForDialect(const string &lhs, const string &rhs, ExpressionType comparison,
                                         SqlDialect dialect) {
	if (dialect == SqlDialect::SPARK && comparison == ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
		return "(" + lhs + " <=> " + rhs + ")";
	}
	if (dialect == SqlDialect::SPARK && comparison == ExpressionType::COMPARE_DISTINCT_FROM) {
		return "(NOT (" + lhs + " <=> " + rhs + "))";
	}
	return "(" + lhs + ") " + ExpressionTypeToOperator(comparison) + " (" + rhs + ")";
}

static void ValidateTryCastForDialect(SqlDialect dialect) {
	if (dialect == SqlDialect::POSTGRES || dialect == SqlDialect::HIVE || dialect == SqlDialect::BIGQUERY ||
	    dialect == SqlDialect::REDSHIFT || dialect == SqlDialect::MYSQL_MARIADB) {
		ThrowLptsNotImplemented("LPTS_DIALECT_SEMANTIC_RISK", dialect, "function", "TRY_CAST", "BOUND_CAST",
		                        "target dialect needs a dialect-specific TRY_CAST/SAFE_CAST rewrite");
	}
}

//--------------------------------------------------------------------------
// CollectLambdaParamNames
//
// Walk a lambda body to find BoundReferenceExpression nodes (the post-binding
// form of lambda parameters). Stops at nested lambda functions.
//--------------------------------------------------------------------------
static void CollectLambdaParamNames(const Expression &expr, std::map<idx_t, string> &names) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_REF) {
		auto &ref = expr.Cast<BoundReferenceExpression>();
		if (names.find(ref.index) == names.end()) {
			names[ref.index] = ref.alias.empty() ? ("p" + to_string(ref.index)) : ref.alias;
		}
		return;
	}
	// For nested lambda functions, only recurse into children (not bind_info)
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr.Cast<BoundFunctionExpression>();
		if (func.function.bind_lambda != nullptr) {
			for (auto &child : func.children) {
				CollectLambdaParamNames(*child, names);
			}
			return;
		}
	}
	ExpressionIterator::EnumerateChildren(const_cast<Expression &>(expr),
	                                      [&](Expression &child) { CollectLambdaParamNames(child, names); });
}

bool LptsExpressionRenderer::ExpressionContainsColumnRef(const Expression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		return true;
	}
	bool contains_column_ref = false;
	ExpressionIterator::EnumerateChildren(const_cast<Expression &>(expr), [&](Expression &child) {
		if (!contains_column_ref && ExpressionContainsColumnRef(child)) {
			contains_column_ref = true;
		}
	});
	return contains_column_ref;
}

bool LptsExpressionRenderer::TableFilterToSql(const TableFilter &filter, const string &column_name,
                                              string &result) const {
	switch (filter.filter_type) {
	case TableFilterType::DYNAMIC_FILTER:
		return false;
	case TableFilterType::OPTIONAL_FILTER: {
		auto &optional_filter = filter.Cast<OptionalFilter>();
		if (!optional_filter.child_filter) {
			return false;
		}
		return TableFilterToSql(*optional_filter.child_filter, column_name, result);
	}
	case TableFilterType::CONJUNCTION_AND: {
		auto &and_filter = filter.Cast<ConjunctionAndFilter>();
		vector<string> children;
		for (auto &child_filter : and_filter.child_filters) {
			string child_sql;
			if (TableFilterToSql(*child_filter, column_name, child_sql)) {
				children.push_back(std::move(child_sql));
			}
		}
		if (children.empty()) {
			return false;
		}
		result = VecToSeparatedList(children, " AND ");
		return true;
	}
	case TableFilterType::CONJUNCTION_OR: {
		auto &or_filter = filter.Cast<ConjunctionOrFilter>();
		vector<string> children;
		for (auto &child_filter : or_filter.child_filters) {
			string child_sql;
			if (!TableFilterToSql(*child_filter, column_name, child_sql)) {
				return false;
			}
			children.push_back(std::move(child_sql));
		}
		if (children.empty()) {
			return false;
		}
		result = VecToSeparatedList(children, " OR ");
		return true;
	}
	case TableFilterType::EXPRESSION_FILTER: {
		auto column_expr = make_uniq<BoundReferenceExpression>(column_name, LogicalType::INVALID, 0);
		auto filter_expr = filter.ToExpression(*column_expr);
		result = ExpressionToAliasedString(filter_expr);
		return true;
	}
	default:
		result = filter.ToString(column_name);
		return true;
	}
}

string LptsExpressionRenderer::QuantileArgument(const BoundAggregateExpression &aggregate) {
	// DuckDB does not expose quantile arguments through the aggregate children after binding.
	// Keep this layout-dependent access isolated so a future DuckDB upgrade has one place
	// to update if QuantileBindData changes.
	struct QuantileValueLayout {
		Value val;
		double dbl;
		hugeint_t integral;
		hugeint_t scaling;
	};
	struct QuantileBindDataLayout {
		void *vtable;
		vector<QuantileValueLayout> quantiles;
		vector<idx_t> order;
		bool desc;
	};
	const auto &bind_data = *reinterpret_cast<const QuantileBindDataLayout *>(aggregate.bind_info.get());
	if (bind_data.quantiles.size() == 1) {
		return bind_data.quantiles[0].val.ToSQLString();
	}
	vector<string> values;
	for (const auto &quantile : bind_data.quantiles) {
		values.push_back(quantile.val.ToSQLString());
	}
	return "[" + VecToSeparatedList(values) + "]";
}

string LptsExpressionRenderer::ApproxQuantileArgument(const BoundAggregateExpression &aggregate) {
	struct ApproxQuantileBindDataLayout {
		void *vtable;
		vector<float> quantiles;
	};
	const auto &bind_data = *reinterpret_cast<const ApproxQuantileBindDataLayout *>(aggregate.bind_info.get());
	if (bind_data.quantiles.size() == 1) {
		return Value::FLOAT(bind_data.quantiles[0]).ToSQLString();
	}
	vector<string> values;
	for (const auto quantile : bind_data.quantiles) {
		values.push_back(Value::FLOAT(quantile).ToSQLString());
	}
	return "[" + VecToSeparatedList(values) + "]";
}

string LptsExpressionRenderer::ReservoirQuantileArguments(const BoundAggregateExpression &aggregate) {
	struct ReservoirQuantileBindDataLayout {
		void *vtable;
		vector<double> quantiles;
		idx_t sample_size;
	};
	const auto &bind_data = *reinterpret_cast<const ReservoirQuantileBindDataLayout *>(aggregate.bind_info.get());
	vector<string> values;
	for (const auto quantile : bind_data.quantiles) {
		values.push_back(Value::DOUBLE(quantile).ToSQLString());
	}
	string quantile_arg = values.size() == 1 ? values[0] : "[" + VecToSeparatedList(values) + "]";
	if (bind_data.sample_size == 8192) {
		return quantile_arg;
	}
	return quantile_arg + ", " + to_string(bind_data.sample_size);
}

bool LptsExpressionRenderer::IsQuantileAggregate(const string &agg_name) {
	return agg_name == "quantile_cont" || agg_name == "quantile_disc";
}

string LptsExpressionRenderer::StripTablePrefix(const string &cte_column_name) {
	const auto underscore_pos = cte_column_name.find('_');
	if (underscore_pos != string::npos && underscore_pos + 1 < cte_column_name.size()) {
		return cte_column_name.substr(underscore_pos + 1);
	}
	return cte_column_name;
}

string LptsExpressionRenderer::StringAggSeparator(const BoundAggregateExpression &aggregate) {
	if (aggregate.function.name != "string_agg" || !aggregate.bind_info) {
		return string();
	}
	// DuckDB stores the separator in string_agg bind data instead of aggregate children.
	// Keep this layout-dependent access isolated; rendering_edges.test covers it.
	struct StringAggBindDataLayout {
		void *vtable;
		string sep;
	};
	return reinterpret_cast<const StringAggBindDataLayout *>(aggregate.bind_info.get())->sep;
}

string LptsExpressionRenderer::GroupingSetsToClause(const vector<string> &group_names,
                                                    const vector<GroupingSet> &grouping_sets) {
	if (grouping_sets.empty()) {
		return VecToSeparatedList(group_names);
	}
	if (grouping_sets.size() == 1) {
		const auto &grouping_set = grouping_sets[0];
		if (grouping_set.empty()) {
			return "()";
		}
		vector<string> set_names;
		for (idx_t group_idx : grouping_set) {
			if (group_idx >= group_names.size()) {
				throw InternalException("LPTS: GROUPING SETS group index out of bounds");
			}
			set_names.push_back(group_names[group_idx]);
		}
		return VecToSeparatedList(set_names);
	}

	vector<string> set_clauses;
	for (const auto &grouping_set : grouping_sets) {
		if (grouping_set.empty()) {
			set_clauses.push_back("()");
			continue;
		}
		vector<string> set_names;
		for (idx_t group_idx : grouping_set) {
			if (group_idx >= group_names.size()) {
				throw InternalException("LPTS: GROUPING SETS group index out of bounds");
			}
			set_names.push_back(group_names[group_idx]);
		}
		set_clauses.push_back("(" + VecToSeparatedList(set_names) + ")");
	}
	return "GROUPING SETS (" + VecToSeparatedList(set_clauses) + ")";
}

string LptsExpressionRenderer::OrderByToAliasedString(const BoundOrderByNode &order) const {
	std::ostringstream result;
	result << ExpressionToAliasedString(order.expression);
	switch (order.type) {
	case OrderType::DESCENDING:
		result << " DESC";
		break;
	case OrderType::ASCENDING:
		result << " ASC";
		break;
	case OrderType::ORDER_DEFAULT:
		break;
	default:
		ThrowLptsNotImplemented("LPTS_UNSUPPORTED_OPERATOR", dialect, "order_by_direction",
		                        std::to_string(static_cast<int>(order.type)), "BoundOrderByNode",
		                        "ORDER BY direction is not implemented by LPTS");
	}
	switch (order.null_order) {
	case OrderByNullType::NULLS_FIRST:
		result << " NULLS FIRST";
		break;
	case OrderByNullType::NULLS_LAST:
		result << " NULLS LAST";
		break;
	case OrderByNullType::ORDER_DEFAULT:
		break;
	default:
		ThrowLptsNotImplemented("LPTS_UNSUPPORTED_OPERATOR", dialect, "order_by_null_order",
		                        std::to_string(static_cast<int>(order.null_order)), "BoundOrderByNode",
		                        "ORDER BY null ordering is not implemented by LPTS");
	}
	return result.str();
}

string LptsExpressionRenderer::WindowFunctionName(const BoundWindowExpression &window) const {
	if (window.aggregate) {
		string agg_name = window.aggregate->name;
		if (agg_name == "sum_no_overflow") {
			return "sum";
		}
		if (agg_name == "count_star") {
			return "count";
		}
		return agg_name;
	}
	switch (window.GetExpressionType()) {
	case ExpressionType::WINDOW_ROW_NUMBER:
		return "row_number";
	case ExpressionType::WINDOW_RANK:
		return "rank";
	case ExpressionType::WINDOW_RANK_DENSE:
		return "dense_rank";
	case ExpressionType::WINDOW_PERCENT_RANK:
		return "percent_rank";
	case ExpressionType::WINDOW_CUME_DIST:
		return "cume_dist";
	case ExpressionType::WINDOW_NTILE:
		return "ntile";
	case ExpressionType::WINDOW_FIRST_VALUE:
		return "first_value";
	case ExpressionType::WINDOW_LAST_VALUE:
		return "last_value";
	case ExpressionType::WINDOW_NTH_VALUE:
		return "nth_value";
	case ExpressionType::WINDOW_LEAD:
		return "lead";
	case ExpressionType::WINDOW_LAG:
		return "lag";
	case ExpressionType::WINDOW_FILL:
		return "fill";
	default:
		ThrowLptsNotImplemented("LPTS_UNSUPPORTED_FUNCTION", dialect, "window_function",
		                        ExpressionTypeToString(window.GetExpressionType()), "BOUND_WINDOW",
		                        "window function is not implemented by LPTS");
	}
}

static string WindowFrameUnits(WindowBoundary boundary) {
	switch (boundary) {
	case WindowBoundary::CURRENT_ROW_ROWS:
	case WindowBoundary::EXPR_PRECEDING_ROWS:
	case WindowBoundary::EXPR_FOLLOWING_ROWS:
		return "ROWS";
	case WindowBoundary::CURRENT_ROW_RANGE:
	case WindowBoundary::EXPR_PRECEDING_RANGE:
	case WindowBoundary::EXPR_FOLLOWING_RANGE:
		return "RANGE";
	case WindowBoundary::CURRENT_ROW_GROUPS:
	case WindowBoundary::EXPR_PRECEDING_GROUPS:
	case WindowBoundary::EXPR_FOLLOWING_GROUPS:
		return "GROUPS";
	default:
		return "ROWS";
	}
}

string LptsExpressionRenderer::WindowRangeFrameOffsetToAliasedString(const BoundWindowExpression &window,
                                                                     const unique_ptr<Expression> &expr,
                                                                     bool preceding) const {
	if (!window.orders.empty() && expr && expr->GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		const auto &func_expr = expr->Cast<BoundFunctionExpression>();
		const string &func_name = func_expr.function.name;
		if (func_expr.children.size() == 2 && ((preceding && func_name == "-") || (!preceding && func_name == "+"))) {
			string order_expr = ExpressionToAliasedString(window.orders[0].expression);
			string left_expr = ExpressionToAliasedString(func_expr.children[0]);
			if (left_expr == order_expr) {
				return ExpressionToAliasedString(func_expr.children[1]);
			}
		}
	}
	return ExpressionToAliasedString(expr);
}

string LptsExpressionRenderer::WindowFrameStartToAliasedString(const BoundWindowExpression &window,
                                                               string &units) const {
	switch (window.start) {
	case WindowBoundary::CURRENT_ROW_RANGE:
	case WindowBoundary::CURRENT_ROW_ROWS:
	case WindowBoundary::CURRENT_ROW_GROUPS:
		units = WindowFrameUnits(window.start);
		return "CURRENT ROW";
	case WindowBoundary::UNBOUNDED_PRECEDING:
		if (window.end != WindowBoundary::CURRENT_ROW_RANGE) {
			return "UNBOUNDED PRECEDING";
		}
		return "";
	case WindowBoundary::EXPR_PRECEDING_ROWS:
	case WindowBoundary::EXPR_PRECEDING_GROUPS:
		units = WindowFrameUnits(window.start);
		return ExpressionToAliasedString(window.start_expr) + " PRECEDING";
	case WindowBoundary::EXPR_PRECEDING_RANGE:
		units = WindowFrameUnits(window.start);
		return WindowRangeFrameOffsetToAliasedString(window, window.start_expr, true) + " PRECEDING";
	case WindowBoundary::EXPR_FOLLOWING_ROWS:
	case WindowBoundary::EXPR_FOLLOWING_GROUPS:
		units = WindowFrameUnits(window.start);
		return ExpressionToAliasedString(window.start_expr) + " FOLLOWING";
	case WindowBoundary::EXPR_FOLLOWING_RANGE:
		units = WindowFrameUnits(window.start);
		return WindowRangeFrameOffsetToAliasedString(window, window.start_expr, false) + " FOLLOWING";
	case WindowBoundary::INVALID:
		return "";
	default:
		ThrowLptsNotImplemented("LPTS_UNSUPPORTED_OPERATOR", dialect, "window_frame_start",
		                        std::to_string(static_cast<int>(window.start)), "BOUND_WINDOW",
		                        "window frame start is not implemented by LPTS");
	}
}

string LptsExpressionRenderer::WindowFrameEndToAliasedString(const BoundWindowExpression &window, string &units) const {
	switch (window.end) {
	case WindowBoundary::CURRENT_ROW_RANGE:
		if (window.start != WindowBoundary::UNBOUNDED_PRECEDING && window.start != WindowBoundary::INVALID) {
			units = "RANGE";
			return "CURRENT ROW";
		}
		return "";
	case WindowBoundary::CURRENT_ROW_ROWS:
	case WindowBoundary::CURRENT_ROW_GROUPS:
		units = WindowFrameUnits(window.end);
		return "CURRENT ROW";
	case WindowBoundary::UNBOUNDED_PRECEDING:
		return "UNBOUNDED PRECEDING";
	case WindowBoundary::UNBOUNDED_FOLLOWING:
		return "UNBOUNDED FOLLOWING";
	case WindowBoundary::EXPR_PRECEDING_ROWS:
	case WindowBoundary::EXPR_PRECEDING_GROUPS:
		units = WindowFrameUnits(window.end);
		return ExpressionToAliasedString(window.end_expr) + " PRECEDING";
	case WindowBoundary::EXPR_PRECEDING_RANGE:
		units = WindowFrameUnits(window.end);
		return WindowRangeFrameOffsetToAliasedString(window, window.end_expr, true) + " PRECEDING";
	case WindowBoundary::EXPR_FOLLOWING_ROWS:
	case WindowBoundary::EXPR_FOLLOWING_GROUPS:
		units = WindowFrameUnits(window.end);
		return ExpressionToAliasedString(window.end_expr) + " FOLLOWING";
	case WindowBoundary::EXPR_FOLLOWING_RANGE:
		units = WindowFrameUnits(window.end);
		return WindowRangeFrameOffsetToAliasedString(window, window.end_expr, false) + " FOLLOWING";
	case WindowBoundary::INVALID:
		return "";
	default:
		ThrowLptsNotImplemented("LPTS_UNSUPPORTED_OPERATOR", dialect, "window_frame_end",
		                        std::to_string(static_cast<int>(window.end)), "BOUND_WINDOW",
		                        "window frame end is not implemented by LPTS");
	}
}

string LptsExpressionRenderer::WindowExpressionToAliasedString(const BoundWindowExpression &window) const {
	std::ostringstream result;
	result << WindowFunctionName(window) << "(";
	if (window.aggregate && window.aggregate->name == "count_star" && window.children.empty()) {
		result << "*";
	}
	for (idx_t i = 0; i < window.children.size(); i++) {
		if (i > 0) {
			result << ", ";
		}
		if (window.distinct && i == 0) {
			result << "DISTINCT ";
		}
		result << ExpressionToAliasedString(window.children[i]);
	}
	if (window.offset_expr) {
		if (!window.children.empty()) {
			result << ", ";
		}
		result << ExpressionToAliasedString(window.offset_expr);
	}
	if (window.default_expr) {
		if (!window.children.empty() || window.offset_expr) {
			result << ", ";
		}
		result << ExpressionToAliasedString(window.default_expr);
	}
	if (!window.arg_orders.empty()) {
		result << " ORDER BY ";
		for (idx_t i = 0; i < window.arg_orders.size(); i++) {
			if (i > 0) {
				result << ", ";
			}
			result << OrderByToAliasedString(window.arg_orders[i]);
		}
	}
	if (window.ignore_nulls) {
		result << " IGNORE NULLS";
	}
	if (window.filter_expr) {
		result << ") FILTER (WHERE " << ExpressionToAliasedString(window.filter_expr);
	}

	result << ") OVER (";
	string separator;
	if (!window.partitions.empty()) {
		result << "PARTITION BY ";
		for (idx_t i = 0; i < window.partitions.size(); i++) {
			if (i > 0) {
				result << ", ";
			}
			result << ExpressionToAliasedString(window.partitions[i]);
		}
		separator = " ";
	}
	if (!window.orders.empty()) {
		result << separator << "ORDER BY ";
		for (idx_t i = 0; i < window.orders.size(); i++) {
			if (i > 0) {
				result << ", ";
			}
			result << OrderByToAliasedString(window.orders[i]);
		}
		separator = " ";
	}

	string units = "ROWS";
	string frame_start = WindowFrameStartToAliasedString(window, units);
	string frame_end = WindowFrameEndToAliasedString(window, units);
	if (window.exclude_clause != WindowExcludeMode::NO_OTHER) {
		if (frame_start.empty()) {
			frame_start = "UNBOUNDED PRECEDING";
		}
		if (frame_end.empty()) {
			frame_end = "CURRENT ROW";
			units = "RANGE";
		}
	}
	if (!frame_start.empty() || !frame_end.empty()) {
		if (dialect == SqlDialect::SPARK && units == "GROUPS") {
			ThrowLptsNotImplemented("LPTS_UNSUPPORTED_OPERATOR", dialect, "window_frame_units", "GROUPS",
			                        "BOUND_WINDOW", "Spark SQL supports ROWS/RANGE frames but not GROUPS");
		}
		result << separator << units;
		if (!frame_start.empty() && !frame_end.empty()) {
			result << " BETWEEN " << frame_start << " AND " << frame_end;
		} else if (!frame_start.empty()) {
			result << " " << frame_start;
		} else {
			result << " " << frame_end;
		}
		separator = " ";
	}
	if (dialect == SqlDialect::SPARK && window.exclude_clause != WindowExcludeMode::NO_OTHER) {
		ThrowLptsNotImplemented("LPTS_UNSUPPORTED_OPERATOR", dialect, "window_exclude_clause",
		                        std::to_string(static_cast<int>(window.exclude_clause)), "BOUND_WINDOW",
		                        "Spark SQL does not support window EXCLUDE clauses");
	}
	switch (window.exclude_clause) {
	case WindowExcludeMode::NO_OTHER:
		break;
	case WindowExcludeMode::CURRENT_ROW:
		result << separator << "EXCLUDE CURRENT ROW";
		break;
	case WindowExcludeMode::GROUP:
		result << separator << "EXCLUDE GROUP";
		break;
	case WindowExcludeMode::TIES:
		result << separator << "EXCLUDE TIES";
		break;
	default:
		ThrowLptsNotImplemented("LPTS_UNSUPPORTED_OPERATOR", dialect, "window_exclude_clause",
		                        std::to_string(static_cast<int>(window.exclude_clause)), "BOUND_WINDOW",
		                        "window exclude mode is not implemented by LPTS");
	}
	result << ")";
	return result.str();
}

//--------------------------------------------------------------------------
// ExpressionToAliasedString
//
// Converts a bound DuckDB expression into a SQL string, replacing internal
// ColumnBinding references with their CTE column names via column_map.
// Converts a bound Expression into a SQL string.
//--------------------------------------------------------------------------
string LptsExpressionRenderer::ExpressionToAliasedString(const unique_ptr<Expression> &expression) const {
	const ExpressionClass e_class = expression->GetExpressionClass();
	std::ostringstream expr_str;
	switch (e_class) {
	case ExpressionClass::BOUND_COLUMN_REF: {
		const BoundColumnRefExpression &bcr = expression->Cast<BoundColumnRefExpression>();
		expr_str << binding_resolver(bcr.binding, "expression");
		break;
	}
	case ExpressionClass::BOUND_CONSTANT: {
		const BoundConstantExpression &constant = expression->Cast<BoundConstantExpression>();
		expr_str << RenderConstantForDialect(constant, dialect);
		break;
	}
	case ExpressionClass::BOUND_COMPARISON: {
		const BoundComparisonExpression &cmp = expression->Cast<BoundComparisonExpression>();
		expr_str << RenderComparisonForDialect(ExpressionToAliasedString(cmp.left), ExpressionToAliasedString(cmp.right),
		                                       cmp.GetExpressionType(), dialect);
		break;
	}
	case ExpressionClass::BOUND_BETWEEN: {
		const BoundBetweenExpression &between_expr = expression->Cast<BoundBetweenExpression>();
		string input = ExpressionToAliasedString(between_expr.input);
		string lower_op = ExpressionTypeToOperator(between_expr.LowerComparisonType());
		string upper_op = ExpressionTypeToOperator(between_expr.UpperComparisonType());
		expr_str << "((" << input << ") " << lower_op << " (" << ExpressionToAliasedString(between_expr.lower)
		         << ")) AND ((" << input << ") " << upper_op << " (" << ExpressionToAliasedString(between_expr.upper)
		         << "))";
		break;
	}
	case ExpressionClass::BOUND_CAST: {
		const BoundCastExpression &cast_expr = expression->Cast<BoundCastExpression>();
		if (cast_expr.try_cast) {
			ValidateTryCastForDialect(dialect);
		}
		expr_str << (cast_expr.try_cast ? "TRY_CAST(" : "CAST(");
		expr_str << ExpressionToAliasedString(cast_expr.child);
		expr_str << " AS " + RenderCastTargetType(cast_expr.return_type, dialect) + ")";
		break;
	}
	case ExpressionClass::BOUND_CONJUNCTION: {
		const BoundConjunctionExpression &conj = expression->Cast<BoundConjunctionExpression>();
		// BoundConjunctionExpression can have N children (N ≥ 2).
		// Serialize as: (child[0]) OP (child[1]) OP ... OP (child[N-1]).
		string op = ExpressionTypeToOperator(conj.GetExpressionType());
		expr_str << "(";
		expr_str << ExpressionToAliasedString(conj.children[0]);
		expr_str << ")";
		for (size_t ci = 1; ci < conj.children.size(); ci++) {
			expr_str << " " << op << " (";
			expr_str << ExpressionToAliasedString(conj.children[ci]);
			expr_str << ")";
		}
		break;
	}
	case ExpressionClass::BOUND_FUNCTION: {
		const BoundFunctionExpression &func_expr = expression->Cast<BoundFunctionExpression>();
		// Strip internal compress/decompress wrappers injected by COMPRESSED_MATERIALIZATION.
		// For non-BOUND_COLUMN_REF GROUP BY keys (e.g. COALESCE), the optimizer inlines
		// __internal_compress_* directly into the aggregate's group expression instead of
		// creating a separate projection. Render the first argument (the original expression)
		// so the generated SQL stays valid and binder-accepted.
		if (!func_expr.children.empty() && (func_expr.function.name.rfind("__internal_compress_", 0) == 0 ||
		                                    func_expr.function.name.rfind("__internal_decompress_", 0) == 0)) {
			expr_str << ExpressionToAliasedString(func_expr.children[0]);
			break;
		}
		ValidateFunctionForDialect(func_expr, dialect);
		// Dialect-specific function name remapping (see dialect_function_map.hpp).
		string func_name = RemapFunctionNameForDialect(func_expr.function.name, dialect);
		// For lambda functions, only serialize non-lambda, non-capture children
		idx_t child_count = func_expr.children.size();
		if (func_expr.function.bind_lambda != nullptr) {
			child_count = 0;
			for (auto &arg_type : func_expr.function.arguments) {
				if (arg_type.id() != LogicalTypeId::LAMBDA) {
					child_count++;
				}
			}
			if (child_count > func_expr.children.size()) {
				child_count = func_expr.children.size();
			}
		}
		// Operators: use infix notation (e.g. "a + b").
		// Some plan rewrites (like AVG decomposition) create operator functions
		// without setting is_operator. Fall back to name-based detection.
		bool is_infix =
		    func_expr.is_operator || (child_count == 2 && (func_name == "/" || func_name == "+" || func_name == "-" ||
		                                                   func_name == "*" || func_name == "%" || func_name == "||"));
		if (is_infix && child_count == 2) {
			expr_str << "(";
			expr_str << ExpressionToAliasedString(func_expr.children[0]);
			expr_str << " " << func_name << " ";
			expr_str << ExpressionToAliasedString(func_expr.children[1]);
			expr_str << ")";
		} else {
			// struct_pack uses `field := expr` syntax; field names live in the
			// return type, not in the children. Emitting bare children loses the
			// names — the re-binder then uses each child's column alias (e.g.
			// "t0_I_NAME") as the struct field name.
			const bool is_struct_pack = (func_name == "struct_pack" || func_name == "row") &&
			                            func_expr.return_type.id() == LogicalTypeId::STRUCT &&
			                            !StructType::IsUnnamed(func_expr.return_type);
			// Some function names collide with SQL keywords (POSITION, SUBSTRING,
			// OVERLAY, TRIM). DuckDB's parser rejects them as plain identifiers and
			// expects the keyword-separator syntax (`POSITION(x IN y)`), but it
			// accepts them as quoted identifiers (`"position"(x, y)`) — let the
			// function resolver see the function call directly and bypass the
			// keyword-syntax path.
			string emit_name = func_name;
			if (func_name == "position" || func_name == "substring" || func_name == "overlay" || func_name == "trim") {
				emit_name = "\"" + func_name + "\"";
			}
			expr_str << emit_name << "(";
			if (UsesBigQueryDateFunctionArgumentOrder(func_expr.function.name, dialect) && child_count >= 2) {
				expr_str << ExpressionToAliasedString(func_expr.children[1]);
				expr_str << ", ";
				expr_str << ExpressionToAliasedString(func_expr.children[0]);
				for (idx_t i = 2; i < child_count; i++) {
					expr_str << ", ";
					expr_str << ExpressionToAliasedString(func_expr.children[i]);
				}
			} else {
				for (idx_t i = 0; i < child_count; i++) {
					if (i > 0) {
						expr_str << ", ";
					}
					if (is_struct_pack && i < StructType::GetChildCount(func_expr.return_type)) {
						expr_str << "\"" << StructType::GetChildName(func_expr.return_type, i) << "\" := ";
					}
					string converted_date_format;
					if (i == 1 && IsDateFormatFunction(func_expr.function.name) &&
					    TryRenderConvertedDateFormat(func_expr.children[i], dialect, converted_date_format)) {
						expr_str << converted_date_format;
					} else {
						expr_str << ExpressionToAliasedString(func_expr.children[i]);
					}
				}
			}
			// Lambda function: serialize the lambda expression from bind_info
			if (func_expr.function.bind_lambda != nullptr && func_expr.bind_info) {
				auto &bind_data = func_expr.bind_info->Cast<ListLambdaBindData>();
				if (bind_data.lambda_expr) {
					if (child_count > 0) {
						expr_str << ", ";
					}
					std::map<idx_t, string> param_map;
					CollectLambdaParamNames(*bind_data.lambda_expr, param_map);
					idx_t param_count = param_map.empty() ? 0 : param_map.rbegin()->first + 1;
					vector<string> param_names(param_count);
					for (auto &entry : param_map) {
						if (entry.first < param_count) {
							param_names[param_count - 1 - entry.first] = entry.second;
						}
					}
					for (idx_t i = 0; i < param_count; i++) {
						if (param_names[i].empty()) {
							param_names[i] = "p" + to_string(i);
						}
					}
					if (UsesArrowLambdaSyntax(dialect)) {
						if (param_count == 1) {
							expr_str << param_names[0];
						} else {
							expr_str << "(";
							for (idx_t i = 0; i < param_count; i++) {
								if (i > 0) {
									expr_str << ", ";
								}
								expr_str << param_names[i];
							}
							expr_str << ")";
						}
						expr_str << " -> ";
					} else {
						expr_str << "lambda ";
						for (idx_t i = 0; i < param_count; i++) {
							if (i > 0) {
								expr_str << ", ";
							}
							expr_str << param_names[i];
						}
						expr_str << ": ";
					}
					expr_str << ExpressionToAliasedString(bind_data.lambda_expr);
				}
			}
			expr_str << ")";
		}
		break;
	}
	case ExpressionClass::BOUND_REF: {
		expr_str << expression->ToString();
		break;
	}
	case ExpressionClass::BOUND_LAMBDA_REF: {
		expr_str << expression->ToString();
		break;
	}
	case ExpressionClass::BOUND_CASE: {
		const BoundCaseExpression &case_expr = expression->Cast<BoundCaseExpression>();
		expr_str << "CASE";
		for (auto &check : case_expr.case_checks) {
			expr_str << " WHEN " << ExpressionToAliasedString(check.when_expr);
			expr_str << " THEN " << ExpressionToAliasedString(check.then_expr);
		}
		if (case_expr.else_expr) {
			expr_str << " ELSE " << ExpressionToAliasedString(case_expr.else_expr);
		}
		expr_str << " END";
		break;
	}
	case ExpressionClass::BOUND_OPERATOR: {
		const BoundOperatorExpression &op_expr = expression->Cast<BoundOperatorExpression>();
		switch (op_expr.GetExpressionType()) {
		case ExpressionType::OPERATOR_IS_NULL:
			expr_str << "(" << ExpressionToAliasedString(op_expr.children[0]) << ") IS NULL";
			break;
		case ExpressionType::OPERATOR_IS_NOT_NULL:
			expr_str << "(" << ExpressionToAliasedString(op_expr.children[0]) << ") IS NOT NULL";
			break;
		case ExpressionType::OPERATOR_NOT:
			expr_str << "NOT (" << ExpressionToAliasedString(op_expr.children[0]) << ")";
			break;
		case ExpressionType::COMPARE_IN:
		case ExpressionType::COMPARE_NOT_IN: {
			const bool is_not_in = op_expr.GetExpressionType() == ExpressionType::COMPARE_NOT_IN;
			expr_str << "(" << ExpressionToAliasedString(op_expr.children[0]) << ")";
			expr_str << (is_not_in ? " NOT IN (" : " IN (");
			for (idx_t i = 1; i < op_expr.children.size(); i++) {
				if (i > 1) {
					expr_str << ", ";
				}
				expr_str << ExpressionToAliasedString(op_expr.children[i]);
			}
			expr_str << ")";
			break;
		}
		case ExpressionType::OPERATOR_COALESCE: {
			expr_str << "COALESCE(";
			for (idx_t i = 0; i < op_expr.children.size(); i++) {
				if (i > 0) {
					expr_str << ", ";
				}
				expr_str << ExpressionToAliasedString(op_expr.children[i]);
			}
			expr_str << ")";
			break;
		}
		case ExpressionType::OPERATOR_NULLIF:
			expr_str << "NULLIF(" << ExpressionToAliasedString(op_expr.children[0]) << ", "
			         << ExpressionToAliasedString(op_expr.children[1]) << ")";
			break;
		case ExpressionType::OPERATOR_TRY:
			if (!IsDuckDBDialect(dialect)) {
				ThrowLptsNotImplemented("LPTS_DIALECT_SEMANTIC_RISK", dialect, "operator", "TRY", "BOUND_OPERATOR",
				                        "TRY expression has dialect-specific semantics");
			}
			expr_str << "TRY(" << ExpressionToAliasedString(op_expr.children[0]) << ")";
			break;
		default:
			ThrowLptsNotImplemented("LPTS_UNSUPPORTED_OPERATOR", dialect, "expression_operator",
			                        ExpressionTypeToString(op_expr.GetExpressionType()), "BOUND_OPERATOR",
			                        "expression operator is not implemented by LPTS");
		}
		break;
	}
	case ExpressionClass::BOUND_UNNEST: {
		const BoundUnnestExpression &unnest_expr = expression->Cast<BoundUnnestExpression>();
		expr_str << "UNNEST(" << ExpressionToAliasedString(unnest_expr.child) << ")";
		break;
	}
	case ExpressionClass::BOUND_WINDOW: {
		const BoundWindowExpression &window_expr = expression->Cast<BoundWindowExpression>();
		expr_str << WindowExpressionToAliasedString(window_expr);
		break;
	}
	default:
		ThrowLptsNotImplemented("LPTS_UNSUPPORTED_OPERATOR", dialect, "expression_class",
		                        ExpressionTypeToString(expression->type), "ExpressionToAliasedString",
		                        "expression class is not implemented by LPTS");
	}
	return expr_str.str();
}

} // namespace duckdb
