#pragma once

#include "sql_dialect.hpp"

#include "duckdb.hpp"
#include "duckdb/parser/group_by_node.hpp"
#include "duckdb/planner/bound_result_modifier.hpp"
#include "duckdb/planner/column_binding.hpp"

#include <functional>

namespace duckdb {

class BoundAggregateExpression;
class BoundFunctionExpression;
class BoundWindowExpression;
class Expression;
class TableFilter;

class LptsExpressionRenderer {
public:
	using BindingResolver = std::function<string(const ColumnBinding &, const char *)>;

	LptsExpressionRenderer(SqlDialect dialect, BindingResolver binding_resolver);

	string ExpressionToAliasedString(const unique_ptr<Expression> &expression) const;
	string OrderByToAliasedString(const BoundOrderByNode &order) const;
	string WindowExpressionToAliasedString(const BoundWindowExpression &window) const;
	bool TableFilterToSql(const TableFilter &filter, const string &column_name, string &result) const;

	static bool ExpressionContainsColumnRef(const Expression &expr);
	static string QuantileArgument(const BoundAggregateExpression &aggregate);
	// True when the quantile was bound descending (`WITHIN GROUP (ORDER BY x DESC)`, or a negative quantile
	// such as `quantile_disc(x, -0.5)` which DuckDB stores as abs(p) + desc). The renderer must emit an
	// explicit `ORDER BY <value> DESC` so the round-trip matches.
	static bool QuantileDesc(const BoundAggregateExpression &aggregate);
	static string ApproxQuantileArgument(const BoundAggregateExpression &aggregate);
	static string ReservoirQuantileArguments(const BoundAggregateExpression &aggregate);
	static bool IsQuantileAggregate(const string &agg_name);
	static string StripTablePrefix(const string &cte_column_name);
	static string StringAggSeparator(const BoundAggregateExpression &aggregate);
	// For a list_aggregate/list_aggr wrapping string_agg (how array_to_string compiles), returns the
	// trailing `, '<sep>'` argument recovered from the nested string_agg's bind data; empty otherwise.
	static string ListAggrStringAggSeparatorArg(const BoundFunctionExpression &func_expr);
	static string GroupingSetsToClause(const vector<string> &group_names, const vector<GroupingSet> &grouping_sets);

private:
	// Render a constant whose type contains an UNNAMED struct at some depth (e.g. list_zip's
	// STRUCT(DATE)[]): such a type cannot be written in SQL, so the value is rebuilt structurally —
	// row(...) / struct_pack(...) / [...] / map(...) — with leaf values going through the normal
	// constant path (keeping their type-fidelity CASTs).
	string RenderConstantContainingUnnamedStruct(const Value &value) const;
	string WindowFunctionName(const BoundWindowExpression &window) const;
	string WindowRangeFrameOffsetToAliasedString(const BoundWindowExpression &window,
	                                             const unique_ptr<Expression> &expr, bool preceding) const;
	string WindowFrameStartToAliasedString(const BoundWindowExpression &window, string &units) const;
	string WindowFrameEndToAliasedString(const BoundWindowExpression &window, string &units) const;

	SqlDialect dialect;
	BindingResolver binding_resolver;
	// While rendering a lambda body, maps a captured BoundReferenceExpression index to the rendered SQL of
	// the captured outer expression. A lambda that closes over an outer column (`x -> x + n`) binds that
	// column as a trailing "parameter"; it must render as the outer reference (via LPTS's column mapping),
	// not as a lambda parameter. Saved/restored around each lambda body so nested lambdas don't collide.
	mutable std::map<idx_t, string> lambda_captures;
	// While rendering a lambda body, maps a parameter's body index to the canonical parameter name used in
	// the emitted parameter list. A body reference can carry a DIFFERENT stored name for the same index
	// (list comprehensions alias the result reference, e.g. "result" for the `fruit` parameter); rendering
	// the ref's own name would emit an unbound identifier.
	mutable std::map<idx_t, string> lambda_param_names;
};

} // namespace duckdb
