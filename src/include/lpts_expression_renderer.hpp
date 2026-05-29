#pragma once

#include "sql_dialect.hpp"

#include "duckdb.hpp"
#include "duckdb/parser/group_by_node.hpp"
#include "duckdb/planner/bound_result_modifier.hpp"
#include "duckdb/planner/column_binding.hpp"

#include <functional>

namespace duckdb {

class BoundAggregateExpression;
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
	static string ApproxQuantileArgument(const BoundAggregateExpression &aggregate);
	static string ReservoirQuantileArguments(const BoundAggregateExpression &aggregate);
	static bool IsQuantileAggregate(const string &agg_name);
	static string StripTablePrefix(const string &cte_column_name);
	static string StringAggSeparator(const BoundAggregateExpression &aggregate);
	static string GroupingSetsToClause(const vector<string> &group_names, const vector<GroupingSet> &grouping_sets);

private:
	string WindowFunctionName(const BoundWindowExpression &window) const;
	string WindowRangeFrameOffsetToAliasedString(const BoundWindowExpression &window,
	                                             const unique_ptr<Expression> &expr, bool preceding) const;
	string WindowFrameStartToAliasedString(const BoundWindowExpression &window, string &units) const;
	string WindowFrameEndToAliasedString(const BoundWindowExpression &window, string &units) const;

	SqlDialect dialect;
	BindingResolver binding_resolver;
};

} // namespace duckdb
