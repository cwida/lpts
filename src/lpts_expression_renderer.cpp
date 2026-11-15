#include "lpts_expression_renderer.hpp"
#include "lpts_helpers.hpp"
#include "lpts_date_format.hpp"
#include "dialect_function_map.hpp"

#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"

#include <map>
#include <set>
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
#include "duckdb/planner/bound_expression_sql_exporter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/function/lambda_functions.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/filter/optional_filter.hpp"
#include "duckdb/function/scalar/generic_common.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/parser.hpp"

#include <limits>

namespace duckdb {

LptsExpressionRenderer::LptsExpressionRenderer(ClientContext &_context, SqlDialect _dialect,
                                               BindingResolver _binding_resolver)
    : context(_context), dialect(_dialect), binding_resolver(std::move(_binding_resolver)) {
}

static const LogicalType &NestedSequenceChildType(const LogicalType &type) {
	D_ASSERT(type.id() == LogicalTypeId::LIST || type.id() == LogicalTypeId::ARRAY);
	return type.id() == LogicalTypeId::ARRAY ? ArrayType::GetChildType(type) : ListType::GetChildType(type);
}

static const vector<Value> &NestedSequenceChildren(const Value &value) {
	D_ASSERT(value.type().id() == LogicalTypeId::LIST || value.type().id() == LogicalTypeId::ARRAY);
	return value.type().id() == LogicalTypeId::ARRAY ? ArrayValue::GetChildren(value) : ListValue::GetChildren(value);
}

// A folded constant rendered bare (or via Value::ToSQLString's default) re-parses to a *different* type
// than the original: a small TINYINT/HUGEINT shows as INTEGER, a FLOAT as DOUBLE, a BIT `00000001` as the
// integer 1. Since the round-trip bag-hash is type-sensitive, these need an explicit CAST to pin the type.
// Types whose literal already self-describes (INTEGER, DOUBLE, VARCHAR, BOOLEAN, DATE/TIME/TS, BLOB, ...)
// do not, so we leave them bare to keep the generated SQL readable.
// VARIANT (anywhere in the type, including inside LIST/ARRAY/STRUCT/MAP) has no faithful re-parseable SQL
// literal, so a value of such a type is untranslatable.
static bool TypeContainsVariant(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::VARIANT:
		return true;
	case LogicalTypeId::LIST:
	case LogicalTypeId::ARRAY:
		return TypeContainsVariant(NestedSequenceChildType(type));
	case LogicalTypeId::STRUCT: {
		for (const auto &child : StructType::GetChildTypes(type)) {
			if (TypeContainsVariant(child.second)) {
				return true;
			}
		}
		return false;
	}
	case LogicalTypeId::MAP:
		return TypeContainsVariant(MapType::KeyType(type)) || TypeContainsVariant(MapType::ValueType(type));
	default:
		return false;
	}
}

static bool TypeContainsEmptyEnum(const LogicalType &type) {
	if (type.id() == LogicalTypeId::ENUM && EnumType::GetSize(type) == 0) {
		return true;
	}
	switch (type.id()) {
	case LogicalTypeId::LIST:
	case LogicalTypeId::ARRAY:
		return TypeContainsEmptyEnum(NestedSequenceChildType(type));
	case LogicalTypeId::STRUCT:
		for (const auto &child : StructType::GetChildTypes(type)) {
			if (TypeContainsEmptyEnum(child.second)) {
				return true;
			}
		}
		return false;
	case LogicalTypeId::MAP:
		return TypeContainsEmptyEnum(MapType::KeyType(type)) || TypeContainsEmptyEnum(MapType::ValueType(type));
	default:
		return false;
	}
}

// An UNNAMED struct type (a ROW value, e.g. produced by `(0, 0)` or list_zip's entries) cannot be written
// in SQL — `STRUCT(INTEGER, INTEGER)` has no field names and does not parse — so neither a bare literal
// nor a CAST('<text>' AS <type>) can express a constant containing one. Such values are rebuilt
// structurally instead (see RenderConstantContainingUnnamedStruct).
static bool TypeContainsUnnamedStruct(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::LIST:
	case LogicalTypeId::ARRAY:
		return TypeContainsUnnamedStruct(NestedSequenceChildType(type));
	case LogicalTypeId::STRUCT: {
		if (StructType::IsUnnamed(type)) {
			return true;
		}
		for (const auto &child : StructType::GetChildTypes(type)) {
			if (TypeContainsUnnamedStruct(child.second)) {
				return true;
			}
		}
		return false;
	}
	case LogicalTypeId::MAP:
		return TypeContainsUnnamedStruct(MapType::KeyType(type)) || TypeContainsUnnamedStruct(MapType::ValueType(type));
	default:
		return false;
	}
}

// Structural type equality that ignores struct field names (`STRUCT(k INT, v INT)` vs
// `STRUCT(INTEGER, INTEGER)`) and treats an untyped-NULL slot as compatible with anything: used to
// recognize casts that are no-ops for re-binding (name-stripping / NULL-slot-typing only).
static bool TypesEqualIgnoringStructNames(const LogicalType &a, const LogicalType &b) {
	if (a.id() == LogicalTypeId::SQLNULL || b.id() == LogicalTypeId::SQLNULL) {
		return true;
	}
	if (a.id() != b.id()) {
		return false;
	}
	switch (a.id()) {
	case LogicalTypeId::LIST:
	case LogicalTypeId::ARRAY:
		return TypesEqualIgnoringStructNames(NestedSequenceChildType(a), NestedSequenceChildType(b));
	case LogicalTypeId::STRUCT: {
		const auto &ac = StructType::GetChildTypes(a);
		const auto &bc = StructType::GetChildTypes(b);
		if (ac.size() != bc.size()) {
			return false;
		}
		for (idx_t i = 0; i < ac.size(); i++) {
			if (!TypesEqualIgnoringStructNames(ac[i].second, bc[i].second)) {
				return false;
			}
		}
		return true;
	}
	case LogicalTypeId::MAP:
		return TypesEqualIgnoringStructNames(MapType::KeyType(a), MapType::KeyType(b)) &&
		       TypesEqualIgnoringStructNames(MapType::ValueType(a), MapType::ValueType(b));
	default:
		return a == b;
	}
}

// True when the type is (or nests) the untyped-NULL type, whose rendering `"NULL"` does not parse.
static bool TypeContainsNullType(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::SQLNULL:
		return true;
	case LogicalTypeId::LIST:
	case LogicalTypeId::ARRAY:
		return TypeContainsNullType(NestedSequenceChildType(type));
	case LogicalTypeId::STRUCT: {
		for (const auto &child : StructType::GetChildTypes(type)) {
			if (TypeContainsNullType(child.second)) {
				return true;
			}
		}
		return false;
	}
	case LogicalTypeId::MAP:
		return TypeContainsNullType(MapType::KeyType(type)) || TypeContainsNullType(MapType::ValueType(type));
	default:
		return false;
	}
}

static bool IsDateFormatFunction(const string &function_name) {
	return function_name == "strftime" || function_name == "strptime";
}

static bool UsesFormatFirstDateFunctionArgumentOrder(const string &function_name, SqlDialect dialect) {
	return (dialect == SqlDialect::BIGQUERY || dialect == SqlDialect::FELDERA) && IsDateFormatFunction(function_name);
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
	const string &name = func_expr.Function().GetName().GetIdentifierName();
	if (func_expr.Function().GetCallbacks().bind_lambda != nullptr && !DialectSupportsLambdaFunction(dialect)) {
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
	if (constant.GetValue().IsNull() || constant.GetValue().type().id() != LogicalTypeId::VARCHAR) {
		return false;
	}
	string format = constant.GetValue().GetValue<string>();
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
	// Types with no SQL syntax cannot be a cast target in ANY dialect: an unnamed struct
	// (`STRUCT(INTEGER, INTEGER)`, from row(...) values — only reachable here when the cast is NOT a
	// names-only no-op, see BOUND_CAST) and internal aggregate-state types (`AGGREGATE_STATE<sum(...)>`).
	if (TypeContainsUnnamedStruct(type) || TypeContainsAggregateState(type)) {
		ThrowLptsNotImplemented("LPTS_UNSUPPORTED_TYPE", dialect, "type", type.ToString(), "BOUND_CAST",
		                        "the cast target type cannot be written in SQL");
	}
	if (TypeContainsEmptyEnum(type)) {
		ThrowLptsNotImplemented("LPTS_UNSUPPORTED_TYPE", dialect, "type", type.ToString(), "BOUND_CAST",
		                        "an empty ENUM has no inline SQL type representation");
	}
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
		return dialect == SqlDialect::SPARK ? "STRING" : "VARCHAR";
	case LogicalTypeId::DATE:
		return "DATE";
	case LogicalTypeId::TIME:
		return "TIME";
	case LogicalTypeId::TIMESTAMP:
		return "TIMESTAMP";
	case LogicalTypeId::TIMESTAMP_TZ:
		if (dialect == SqlDialect::SPARK) {
			// Both types represent an absolute instant and render it in the session time zone.
			return "TIMESTAMP_LTZ";
		}
		ThrowLptsNotImplemented("LPTS_UNSUPPORTED_TYPE", dialect, "type", type.ToString(), "BOUND_CAST",
		                        "target dialect has no verified timestamp-with-local-time-zone cast type");
		return type.ToString();
	default:
		ThrowLptsNotImplemented("LPTS_UNSUPPORTED_TYPE", dialect, "type", type.ToString(), "BOUND_CAST",
		                        "no verified target dialect cast type mapping");
		return type.ToString();
	}
}

static bool SparkCanRepresentHugeintCastAsDecimal38(const LogicalType &source_type) {
	switch (source_type.id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::SQLNULL:
		// Every value in these source domains fits in Spark's exact DECIMAL(38,0).
		// Do not apply this to HUGEINT itself: DuckDB's signed 128-bit range needs
		// 39 decimal digits, while Spark DECIMAL is capped at precision 38.
		return true;
	default:
		return false;
	}
}

static string RenderValueForDialect(const Value &value, SqlDialect dialect) {
	if (TypeContainsAggregateState(value.type())) {
		ThrowLptsNotImplemented("LPTS_UNSUPPORTED_TYPE", dialect, "type", value.type().ToString(), "BOUND_CONSTANT",
		                        "an aggregate-state constant does not retain the SQL aggregate needed to rebuild it");
	}
	if (value.IsNull()) {
		// An untyped NULL literal (SQLNULL) has no target-dialect cast type; emit a
		// bare NULL, which is valid in every non-DuckDB dialect. Casting it to its own
		// "NULL" type would hit the RenderCastTargetType whitelist and fail (e.g. the
		// to_date/to_timestamp shim's CASE ... THEN NULL branches). Typed NULLs keep
		// their explicit CAST so downstream type inference is preserved.
		if (value.type().id() == LogicalTypeId::SQLNULL) {
			return "NULL";
		}
		if (dialect == SqlDialect::SPARK && value.type().id() == LogicalTypeId::HUGEINT) {
			return "CAST(NULL AS DECIMAL(38,0))";
		}
		return "CAST(NULL AS " + RenderCastTargetType(value.type(), dialect) + ")";
	}
	switch (value.type().id()) {
	case LogicalTypeId::LIST:
	case LogicalTypeId::ARRAY: {
		if (dialect != SqlDialect::SPARK) {
			return value.ToSQLString();
		}
		const auto &children = NestedSequenceChildren(value);
		vector<string> elements;
		elements.reserve(children.size());
		for (const auto &child : children) {
			elements.push_back(RenderValueForDialect(child, dialect));
		}
		return "array(" + StringUtil::Join(elements, ", ") + ")";
	}
	case LogicalTypeId::DATE:
		return "DATE '" + EscapeSingleQuotes(value.ToString()) + "'";
	case LogicalTypeId::TIMESTAMP:
		return "TIMESTAMP '" + EscapeSingleQuotes(value.ToString()) + "'";
	case LogicalTypeId::TIMESTAMP_TZ:
		if (dialect == SqlDialect::SPARK) {
			return "CAST('" + EscapeSingleQuotes(value.ToString()) + "' AS TIMESTAMP_LTZ)";
		}
		return value.ToSQLString();
	case LogicalTypeId::HUGEINT:
		if (dialect == SqlDialect::SPARK) {
			const string integer = value.ToString();
			const idx_t digits = !integer.empty() && integer[0] == '-' ? integer.size() - 1 : integer.size();
			if (digits <= 38) {
				return "CAST(" + integer + " AS DECIMAL(38,0))";
			}
			ThrowLptsNotImplemented("LPTS_UNSUPPORTED_TYPE", dialect, "type", value.type().ToString(), "BOUND_CONSTANT",
			                        "the HUGEINT constant needs 39 digits, beyond Spark DECIMAL(38,0)");
		}
		return value.ToSQLString();
	case LogicalTypeId::VARCHAR:
		return "'" + EscapeSingleQuotes(value.GetValue<string>()) + "'";
	case LogicalTypeId::INTERVAL: {
		const auto interval = value.GetValue<interval_t>();
		vector<string> parts;
		if (interval.months != 0) {
			parts.push_back("INTERVAL '" + std::to_string(interval.months) + "' MONTH");
		}
		if (interval.days != 0) {
			parts.push_back("INTERVAL '" + std::to_string(interval.days) + "' DAY");
		}
		if (interval.micros != 0) {
			const bool negative = interval.micros < 0;
			const uint64_t magnitude = negative ? uint64_t(-(interval.micros + 1)) + 1 : uint64_t(interval.micros);
			const uint64_t seconds = magnitude / Interval::MICROS_PER_SEC;
			const uint64_t micros = magnitude % Interval::MICROS_PER_SEC;
			string seconds_text = (negative ? "-" : "") + std::to_string(seconds);
			if (micros != 0) {
				string fraction = StringUtil::Format("%06llu", (unsigned long long)micros);
				while (fraction.back() == '0') {
					fraction.pop_back();
				}
				seconds_text += "." + fraction;
			}
			parts.push_back("INTERVAL '" + seconds_text + "' SECOND");
		}
		if (parts.empty()) {
			return "INTERVAL '0' SECOND";
		}
		return parts.size() == 1 ? parts[0] : "(" + StringUtil::Join(parts, " + ") + ")";
	}
	default:
		return value.ToSQLString();
	}
}

static string RenderConstantForDialect(const BoundConstantExpression &constant, SqlDialect dialect) {
	if (IsDuckDBDialect(dialect)) {
		return ConstantExpression::FromValue(constant.GetValue())->ToString();
	}
	return RenderValueForDialect(constant.GetValue(), dialect);
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
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_LAMBDA) {
		return;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_REF) {
		auto &ref = expr.Cast<BoundReferenceExpression>();
		if (names.find(ref.Index()) == names.end()) {
			names[ref.Index()] = ref.GetAlias().GetIdentifierName().empty() ? ("p" + to_string(ref.Index()))
			                                                                : ref.GetAlias().GetIdentifierName();
		}
		return;
	}
	// For nested lambda functions, only recurse into children (not bind_info)
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr.Cast<BoundFunctionExpression>();
		if (func.Function().GetCallbacks().bind_lambda != nullptr) {
			for (auto &child : func.GetChildren()) {
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
                                              const LogicalType &column_type, string &result) const {
	auto column = make_uniq<BoundReferenceExpression>(Identifier(column_name), column_type, 0);
	auto expression = filter.ToExpression(*column);
	return FilterExpressionToSql(*expression, result);
}

bool LptsExpressionRenderer::FilterExpressionToSql(const Expression &expression, string &result) const {
	std::function<bool(const Expression &, string &)> render = [&](const Expression &expr, string &sql) {
		if (ExpressionFilter::IsOptionalExpression(expr)) {
			return false;
		}
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
			auto &conjunction = expr.Cast<BoundConjunctionExpression>();
			vector<string> children;
			for (auto &child : conjunction.GetChildren()) {
				string child_sql;
				if (render(*child, child_sql)) {
					children.push_back("(" + child_sql + ")");
				} else if (expr.GetExpressionType() == ExpressionType::CONJUNCTION_OR) {
					return false;
				}
			}
			if (children.empty()) {
				return false;
			}
			sql = VecToSeparatedList(children,
			                         expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND ? " AND " : " OR ");
			return true;
		}
		sql = ExpressionToAliasedString(expr);
		return true;
	};
	return render(expression, result);
}

vector<string> LptsExpressionRenderer::ScanFiltersToSql(const LogicalGet &get, const string &qualifier) const {
	vector<string> result;
	auto base_column_sql = [&](const ColumnIndex &column) {
		return qualifier + DialectQuoteIdent(get.GetColumnName(column).GetIdentifierName(), dialect);
	};
	auto column_sql = [&](ProjectionIndex index) {
		const auto &column = get.GetColumnIndex(index);
		const auto primary_index = column.GetPrimaryIndex();
		string base_expression = base_column_sql(column);
		if (!column.HasChildren() || primary_index >= get.returned_types.size()) {
			return base_expression;
		}
		string extracted;
		string leaf_name;
		return RenderStructExtractPath(base_expression, get.returned_types[primary_index], column, extracted, leaf_name)
		           ? extracted
		           : base_expression;
	};
	for (auto &entry : get.table_filters) {
		if (ExpressionFilter::IsOptionalFilter(entry.Filter())) {
			continue;
		}
		const auto &column = get.GetColumnIndex(entry.GetIndex());
		string filter_column = column_sql(entry.GetIndex());
		LogicalType filter_type = get.GetColumnType(column);
		if (entry.Filter().filter_type == TableFilterType::EXPRESSION_FILTER) {
			const auto &expression_filter = ExpressionFilter::GetExpressionFilter(entry.Filter(), "LPTS scan");
			bool expression_extracts_child = false;
			if (column.HasChildren()) {
				ExpressionIterator::VisitExpression<BoundFunctionExpression>(
				    *expression_filter.expr, [&](const BoundFunctionExpression &function) {
					    const auto &name = function.Function().GetName().GetIdentifierName();
					    expression_extracts_child |= name == "struct_extract" || name == "struct_extract_at";
				    });
			}
			if (column.HasChildren() && expression_extracts_child) {
				const auto &base_type = get.returned_types[column.GetPrimaryIndex()];
				filter_column = base_column_sql(column);
				filter_type = base_type;
			}
		}
		string sql;
		if (TableFilterToSql(entry.Filter(), filter_column, filter_type, sql)) {
			result.push_back(std::move(sql));
		}
	}
	for (auto &filter : get.table_filters.GetMultiColumnFilters()) {
		if (ExpressionFilter::IsOptionalFilter(*filter)) {
			continue;
		}
		const auto &multi = ExpressionFilter::GetExpressionFilter(*filter, "LPTS scan");
		auto expression = multi.expr->Copy();
		std::function<void(unique_ptr<Expression> &)> resolve = [&](unique_ptr<Expression> &expr) {
			if (expr->GetExpressionClass() == ExpressionClass::BOUND_REF) {
				auto &ref = expr->Cast<BoundReferenceExpression>();
				if (ref.Index() >= multi.column_indexes.size()) {
					throw InternalException("Invalid multi-column scan filter reference");
				}
				const auto projection_index = multi.column_indexes[ref.Index()];
				string resolved_column = column_sql(projection_index);
				expr =
				    make_uniq<BoundReferenceExpression>(Identifier(resolved_column), ref.GetReturnType(), ref.Index());
			} else {
				ExpressionIterator::EnumerateChildren(*expr, resolve);
			}
		};
		resolve(expression);
		string sql;
		if (FilterExpressionToSql(*expression, sql)) {
			result.push_back(std::move(sql));
		}
	}
	return result;
}

bool LptsExpressionRenderer::RenderStructExtractPath(const string &base_expression, const LogicalType &base_type,
                                                     const ColumnIndex &index, string &result,
                                                     string &leaf_name) const {
	if (!index.HasChildren()) {
		return false;
	}
	string expression = base_expression;
	LogicalType current_type = base_type;
	const ColumnIndex *node = &index;
	while (node->HasChildren()) {
		const ColumnIndex &child = node->GetChildIndex(0);
		string field_name;
		if (child.HasPrimaryIndex()) {
			if (current_type.id() != LogicalTypeId::STRUCT) {
				ThrowLptsNotImplemented("LPTS_STRUCT_EXTRACT_PUSHDOWN", dialect, "expression",
				                        "struct field extraction", "LOGICAL_GET",
				                        "field-extraction pushdown through a non-STRUCT type is not implemented");
			}
			const auto &child_types = StructType::GetChildTypes(current_type);
			const idx_t field_position = child.GetPrimaryIndex();
			if (field_position >= child_types.size()) {
				ThrowLptsNotImplemented("LPTS_STRUCT_EXTRACT_PUSHDOWN", dialect, "expression",
				                        "struct field extraction", "LOGICAL_GET",
				                        "field-extraction pushdown references an out-of-range struct field");
			}
			field_name = StructType::GetChildName(current_type, field_position).GetIdentifierName();
			current_type = child_types[field_position].second;
			expression = "struct_extract(" + expression + ", '" + EscapeSingleQuotes(field_name) + "')";
		} else {
			field_name = child.GetFieldName();
			expression += "['" + EscapeSingleQuotes(field_name) + "']";
			current_type = LogicalType(LogicalTypeId::INVALID);
		}
		leaf_name = field_name;
		node = &child;
	}
	if (index.IsPushdownExtract() && index.HasType() &&
	    ((current_type.id() != LogicalTypeId::INVALID && index.GetScanType() != current_type) ||
	     (current_type.id() == LogicalTypeId::INVALID && index.GetScanType().id() != LogicalTypeId::VARIANT))) {
		expression = "CAST(" + expression + " AS " + index.GetScanType().ToString() + ")";
	}
	result = std::move(expression);
	return true;
}

// DuckDB does not expose quantile arguments through the function children after binding — they live in
// the bind data, both for aggregate calls and for the same aggregate used as a window function. Keep the
// layout-dependent access isolated here so a future DuckDB upgrade has one place to update if
// QuantileBindData (defined in a .cpp) changes.
namespace {
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
} // namespace

static string QuantileArgumentFromBindInfo(const FunctionData &bind_info) {
	const auto &bind_data = reinterpret_cast<const QuantileBindDataLayout &>(bind_info);
	if (bind_data.quantiles.size() == 1) {
		return bind_data.quantiles[0].val.ToSQLString();
	}
	vector<string> values;
	for (const auto &quantile : bind_data.quantiles) {
		values.push_back(quantile.val.ToSQLString());
	}
	return "[" + VecToSeparatedList(values) + "]";
}

static string ApproxQuantileArgumentFromBindInfo(const FunctionData &bind_info) {
	struct ApproxQuantileBindDataLayout {
		void *vtable;
		vector<float> quantiles;
	};
	const auto &bind_data = reinterpret_cast<const ApproxQuantileBindDataLayout &>(bind_info);
	if (bind_data.quantiles.size() == 1) {
		return Value::FLOAT(bind_data.quantiles[0]).ToSQLString();
	}
	vector<string> values;
	for (const auto quantile : bind_data.quantiles) {
		values.push_back(Value::FLOAT(quantile).ToSQLString());
	}
	return "[" + VecToSeparatedList(values) + "]";
}

static string ReservoirQuantileArgumentsFromBindInfo(const FunctionData &bind_info) {
	struct ReservoirQuantileBindDataLayout {
		void *vtable;
		vector<double> quantiles;
		idx_t sample_size;
	};
	const auto &bind_data = reinterpret_cast<const ReservoirQuantileBindDataLayout &>(bind_info);
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

string LptsExpressionRenderer::QuantileArgument(const BoundAggregateExpression &aggregate) {
	return QuantileArgumentFromBindInfo(*aggregate.BindInfo());
}

bool LptsExpressionRenderer::QuantileDesc(const BoundAggregateExpression &aggregate) {
	// The `desc` flag is set both for `WITHIN GROUP (ORDER BY x DESC)` and for negative quantile
	// arguments (DuckDB normalizes `quantile_disc(x, -0.5)` to quantile 0.5 with desc=true).
	if (!aggregate.BindInfo()) {
		return false;
	}
	return reinterpret_cast<const QuantileBindDataLayout *>(aggregate.BindInfo().get())->desc;
}

string LptsExpressionRenderer::ApproxQuantileArgument(const BoundAggregateExpression &aggregate) {
	return ApproxQuantileArgumentFromBindInfo(*aggregate.BindInfo());
}

string LptsExpressionRenderer::ReservoirQuantileArguments(const BoundAggregateExpression &aggregate) {
	return ReservoirQuantileArgumentsFromBindInfo(*aggregate.BindInfo());
}

bool LptsExpressionRenderer::IsQuantileAggregate(const string &agg_name) {
	return agg_name == "quantile_cont" || agg_name == "quantile_disc";
}

string LptsExpressionRenderer::FunctionNameForDialect(const BoundSimpleFunction &function, const string &name,
                                                      SqlDialect dialect) {
	const auto &path = function.GetQualifiedName().Path();
	if (dialect != SqlDialect::DUCKDB || path.size() <= 1) {
		return name;
	}
	return function.GetQualifiedName().WithName(Identifier(name)).ToString();
}

string LptsExpressionRenderer::StripTablePrefix(const string &cte_column_name) {
	const auto underscore_pos = cte_column_name.find('_');
	if (underscore_pos != string::npos && underscore_pos + 1 < cte_column_name.size()) {
		return cte_column_name.substr(underscore_pos + 1);
	}
	return cte_column_name;
}

string LptsExpressionRenderer::StringAggSeparator(const BoundAggregateExpression &aggregate) {
	if (aggregate.Function().GetName().GetIdentifierName() != "string_agg" || !aggregate.BindInfo()) {
		return string();
	}
	// DuckDB stores the separator in string_agg bind data instead of aggregate children.
	// Keep this layout-dependent access isolated; rendering_edges.test covers it.
	struct StringAggBindDataLayout {
		void *vtable;
		string sep;
	};
	return reinterpret_cast<const StringAggBindDataLayout *>(aggregate.BindInfo().get())->sep;
}

string LptsExpressionRenderer::ListAggrStringAggSeparatorArg(const BoundFunctionExpression &func_expr) {
	// list_aggregate/list_aggr wrapping string_agg is how `array_to_string(list, sep)` compiles. The
	// separator is not a child of list_aggr — it lives in the nested string_agg's bind data, reachable via
	// ListAggregatesBindData::aggr_expr (the bound string_agg aggregate). Re-emit it as a trailing
	// `, '<sep>'` argument (list_aggregate forwards extra args to the sub-aggregate). Returns "" when this
	// is not a string_agg-wrapping list_aggr. ListAggregatesBindData is defined in a .cpp, so mirror its
	// layout (like StringAggSeparator); keep this the single place that depends on it.
	if ((func_expr.Function().GetName().GetIdentifierName() != "list_aggr" &&
	     func_expr.Function().GetName().GetIdentifierName() != "list_aggregate") ||
	    !func_expr.BindInfo()) {
		return string();
	}
	// DuckDB 2.0 preserves forwarded aggregate arguments as function children.
	// Only recover bind-data arguments for older/two-child plan shapes.
	if (func_expr.GetChildren().size() > 2) {
		return string();
	}
	struct ListAggregatesBindDataLayout {
		void *vtable;
		LogicalType stype;
		unique_ptr<Expression> aggr_expr;
	};
	const auto &bind_data = *reinterpret_cast<const ListAggregatesBindDataLayout *>(func_expr.BindInfo().get());
	if (!bind_data.aggr_expr || bind_data.aggr_expr->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		return string();
	}
	const auto &aggregate = bind_data.aggr_expr->Cast<BoundAggregateExpression>();
	if (aggregate.Function().GetName().GetIdentifierName() == "string_agg") {
		return ", '" + EscapeSingleQuotes(StringAggSeparator(aggregate)) + "'";
	}
	// Same recovery for quantile-family sub-aggregates (`list_aggr(l, 'quantile', 0.5)`): the quantile
	// argument was consumed into the nested aggregate's bind data.
	if (aggregate.BindInfo()) {
		if (IsQuantileAggregate(aggregate.Function().GetName().GetIdentifierName())) {
			return ", " + QuantileArgumentFromBindInfo(*aggregate.BindInfo());
		}
		if (aggregate.Function().GetName().GetIdentifierName() == "approx_quantile") {
			return ", " + ApproxQuantileArgumentFromBindInfo(*aggregate.BindInfo());
		}
		if (aggregate.Function().GetName().GetIdentifierName() == "reservoir_quantile") {
			return ", " + ReservoirQuantileArgumentsFromBindInfo(*aggregate.BindInfo());
		}
	}
	return string();
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
	// A folded constant order key (e.g. `ORDER BY j` where j aliases the literal 10) would render as a
	// bare integer and be re-read as an ORDINAL position. Parenthesize: `(10)` is a constant expression.
	if (order.expression->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		result << "(" << ExpressionToAliasedString(order.expression) << ")";
	} else {
		result << ExpressionToAliasedString(order.expression);
	}
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

bool LptsExpressionRenderer::IsRemovableConstantOrderKey(const BoundOrderByNode &order) {
	return order.expression->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT;
}

namespace {

/// Return a constant datetime-unit argument as a bare uppercase keyword, or ""
/// when the argument is not a recognised unit literal.
///
/// PostgreSQL EXTRACT field for a DuckDB single-argument date-part function, or
/// nullptr when there is no exact equivalent.
///
/// Only fields whose DuckDB and SQL-standard meanings coincide are listed.
/// `dayofweek` maps to DOW because both count Sunday as 0; `isodow` differs and
/// is therefore mapped separately rather than folded in. Anything absent falls
/// through to the generic call path, so an unmapped function is reported by the
/// target instead of being silently mistranslated.
const char *PostgresExtractField(const string &function_name) {
	static const std::map<string, const char *> kExtractFields = {
	    {"year", "YEAR"},
	    {"month", "MONTH"},
	    {"day", "DAY"},
	    {"hour", "HOUR"},
	    {"minute", "MINUTE"},
	    {"second", "SECOND"},
	    {"quarter", "QUARTER"},
	    {"week", "WEEK"},
	    {"dayofweek", "DOW"},
	    {"dayofyear", "DOY"},
	    {"isodow", "ISODOW"},
	    {"epoch", "EPOCH"},
	    {"millennium", "MILLENNIUM"},
	    {"century", "CENTURY"},
	    {"decade", "DECADE"},
	};
	auto entry = kExtractFields.find(StringUtil::Lower(function_name));
	return entry == kExtractFields.end() ? nullptr : entry->second;
}

/// Spark and Feldera spell the unit as a keyword (`datediff(DAY, a, b)`) where
/// DuckDB uses a string (`datediff('day', a, b)`). Only documented units are unquoted;
/// anything else (a non-constant expression, an alias like 'dow', a unit the target
/// does not accept) returns "" so the caller emits the original form and the
/// engine reports it, rather than this silently inventing a keyword.
string UnquotedDatetimeUnitLiteral(const unique_ptr<Expression> &arg) {
	if (!arg || arg->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return string();
	}
	const auto &constant = arg->Cast<BoundConstantExpression>();
	if (constant.GetValue().IsNull() || constant.GetValue().type().id() != LogicalTypeId::VARCHAR) {
		return string();
	}
	static const std::set<string> kDatetimeUnits = {"YEAR",   "QUARTER",     "MONTH",      "WEEK",
	                                                "DAY",    "DAYOFYEAR",   "HOUR",       "MINUTE",
	                                                "SECOND", "MILLISECOND", "MICROSECOND"};
	string unit = StringUtil::Upper(constant.GetValue().GetValue<string>());
	if (kDatetimeUnits.find(unit) == kDatetimeUnits.end()) {
		return string();
	}
	return unit;
}

} // namespace

string LptsExpressionRenderer::WindowFunctionName(const BoundWindowExpression &window) const {
	if (window.AggregateFunction()) {
		string agg_name = window.AggregateFunction()->GetName().GetIdentifierName();
		if (agg_name == "sum_no_overflow") {
			agg_name = "sum";
		}
		if (agg_name == "count_star") {
			agg_name = "count";
		}
		return FunctionNameForDialect(*window.AggregateFunction(), agg_name, dialect);
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
	// The plan stores a RANGE bound as `<order key> - <offset>` / `+ <offset>` for ascending order and
	// reverses the arithmetic for descending order. Recover the bare offset.
	// DuckDB may wrap the order key in casts inconsistently between the ORDER BY and the frame expression
	// (e.g. `CAST(date AS TIMESTAMP)` for interval arithmetic) — compare the two modulo casts, or falling
	// back to rendering the whole bound expression would re-bind the SUBTRACTION as the offset (a
	// TIMESTAMP), which cannot be added to the order key.
	auto peel_casts = [](const Expression *e) {
		while (e && BoundCastExpression::IsCast(*e)) {
			e = &BoundCastExpression::Child(e->Cast<BoundFunctionExpression>());
		}
		return e;
	};
	if (!window.OrderBy().empty() && expr) {
		const Expression *order_peeled = peel_casts(window.OrderBy()[0].expression.get());
		const Expression *endpoint_peeled = peel_casts(expr.get());
		if (order_peeled && endpoint_peeled) {
			unique_ptr<Expression> order_copy = order_peeled->Copy();
			unique_ptr<Expression> endpoint_copy = endpoint_peeled->Copy();
			if (ExpressionToAliasedString(endpoint_copy) == ExpressionToAliasedString(order_copy)) {
				return "0";
			}
		}
		if (expr->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
			return ExpressionToAliasedString(expr);
		}
		const auto &func_expr = expr->Cast<BoundFunctionExpression>();
		const string &func_name = func_expr.Function().GetName().GetIdentifierName();
		const bool descending = window.OrderBy()[0].type == OrderType::DESCENDING;
		const string expected_function = preceding != descending ? "-" : "+";
		if (func_expr.GetChildren().size() == 2 && func_name == expected_function) {
			const Expression *left_peeled = peel_casts(func_expr.GetChildren()[0].get());
			if (order_peeled && left_peeled) {
				unique_ptr<Expression> order_copy = order_peeled->Copy();
				unique_ptr<Expression> left_copy = left_peeled->Copy();
				if (ExpressionToAliasedString(left_copy) == ExpressionToAliasedString(order_copy)) {
					return ExpressionToAliasedString(func_expr.GetChildren()[1]);
				}
			}
		}
	}
	return ExpressionToAliasedString(expr);
}

string LptsExpressionRenderer::WindowFrameStartToAliasedString(const BoundWindowExpression &window,
                                                               string &units) const {
	switch (window.WindowStart()) {
	case WindowBoundary::CURRENT_ROW_RANGE:
	case WindowBoundary::CURRENT_ROW_ROWS:
	case WindowBoundary::CURRENT_ROW_GROUPS:
		units = WindowFrameUnits(window.WindowStart());
		return "CURRENT ROW";
	case WindowBoundary::UNBOUNDED_PRECEDING:
		if (window.WindowEnd() != WindowBoundary::CURRENT_ROW_RANGE) {
			return "UNBOUNDED PRECEDING";
		}
		return "";
	case WindowBoundary::EXPR_PRECEDING_ROWS:
	case WindowBoundary::EXPR_PRECEDING_GROUPS:
		units = WindowFrameUnits(window.WindowStart());
		return ExpressionToAliasedString(window.StartExpr()) + " PRECEDING";
	case WindowBoundary::EXPR_PRECEDING_RANGE:
		units = WindowFrameUnits(window.WindowStart());
		return WindowRangeFrameOffsetToAliasedString(window, window.StartExpr(), true) + " PRECEDING";
	case WindowBoundary::EXPR_FOLLOWING_ROWS:
	case WindowBoundary::EXPR_FOLLOWING_GROUPS:
		units = WindowFrameUnits(window.WindowStart());
		return ExpressionToAliasedString(window.StartExpr()) + " FOLLOWING";
	case WindowBoundary::EXPR_FOLLOWING_RANGE:
		units = WindowFrameUnits(window.WindowStart());
		return WindowRangeFrameOffsetToAliasedString(window, window.StartExpr(), false) + " FOLLOWING";
	case WindowBoundary::INVALID:
		return "";
	default:
		ThrowLptsNotImplemented("LPTS_UNSUPPORTED_OPERATOR", dialect, "window_frame_start",
		                        std::to_string(static_cast<int>(window.WindowStart())), "BOUND_WINDOW",
		                        "window frame start is not implemented by LPTS");
	}
}

string LptsExpressionRenderer::WindowFrameEndToAliasedString(const BoundWindowExpression &window, string &units) const {
	switch (window.WindowEnd()) {
	case WindowBoundary::CURRENT_ROW_RANGE:
		if (window.WindowStart() != WindowBoundary::UNBOUNDED_PRECEDING &&
		    window.WindowStart() != WindowBoundary::INVALID) {
			units = "RANGE";
			return "CURRENT ROW";
		}
		return "";
	case WindowBoundary::CURRENT_ROW_ROWS:
	case WindowBoundary::CURRENT_ROW_GROUPS:
		units = WindowFrameUnits(window.WindowEnd());
		return "CURRENT ROW";
	case WindowBoundary::UNBOUNDED_PRECEDING:
		return "UNBOUNDED PRECEDING";
	case WindowBoundary::UNBOUNDED_FOLLOWING:
		return "UNBOUNDED FOLLOWING";
	case WindowBoundary::EXPR_PRECEDING_ROWS:
	case WindowBoundary::EXPR_PRECEDING_GROUPS:
		units = WindowFrameUnits(window.WindowEnd());
		return ExpressionToAliasedString(window.EndExpr()) + " PRECEDING";
	case WindowBoundary::EXPR_PRECEDING_RANGE:
		units = WindowFrameUnits(window.WindowEnd());
		return WindowRangeFrameOffsetToAliasedString(window, window.EndExpr(), true) + " PRECEDING";
	case WindowBoundary::EXPR_FOLLOWING_ROWS:
	case WindowBoundary::EXPR_FOLLOWING_GROUPS:
		units = WindowFrameUnits(window.WindowEnd());
		return ExpressionToAliasedString(window.EndExpr()) + " FOLLOWING";
	case WindowBoundary::EXPR_FOLLOWING_RANGE:
		units = WindowFrameUnits(window.WindowEnd());
		return WindowRangeFrameOffsetToAliasedString(window, window.EndExpr(), false) + " FOLLOWING";
	case WindowBoundary::INVALID:
		return "";
	default:
		ThrowLptsNotImplemented("LPTS_UNSUPPORTED_OPERATOR", dialect, "window_frame_end",
		                        std::to_string(static_cast<int>(window.WindowEnd())), "BOUND_WINDOW",
		                        "window frame end is not implemented by LPTS");
	}
}

string LptsExpressionRenderer::WindowExpressionToAliasedString(const BoundWindowExpression &window) const {
	// `WITH ORDINALITY` compiles to a distinctive `row_number() OVER (ROWS BETWEEN UNBOUNDED PRECEDING AND
	// CURRENT ROW)` — no PARTITION BY, no ORDER BY, and a ROWS frame (end = CURRENT_ROW_ROWS). The numbering
	// follows an unspecified scan order, so the rewrite cannot reproduce it. DuckDB's correlated-LIMIT
	// decorrelator uses the same frame but names its private column limit_rownum; that row number implements
	// the original LIMIT and can be emitted. A plain row_number() OVER () has a RANGE frame.
	if (window.GetExpressionType() == ExpressionType::WINDOW_ROW_NUMBER && window.OrderBy().empty() &&
	    window.Partitions().empty() && window.WindowStart() == WindowBoundary::UNBOUNDED_PRECEDING &&
	    window.WindowEnd() == WindowBoundary::CURRENT_ROW_ROWS &&
	    window.GetAlias().GetIdentifierName() != "limit_rownum") {
		ThrowLptsNotImplemented("LPTS_UNSUPPORTED_WINDOW", dialect, "window_function", "row_number", "BOUND_WINDOW",
		                        "WITH ORDINALITY (row_number over an unspecified row order) cannot be reproduced");
	}
	std::ostringstream result;
	result << WindowFunctionName(window) << "(";
	if (window.AggregateFunction() && window.AggregateFunction()->GetName().GetIdentifierName() == "count_star" &&
	    window.GetChildren().empty()) {
		result << "*";
	}
	for (idx_t i = 0; i < window.GetChildren().size(); i++) {
		if (i > 0) {
			result << ", ";
		}
		if (window.Distinct() && i == 0) {
			result << "DISTINCT ";
		}
		result << ExpressionToAliasedString(window.GetChildren()[i]);
	}
	// Older plan shapes consumed quantile arguments into bind data, leaving only the input child. DuckDB 2.0
	// keeps those arguments as children, so recover bind-data arguments only for the one-child shape.
	if (window.AggregateFunction() && window.BindInfo() && !window.GetChildren().empty()) {
		const string &agg_name = window.AggregateFunction()->GetName().GetIdentifierName();
		if (IsQuantileAggregate(agg_name) && window.GetChildren().size() == 1) {
			result << ", " << QuantileArgumentFromBindInfo(*window.BindInfo());
		} else if (agg_name == "approx_quantile" && window.GetChildren().size() == 1) {
			result << ", " << ApproxQuantileArgumentFromBindInfo(*window.BindInfo());
		} else if (agg_name == "reservoir_quantile" && window.GetChildren().size() == 1) {
			result << ", " << ReservoirQuantileArgumentsFromBindInfo(*window.BindInfo());
		}
	}
	if (!window.ArgOrders().empty()) {
		string separator;
		for (const auto &order : window.ArgOrders()) {
			if (IsRemovableConstantOrderKey(order)) {
				continue;
			}
			if (separator.empty()) {
				result << " ORDER BY ";
			} else {
				result << ", ";
			}
			result << OrderByToAliasedString(order);
			separator = ", ";
		}
	}
	if (window.IgnoreNulls()) {
		result << " IGNORE NULLS";
	}
	if (window.Filter()) {
		result << ") FILTER (WHERE " << ExpressionToAliasedString(window.Filter());
	}

	result << ") OVER (";
	string separator;
	if (!window.Partitions().empty()) {
		result << "PARTITION BY ";
		for (idx_t i = 0; i < window.Partitions().size(); i++) {
			if (i > 0) {
				result << ", ";
			}
			result << ExpressionToAliasedString(window.Partitions()[i]);
		}
		separator = " ";
	}
	if (!window.OrderBy().empty()) {
		bool emitted_order = false;
		for (const auto &order : window.OrderBy()) {
			if (IsRemovableConstantOrderKey(order)) {
				continue;
			}
			if (!emitted_order) {
				result << separator << "ORDER BY ";
			} else {
				result << ", ";
			}
			result << OrderByToAliasedString(order);
			emitted_order = true;
		}
		if (emitted_order) {
			separator = " ";
		}
	}

	string units = "ROWS";
	string frame_start = WindowFrameStartToAliasedString(window, units);
	string frame_end = WindowFrameEndToAliasedString(window, units);
	if (window.WindowExclude() != WindowExcludeMode::NO_OTHER) {
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
	if (dialect == SqlDialect::SPARK && window.WindowExclude() != WindowExcludeMode::NO_OTHER) {
		ThrowLptsNotImplemented("LPTS_UNSUPPORTED_OPERATOR", dialect, "window_exclude_clause",
		                        std::to_string(static_cast<int>(window.WindowExclude())), "BOUND_WINDOW",
		                        "Spark SQL does not support window EXCLUDE clauses");
	}
	switch (window.WindowExclude()) {
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
		                        std::to_string(static_cast<int>(window.WindowExclude())), "BOUND_WINDOW",
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
	return ExpressionToAliasedString(*expression);
}

string LptsExpressionRenderer::ExpressionToAliasedString(const Expression &expr) const {
	auto expression = &expr;
	// VARIANT has no faithful, re-parseable SQL literal (Value::ToSQLString emits a `VARIANT(...)` form that
	// does not round-trip), so any expression whose type contains VARIANT (directly or nested in a
	// list/struct/map) is untranslatable — fail rather than emit a value that differs from the original.
	if (TypeContainsVariant(expression->GetReturnType())) {
		ThrowLptsNotImplemented("LPTS_UNSUPPORTED_TYPE", dialect, "type", "VARIANT", "expression",
		                        "VARIANT values have no faithful SQL literal");
	}
	const ExpressionClass e_class = expression->GetExpressionClass();
	std::ostringstream expr_str;
	switch (e_class) {
	case ExpressionClass::BOUND_COLUMN_REF: {
		const BoundColumnRefExpression &bcr = expression->Cast<BoundColumnRefExpression>();
		expr_str << binding_resolver(bcr.Binding(), "expression");
		break;
	}
	case ExpressionClass::BOUND_CONSTANT: {
		const BoundConstantExpression &constant = expression->Cast<BoundConstantExpression>();
		if (TypeContainsEmptyEnum(constant.GetValue().type())) {
			ThrowLptsNotImplemented("LPTS_UNSUPPORTED_TYPE", dialect, "type", constant.GetValue().type().ToString(),
			                        "BOUND_CONSTANT", "an empty ENUM constant has no inline SQL representation");
		}
		// Non-DuckDB dialects render constants through the dialect-aware path (target-specific
		// literal/cast forms, bare NULL for untyped SQLNULL). The faithful, re-parseable rendering
		// below is DuckDB-specific.
		if (!IsDuckDBDialect(dialect)) {
			expr_str << RenderConstantForDialect(constant, dialect);
			break;
		}
		// DuckDB owns the lossless SQL representation of its values. Its native exporter preserves
		// nested type witnesses, aliases, collations, aggregate-state signatures, and typed NULLs.
		expr_str << RenderConstantForDialect(constant, dialect);
		break;
	}
	case ExpressionClass::BOUND_CONJUNCTION: {
		const BoundConjunctionExpression &conj = expression->Cast<BoundConjunctionExpression>();
		// BoundConjunctionExpression can have N children (N ≥ 2).
		// Serialize as: (child[0]) OP (child[1]) OP ... OP (child[N-1]).
		string op = ExpressionTypeToOperator(conj.GetExpressionType());
		expr_str << "(";
		expr_str << ExpressionToAliasedString(conj.GetChildren()[0]);
		expr_str << ")";
		for (size_t ci = 1; ci < conj.GetChildren().size(); ci++) {
			expr_str << " " << op << " (";
			expr_str << ExpressionToAliasedString(conj.GetChildren()[ci]);
			expr_str << ")";
		}
		break;
	}
	case ExpressionClass::BOUND_FUNCTION: {
		if (BoundComparisonExpression::IsComparison(*expression)) {
			const BoundFunctionExpression &cmp = expression->Cast<BoundFunctionExpression>();
			const auto &left = BoundComparisonExpression::Left(cmp);
			const auto &right = BoundComparisonExpression::Right(cmp);
			const bool mismatched_varchar_collations =
			    left.GetReturnType().id() == LogicalTypeId::VARCHAR &&
			    right.GetReturnType().id() == LogicalTypeId::VARCHAR &&
			    !left.GetReturnType().EqualsIncludingCollation(right.GetReturnType());
			if (IsDuckDBDialect(dialect) && mismatched_varchar_collations) {
				auto is_constant_operand = [](const Expression &operand) {
					const Expression *current = &operand;
					while (BoundCastExpression::IsCast(*current)) {
						current = &BoundCastExpression::Child(current->Cast<BoundFunctionExpression>());
					}
					return current->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT;
				};
				const string &left_collation = StringType::GetCollation(left.GetReturnType());
				const string &right_collation = StringType::GetCollation(right.GetReturnType());
				string comparison_collation;
				if (left_collation.empty() != right_collation.empty()) {
					comparison_collation = left_collation.empty() ? right_collation : left_collation;
				} else if (is_constant_operand(left) != is_constant_operand(right)) {
					comparison_collation = is_constant_operand(left) ? left_collation : right_collation;
				}
				if (comparison_collation.empty()) {
					ThrowLptsNotImplemented("LPTS_UNSUPPORTED_COLLATION", dialect, "comparison_collations",
					                        left_collation + "," + right_collation, "BOUND_FUNCTION",
					                        "an optimizer-typed comparison between two non-constant values has no "
					                        "unambiguous SQL collation");
				}
				auto render_collated = [&](const Expression &operand) {
					return "(" + ExpressionToAliasedString(operand) + ") COLLATE " +
					       SQLIdentifier(comparison_collation);
				};
				expr_str << RenderComparisonForDialect(render_collated(left), render_collated(right),
				                                       cmp.GetExpressionType(), dialect);
				break;
			}
			const bool requires_native_export = (left.GetReturnType().id() == LogicalTypeId::VARCHAR &&
			                                     right.GetReturnType().id() == LogicalTypeId::BLOB) ||
			                                    (left.GetReturnType().id() == LogicalTypeId::BLOB &&
			                                     right.GetReturnType().id() == LogicalTypeId::VARCHAR);
			if (IsDuckDBDialect(dialect) && requires_native_export) {
				vector<pair<ColumnBinding, ResolvedSQLColumnReference>> bindings;
				ExpressionIterator::VisitExpression<BoundColumnRefExpression>(
				    *expression, [&](const BoundColumnRefExpression &column) {
					    bindings.push_back(
					        {column.Binding(),
					         {{Identifier(binding_resolver(column.Binding(), "comparison"))}, column.GetReturnType()}});
				    });
				BoundExpressionSQLExportContext export_context;
				export_context.client_context = context;
				export_context.discard_optimizer_metadata = true;
				export_context.resolve_binding =
				    [bindings =
				         std::move(bindings)](const ColumnBinding &binding) -> optional<ResolvedSQLColumnReference> {
					for (const auto &entry : bindings) {
						if (entry.first == binding) {
							return entry.second;
						}
					}
					return {};
				};
				auto exported = BoundExpressionSQLExporter::Export(*expression, export_context);
				if (exported.HasError()) {
					const auto &issues = exported.GetIssues();
					throw InternalException("DuckDB could not export an optimizer-typed comparison: %s",
					                        issues.empty() ? "unknown export failure" : issues[0].message);
				}
				expr_str << exported.GetValue()->ToString();
				break;
			}
			auto render_operand = [&](const Expression &operand, const Expression &peer) {
				if (operand.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT &&
				    operand.GetReturnType().IsNumeric() && operand.GetReturnType() == peer.GetReturnType()) {
					return operand.Cast<BoundConstantExpression>().GetValue().ToSQLString();
				}
				if (BoundCastExpression::IsCast(operand)) {
					const auto &cast = operand.Cast<BoundFunctionExpression>();
					const auto &child = BoundCastExpression::Child(cast);
					if (!BoundCastExpression::IsTryCast(cast) && BoundCastExpression::IsDefaultCast(cast) &&
					    child.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT &&
					    child.GetReturnType().IsNumeric() && operand.GetReturnType().IsNumeric() &&
					    operand.GetReturnType() == peer.GetReturnType()) {
						return ExpressionToAliasedString(child);
					}
				}
				return ExpressionToAliasedString(operand);
			};
			expr_str << RenderComparisonForDialect(render_operand(left, right), render_operand(right, left),
			                                       cmp.GetExpressionType(), dialect);
			break;
		}
		if (expression->GetExpressionType() == ExpressionType::COMPARE_BETWEEN) {
			const auto &between_expr = expression->Cast<BoundFunctionExpression>();
			string input = ExpressionToAliasedString(BoundBetweenExpression::Input(between_expr));
			string lower_op = ExpressionTypeToOperator(BoundBetweenExpression::LowerComparisonType(between_expr));
			string upper_op = ExpressionTypeToOperator(BoundBetweenExpression::UpperComparisonType(between_expr));
			expr_str << "((" << input << ") " << lower_op << " ("
			         << ExpressionToAliasedString(BoundBetweenExpression::LowerBound(between_expr)) << ")) AND (("
			         << input << ") " << upper_op << " ("
			         << ExpressionToAliasedString(BoundBetweenExpression::UpperBound(between_expr)) << "))";
			break;
		}
		if (BoundCastExpression::IsCast(*expression)) {
			const BoundFunctionExpression &cast_expr = expression->Cast<BoundFunctionExpression>();
			const auto &cast_child = BoundCastExpression::Child(cast_expr);
			if (BoundCastExpression::IsTryCast(cast_expr)) {
				ValidateTryCastForDialect(dialect);
			}
			if (cast_expr.GetReturnType().IsAggregateState()) {
				if (!IsDuckDBDialect(dialect) || BoundCastExpression::IsTryCast(cast_expr)) {
					ThrowLptsNotImplemented("LPTS_UNSUPPORTED_TYPE", dialect, "type",
					                        cast_expr.GetReturnType().ToString(), "BOUND_CAST",
					                        "aggregate-state casts require DuckDB's state reconstruction syntax");
				}
				auto parser = Parser::GetBuiltinParser();
				auto child =
				    parser.ParseSingleExpression(ExpressionToAliasedString(BoundCastExpression::Child(cast_expr)));
				auto state = ExportAggregateFunction::StateToSQL(cast_expr.GetReturnType(), std::move(child));
				if (!state) {
					ThrowLptsNotImplemented("LPTS_UNSUPPORTED_TYPE", dialect, "type",
					                        cast_expr.GetReturnType().ToString(), "BOUND_CAST",
					                        "aggregate-state metadata is insufficient to reconstruct SQL");
				}
				expr_str << state->ToString();
				break;
			}
			// A cast to a type containing an UNNAMED struct (e.g. aligning row(...) values, or feeding
			// map_from_entries) cannot be written — the type has no SQL syntax. When the cast only strips
			// field names (same structure and child types), it is a no-op for re-binding: render the child
			// bare and let the binder re-derive whatever internal cast it needs.
			if (TypeContainsUnnamedStruct(cast_expr.GetReturnType()) &&
			    TypesEqualIgnoringStructNames(BoundCastExpression::Child(cast_expr).GetReturnType(),
			                                  cast_expr.GetReturnType())) {
				expr_str << ExpressionToAliasedString(BoundCastExpression::Child(cast_expr));
				break;
			}
			// A scalar cast to the untyped-NULL type is a binder-internal artifact of a fully-folded
			// expression (e.g. a lambda body over an empty list): `CAST(x AS "NULL")` does not parse, and
			// dropping it lets the re-bind derive its own typing. Nested NULL types are different:
			// DuckDB accepts e.g. MAP("NULL", "NULL"), and TRY_CAST to that target has observable semantics.
			if (cast_expr.GetReturnType().id() == LogicalTypeId::SQLNULL) {
				expr_str << ExpressionToAliasedString(BoundCastExpression::Child(cast_expr));
				break;
			}
			expr_str << (BoundCastExpression::IsTryCast(cast_expr) ? "TRY_CAST(" : "CAST(");
			expr_str << ExpressionToAliasedString(cast_child);
			if (dialect == SqlDialect::SPARK && cast_expr.GetReturnType().id() == LogicalTypeId::HUGEINT &&
			    SparkCanRepresentHugeintCastAsDecimal38(cast_child.GetReturnType())) {
				expr_str << " AS DECIMAL(38,0))";
			} else if (dialect == SqlDialect::SPARK && cast_expr.GetReturnType().id() == LogicalTypeId::HUGEINT) {
				ThrowLptsNotImplemented("LPTS_UNSUPPORTED_TYPE", dialect, "type", cast_expr.GetReturnType().ToString(),
				                        "BOUND_CAST",
				                        "source type " + cast_child.GetReturnType().ToString() +
				                            " may use DuckDB HUGEINT's 39-digit range, beyond Spark DECIMAL(38,0)");
			} else {
				expr_str << " AS " + RenderCastTargetType(cast_expr.GetReturnType(), dialect) + ")";
			}
			break;
		}
		const BoundFunctionExpression &func_expr = expression->Cast<BoundFunctionExpression>();
		// Strip internal compress/decompress wrappers injected by COMPRESSED_MATERIALIZATION.
		// For non-BOUND_COLUMN_REF GROUP BY keys (e.g. COALESCE), the optimizer inlines
		// __internal_compress_* directly into the aggregate's group expression instead of
		// creating a separate projection. Render the first argument (the original expression)
		// so the generated SQL stays valid and binder-accepted.
		if (!func_expr.GetChildren().empty() &&
		    (func_expr.Function().GetName().GetIdentifierName().rfind("__internal_compress_", 0) == 0 ||
		     func_expr.Function().GetName().GetIdentifierName().rfind("__internal_decompress_", 0) == 0)) {
			expr_str << ExpressionToAliasedString(func_expr.GetChildren()[0]);
			break;
		}
		// `alias(expr)` returns the *name* of its argument as a string. LPTS rewrites columns to its own
		// names (t0_*) and re-renders expressions, so the name alias() observes — and therefore its value —
		// cannot match the original. It is untranslatable; fail rather than emit a different value.
		if (func_expr.Function().GetName().GetIdentifierName() == "alias") {
			ThrowLptsNotImplemented("LPTS_UNSUPPORTED_FUNCTION", dialect, "function", "alias", "BOUND_FUNCTION",
			                        "alias() reflects column names that LPTS rewrites");
		}
		// list/array slicing with an OMITTED bound (`s[:2]`, `s[2:]`, and the array_pop_back/front
		// rewrites) marks the omitted side with a sentinel constant of the *list's own type* (an empty
		// list value). `array_slice(s, <list>, ...)` does not bind — "bounds must be a BIGINT" — and no
		// function-call form can express the omission; only the bracket syntax can. Render brackets,
		// leaving sentinel sides empty.
		if ((func_expr.Function().GetName().GetIdentifierName() == "array_slice" ||
		     func_expr.Function().GetName().GetIdentifierName() == "list_slice") &&
		    (func_expr.GetChildren().size() == 3 || func_expr.GetChildren().size() == 4)) {
			auto is_omitted_bound = [](const unique_ptr<Expression> &e) {
				return e->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT &&
				       (e->GetReturnType().id() == LogicalTypeId::LIST ||
				        e->GetReturnType().id() == LogicalTypeId::ARRAY);
			};
			// Full-range step -1 slice is how list_reverse compiles; `[::-1]` itself does not parse.
			if (func_expr.GetChildren().size() == 4 && is_omitted_bound(func_expr.GetChildren()[1]) &&
			    is_omitted_bound(func_expr.GetChildren()[2]) &&
			    func_expr.GetChildren()[3]->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
				const auto &step_val = func_expr.GetChildren()[3]->Cast<BoundConstantExpression>().GetValue();
				if (!step_val.IsNull() && step_val.type().IsIntegral() && step_val.GetValue<int64_t>() == -1) {
					expr_str << "list_reverse(" << ExpressionToAliasedString(func_expr.GetChildren()[0]) << ")";
					break;
				}
			}
			if (is_omitted_bound(func_expr.GetChildren()[1]) || is_omitted_bound(func_expr.GetChildren()[2])) {
				const bool has_step = func_expr.GetChildren().size() == 4;
				expr_str << "(" << ExpressionToAliasedString(func_expr.GetChildren()[0]) << ")[";
				if (!is_omitted_bound(func_expr.GetChildren()[1])) {
					expr_str << ExpressionToAliasedString(func_expr.GetChildren()[1]);
				}
				expr_str << ":";
				if (!is_omitted_bound(func_expr.GetChildren()[2])) {
					expr_str << ExpressionToAliasedString(func_expr.GetChildren()[2]);
				} else if (has_step) {
					// With a step, an omitted end is written as the `-` end marker (`a[start:-:step]`);
					// a bare `start::step` does not parse.
					expr_str << "-";
				}
				if (has_step) {
					expr_str << ":" << ExpressionToAliasedString(func_expr.GetChildren()[3]);
				}
				expr_str << "]";
				break;
			}
		}
		// The struct variant `date_part(['year','month'], d)` erases its constant list argument at bind
		// time (Function::EraseArgument) — only the date child remains, so a plain rendering emits
		// `date_part(d)`, which does not bind. The part names live on as the STRUCT return type's field
		// names (in order); rebuild the list literal from them.
		if ((func_expr.Function().GetName().GetIdentifierName() == "date_part" ||
		     func_expr.Function().GetName().GetIdentifierName() == "datepart") &&
		    func_expr.GetReturnType().id() == LogicalTypeId::STRUCT && func_expr.GetChildren().size() == 1) {
			expr_str << func_expr.Function().GetName().GetIdentifierName() << "([";
			for (idx_t i = 0; i < StructType::GetChildCount(func_expr.GetReturnType()); i++) {
				if (i > 0) {
					expr_str << ", ";
				}
				expr_str << "'"
				         << EscapeSingleQuotes(
				                StructType::GetChildName(func_expr.GetReturnType(), i).GetIdentifierName())
				         << "'";
			}
			expr_str << "], " << ExpressionToAliasedString(func_expr.GetChildren()[0]) << ")";
			break;
		}
		ValidateFunctionForDialect(func_expr, dialect);
		if (dialect == SqlDialect::FELDERA && func_expr.GetChildren().size() == 1 &&
		    (func_expr.Function().GetName().GetIdentifierName() == "year" ||
		     func_expr.Function().GetName().GetIdentifierName() == "month")) {
			expr_str << "EXTRACT(" << StringUtil::Upper(func_expr.Function().GetName().GetIdentifierName()) << " FROM "
			         << ExpressionToAliasedString(func_expr.GetChildren()[0]) << ")";
			break;
		}
		// PostgreSQL has no year()/month()/dayofweek()/... date-part shorthands;
		// the part is spelled EXTRACT(<field> FROM value). Emitting the DuckDB
		// name produced "function year(timestamp without time zone) does not
		// exist" at the target, which reads as an engine gap when it is really a
		// translation one. Only the fields whose DuckDB and SQL-standard
		// meanings coincide are mapped; anything else falls through to the
		// generic path and is reported honestly by the target.
		if (dialect == SqlDialect::POSTGRES && func_expr.GetChildren().size() == 1) {
			const char *extract_field = PostgresExtractField(func_expr.Function().GetName().GetIdentifierName());
			if (extract_field != nullptr) {
				expr_str << "EXTRACT(" << extract_field << " FROM "
				         << ExpressionToAliasedString(func_expr.GetChildren()[0]) << ")";
				break;
			}
		}
		// DuckDB's contains(haystack, needle) -> boolean has no PostgreSQL
		// equivalent under that name, but strpos() is exactly it once compared
		// against zero.
		if (dialect == SqlDialect::POSTGRES && func_expr.Function().GetName().GetIdentifierName() == "contains" &&
		    func_expr.GetChildren().size() == 2) {
			expr_str << "(strpos(" << ExpressionToAliasedString(func_expr.GetChildren()[0]) << ", "
			         << ExpressionToAliasedString(func_expr.GetChildren()[1]) << ") > 0)";
			break;
		}
		// Dialect-specific function name remapping (see dialect_function_map.hpp).
		string func_name = RemapFunctionNameForDialect(func_expr.Function().GetName().GetIdentifierName(), dialect);
		// Lambda children remain in the 2.0 expression tree; captures follow the
		// declared arguments. Use the native parameter count, including unused parameters.
		idx_t child_count = func_expr.GetChildren().size();
		const BoundLambdaExpression *bound_lambda = nullptr;
		idx_t native_lambda_pos = child_count;
		for (idx_t i = 0; i < func_expr.GetChildren().size(); i++) {
			if (func_expr.GetChildren()[i]->GetExpressionClass() == ExpressionClass::BOUND_LAMBDA) {
				bound_lambda = &func_expr.GetChildren()[i]->Cast<BoundLambdaExpression>();
				native_lambda_pos = i;
				child_count = MinValue<idx_t>(child_count, func_expr.Function().GetArguments().size());
				break;
			}
		}
		// Operators: use infix notation (e.g. "a + b").
		// Some plan rewrites (like AVG decomposition) create operator functions
		// without setting is_operator. Fall back to name-based detection.
		bool is_infix =
		    func_expr.IsOperator() || (child_count == 2 && (func_name == "/" || func_name == "+" || func_name == "-" ||
		                                                    func_name == "*" || func_name == "%" || func_name == "||"));
		if (is_infix && child_count == 2) {
			expr_str << "(";
			expr_str << ExpressionToAliasedString(func_expr.GetChildren()[0]);
			expr_str << " " << func_name << " ";
			expr_str << ExpressionToAliasedString(func_expr.GetChildren()[1]);
			expr_str << ")";
		} else {
			// struct_pack uses `field := expr` syntax; field names live in the
			// return type, not in the children. Emitting bare children loses the
			// names — the re-binder then uses each child's column alias (e.g.
			// "t0_I_NAME") as the struct field name.
			const bool is_struct_pack = (func_name == "struct_pack" || func_name == "row") &&
			                            func_expr.GetReturnType().id() == LogicalTypeId::STRUCT &&
			                            !StructType::IsUnnamed(func_expr.GetReturnType());
			// Some function names collide with SQL keywords (POSITION, SUBSTRING,
			// OVERLAY, TRIM). DuckDB's parser rejects them as plain identifiers and
			// expects the keyword-separator syntax (`POSITION(x IN y)`), but it
			// accepts them as quoted identifiers (`"position"(x, y)`) — let the
			// function resolver see the function call directly and bypass the
			// keyword-syntax path.
			string emit_name = func_name;
			if (IsDuckDBDialect(dialect) && (func_name == "position" || func_name == "substring" ||
			                                 func_name == "overlay" || func_name == "trim")) {
				emit_name = "\"" + func_name + "\"";
			}
			if (IsDuckDBDialect(dialect) && func_expr.Function().GetQualifiedName().Path().size() > 1) {
				emit_name = FunctionNameForDialect(func_expr.Function(), func_name, dialect);
			}
			if (dialect == SqlDialect::SPARK &&
			    (func_expr.Function().GetName().GetIdentifierName() == "struct_pack" ||
			     func_expr.Function().GetName().GetIdentifierName() == "row") &&
			    func_expr.GetReturnType().id() == LogicalTypeId::STRUCT &&
			    !StructType::IsUnnamed(func_expr.GetReturnType())) {
				expr_str << "named_struct(";
				for (idx_t i = 0; i < child_count; i++) {
					if (i > 0) {
						expr_str << ", ";
					}
					expr_str << "'"
					         << EscapeSingleQuotes(
					                StructType::GetChildName(func_expr.GetReturnType(), i).GetIdentifierName())
					         << "', " << ExpressionToAliasedString(func_expr.GetChildren()[i]);
				}
				expr_str << ")";
				break;
			}
			// DuckDB's date_diff(unit, start, end) differs from Spark 3.5's two-argument
			// datediff(end, start). The DAY form is exactly expressible by reversing the value arguments;
			// refuse other units until they have a verified Spark equivalent.
			if (dialect == SqlDialect::SPARK && (func_name == "datediff" || func_name == "date_diff") &&
			    child_count == 3) {
				string unit = UnquotedDatetimeUnitLiteral(func_expr.GetChildren()[0]);
				if (unit != "DAY") {
					ThrowLptsNotImplemented("LPTS_UNSUPPORTED_FUNCTION", dialect, "function", func_name,
					                        "BOUND_FUNCTION",
					                        "Spark 3.5 only has a verified date_diff mapping for DAY");
				}
				expr_str << "datediff(" << ExpressionToAliasedString(func_expr.GetChildren()[2]) << ", "
				         << ExpressionToAliasedString(func_expr.GetChildren()[1]) << ")";
				break;
			}
			// Feldera takes the three-argument datediff unit as a bare keyword.
			if (dialect == SqlDialect::FELDERA && (func_name == "datediff" || func_name == "date_diff") &&
			    child_count >= 1) {
				string unit = UnquotedDatetimeUnitLiteral(func_expr.GetChildren()[0]);
				if (!unit.empty()) {
					expr_str << emit_name << "(" << unit;
					for (idx_t i = 1; i < child_count; i++) {
						expr_str << ", " << ExpressionToAliasedString(func_expr.GetChildren()[i]);
					}
					expr_str << ")";
					break;
				}
			}
			expr_str << emit_name << "(";
			if (UsesFormatFirstDateFunctionArgumentOrder(func_expr.Function().GetName().GetIdentifierName(), dialect) &&
			    child_count >= 2) {
				expr_str << ExpressionToAliasedString(func_expr.GetChildren()[1]);
				expr_str << ", ";
				expr_str << ExpressionToAliasedString(func_expr.GetChildren()[0]);
				for (idx_t i = 2; i < child_count; i++) {
					expr_str << ", ";
					expr_str << ExpressionToAliasedString(func_expr.GetChildren()[i]);
				}
			} else {
				// Functions with NAMED arguments (`struct_insert(s, a := x)`, `write_log(msg, level := 'x')`):
				// the parser records each `name := expr` as the bound child's alias. Re-emit the names — these
				// functions reject (or misread) plain positional arguments.
				// Lambda function: serialize the lambda expression from bind_info FIRST (into its own
				// buffer), so it can be inserted at its declared argument position. list_transform's lambda
				// is the last argument, but list_reduce(list, lambda, initial) has a trailing initial value:
				// appending the lambda after the children would swap the arguments.
				string lambda_text;
				idx_t lambda_arg_pos = child_count; // default: append after all children
				if (bound_lambda) {
					if (bound_lambda->LambdaExpr()) {
						lambda_arg_pos = native_lambda_pos;
						std::ostringstream lam;
						std::map<idx_t, string> param_map;
						CollectLambdaParamNames(*bound_lambda->LambdaExpr(), param_map);
						idx_t total_refs = param_map.empty() ? 0 : param_map.rbegin()->first + 1;
						// A lambda that closes over outer columns binds them as trailing "parameters": the real
						// lambda parameters occupy body indices [0, real_count) (reverse declaration order) and the
						// captures occupy [real_count, total_refs), mapping to the function's non-list children
						// (child_count..). Only the real parameters go in the lambda's parameter list; captures are
						// rendered as the outer expression (so LPTS's column renaming applies), else e.g.
						// `x -> x + n` would render `lambda n, x: x + n` and shadow the outer column.
						idx_t capture_count = func_expr.GetChildren().size() > child_count
						                          ? func_expr.GetChildren().size() - child_count
						                          : 0;
						const idx_t real_count = bound_lambda->ParameterCount();
						vector<string> param_names(real_count);
						for (idx_t idx = 0; idx < real_count; idx++) {
							auto it = param_map.find(idx);
							string nm =
							    (it != param_map.end() && !it->second.empty()) ? it->second : ("p" + to_string(idx));
							// Quote when not a plain identifier (e.g. a `"x.y"` parameter) — used verbatim in
							// both the parameter list and the body refs, so they stay consistent.
							param_names[real_count - 1 - idx] =
							    (QuoteIdentifier(nm) == nm) ? nm : DialectQuoteIdent(nm, dialect);
						}
						std::map<idx_t, string> captures;
						for (idx_t idx = real_count; idx < total_refs; idx++) {
							const idx_t cap_child = child_count + (idx - real_count);
							if (cap_child < func_expr.GetChildren().size()) {
								captures[idx] = ExpressionToAliasedString(func_expr.GetChildren()[cap_child]);
							}
						}
						// A lambda parameter must not SHADOW a captured outer column: the capture renders as
						// a generated column name (t0_x) that the final de-prefixing pass may collapse to its
						// bare form (x) — identical to a parameter named x. Rename such parameters (body refs
						// follow via the canonical-name map).
						for (auto &nm : param_names) {
							bool renamed = true;
							while (renamed) {
								renamed = false;
								for (const auto &cap : captures) {
									if (StringUtil::Lower(StripTablePrefix(cap.second)) == StringUtil::Lower(nm) ||
									    StringUtil::Lower(cap.second) == StringUtil::Lower(nm)) {
										nm += "_1";
										renamed = true;
									}
								}
							}
						}
						if (UsesArrowLambdaSyntax(dialect)) {
							if (real_count == 1) {
								lam << param_names[0];
							} else {
								lam << "(";
								for (idx_t i = 0; i < real_count; i++) {
									if (i > 0) {
										lam << ", ";
									}
									lam << param_names[i];
								}
								lam << ")";
							}
							lam << " -> ";
						} else {
							lam << "lambda ";
							for (idx_t i = 0; i < real_count; i++) {
								if (i > 0) {
									lam << ", ";
								}
								lam << param_names[i];
							}
							lam << ": ";
						}
						// Render the body with capture indices substituted by their outer expressions and
						// parameter indices rendered under their canonical names (a body ref can carry a
						// different stored name for the same index, e.g. a comprehension's "result" alias).
						std::map<idx_t, string> body_param_names;
						for (idx_t idx = 0; idx < real_count; idx++) {
							body_param_names[idx] = param_names[real_count - 1 - idx];
						}
						auto saved_captures = lambda_captures;
						auto saved_params = lambda_param_names;
						lambda_captures = std::move(captures);
						lambda_param_names = std::move(body_param_names);
						lam << ExpressionToAliasedString(bound_lambda->LambdaExpr());
						lambda_captures = std::move(saved_captures);
						lambda_param_names = std::move(saved_params);
						lambda_text = lam.str();
					}
				}
				const bool uses_named_arguments =
				    func_name == "struct_insert" || func_name == "struct_update" || func_name == "write_log";
				const auto &native_named_arguments = func_expr.Function().GetNamedArguments();
				const idx_t positional_argument_count = func_expr.Function().GetPositionalArgumentCount();
				if (!native_named_arguments.empty() &&
				    positional_argument_count + native_named_arguments.size() != child_count) {
					ThrowLptsNotImplemented("LPTS_UNSUPPORTED_FUNCTION", dialect, "function", func_name,
					                        "BOUND_FUNCTION", "the bound function retained incomplete named arguments");
				}
				idx_t emitted = 0;
				for (idx_t i = 0; i < child_count; i++) {
					if (!lambda_text.empty() && i == lambda_arg_pos) {
						if (emitted++ > 0) {
							expr_str << ", ";
						}
						expr_str << lambda_text;
						continue;
					}
					if (emitted++ > 0) {
						expr_str << ", ";
					}
					if (is_struct_pack && i < StructType::GetChildCount(func_expr.GetReturnType())) {
						expr_str << "\"" << StructType::GetChildName(func_expr.GetReturnType(), i) << "\" := ";
					} else if (!native_named_arguments.empty() && i >= positional_argument_count) {
						expr_str << SQLIdentifier(native_named_arguments[i - positional_argument_count]) << " := ";
					} else if (uses_named_arguments && i > 0 && func_expr.GetChildren()[i]->HasAlias() &&
					           func_expr.GetChildren()[i]->GetAlias().GetIdentifierName().find('(') == string::npos) {
						expr_str << "\"" << func_expr.GetChildren()[i]->GetAlias().GetIdentifierName() << "\" := ";
					}
					string converted_date_format;
					if (i == 1 && IsDateFormatFunction(func_expr.Function().GetName().GetIdentifierName()) &&
					    TryRenderConvertedDateFormat(func_expr.GetChildren()[i], dialect, converted_date_format)) {
						expr_str << converted_date_format;
					} else if (func_name == "equi_width_bins" &&
					           func_expr.GetChildren()[i]->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT &&
					           func_expr.GetChildren()[i]->GetReturnType().id() == LogicalTypeId::VARCHAR) {
						// equi_width_bins is overloaded on VARCHAR vs numeric/timestamp; a bare string
						// literal re-binds as STRING_LITERAL (castable to all) — "could not choose a best
						// candidate". Pin the VARCHAR overload with an explicit cast.
						expr_str << "CAST(" << ExpressionToAliasedString(func_expr.GetChildren()[i]) << " AS VARCHAR)";
					} else {
						expr_str << ExpressionToAliasedString(func_expr.GetChildren()[i]);
					}
				}
				if (!lambda_text.empty() && lambda_arg_pos >= child_count) {
					if (emitted++ > 0) {
						expr_str << ", ";
					}
					expr_str << lambda_text;
				}
			}
			// array_to_string compiles to list_aggr(list, 'string_agg'); re-emit the string_agg separator
			// (recovered from the nested aggregate's bind data) as a trailing argument, else it's lost.
			expr_str << ListAggrStringAggSeparatorArg(func_expr);
			expr_str << ")";
		}
		break;
	}
	case ExpressionClass::BOUND_REF: {
		// Inside a lambda body, a captured outer column is a trailing BoundReferenceExpression — render it as
		// the captured outer expression (see the lambda rendering), not as a lambda parameter name.
		const auto &ref = expression->Cast<BoundReferenceExpression>();
		auto cap = lambda_captures.find(ref.Index());
		if (cap != lambda_captures.end()) {
			expr_str << cap->second;
		} else {
			auto par = lambda_param_names.find(ref.Index());
			if (par != lambda_param_names.end()) {
				expr_str << par->second;
			} else {
				expr_str << expression->ToString();
			}
		}
		break;
	}
	case ExpressionClass::BOUND_LAMBDA_REF: {
		expr_str << expression->ToString();
		break;
	}
	case ExpressionClass::BOUND_CASE: {
		const BoundCaseExpression &case_expr = expression->Cast<BoundCaseExpression>();
		expr_str << "CASE";
		for (auto &check : case_expr.CaseChecks()) {
			expr_str << " WHEN " << ExpressionToAliasedString(check.when_expr);
			expr_str << " THEN " << ExpressionToAliasedString(check.then_expr);
		}
		if (case_expr.ElseExpression()) {
			expr_str << " ELSE " << ExpressionToAliasedString(case_expr.ElseExpression());
		}
		expr_str << " END";
		break;
	}
	case ExpressionClass::BOUND_OPERATOR: {
		const BoundOperatorExpression &op_expr = expression->Cast<BoundOperatorExpression>();
		switch (op_expr.GetExpressionType()) {
		case ExpressionType::OPERATOR_IS_NULL:
			expr_str << "(" << ExpressionToAliasedString(op_expr.GetChildren()[0]) << ") IS NULL";
			break;
		case ExpressionType::OPERATOR_IS_NOT_NULL:
			expr_str << "(" << ExpressionToAliasedString(op_expr.GetChildren()[0]) << ") IS NOT NULL";
			break;
		case ExpressionType::OPERATOR_NOT:
			expr_str << "NOT (" << ExpressionToAliasedString(op_expr.GetChildren()[0]) << ")";
			break;
		case ExpressionType::COMPARE_IN:
		case ExpressionType::COMPARE_NOT_IN: {
			const bool is_not_in = op_expr.GetExpressionType() == ExpressionType::COMPARE_NOT_IN;
			expr_str << "(" << ExpressionToAliasedString(op_expr.GetChildren()[0]) << ")";
			expr_str << (is_not_in ? " NOT IN (" : " IN (");
			for (idx_t i = 1; i < op_expr.GetChildren().size(); i++) {
				if (i > 1) {
					expr_str << ", ";
				}
				expr_str << ExpressionToAliasedString(op_expr.GetChildren()[i]);
			}
			expr_str << ")";
			break;
		}
		case ExpressionType::OPERATOR_COALESCE: {
			expr_str << "COALESCE(";
			for (idx_t i = 0; i < op_expr.GetChildren().size(); i++) {
				if (i > 0) {
					expr_str << ", ";
				}
				expr_str << ExpressionToAliasedString(op_expr.GetChildren()[i]);
			}
			expr_str << ")";
			break;
		}
		case ExpressionType::OPERATOR_NULLIF:
			expr_str << "NULLIF(" << ExpressionToAliasedString(op_expr.GetChildren()[0]) << ", "
			         << ExpressionToAliasedString(op_expr.GetChildren()[1]) << ")";
			break;
		case ExpressionType::OPERATOR_TRY:
			if (!IsDuckDBDialect(dialect)) {
				ThrowLptsNotImplemented("LPTS_DIALECT_SEMANTIC_RISK", dialect, "operator", "TRY", "BOUND_OPERATOR",
				                        "TRY expression has dialect-specific semantics");
			}
			expr_str << "TRY(" << ExpressionToAliasedString(op_expr.GetChildren()[0]) << ")";
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
		expr_str << (dialect == SqlDialect::SPARK ? "explode(" : "UNNEST(")
		         << ExpressionToAliasedString(unnest_expr.Child()) << ")";
		break;
	}
	case ExpressionClass::BOUND_WINDOW: {
		const BoundWindowExpression &window_expr = expression->Cast<BoundWindowExpression>();
		expr_str << WindowExpressionToAliasedString(window_expr);
		break;
	}
	default:
		ThrowLptsNotImplemented("LPTS_UNSUPPORTED_OPERATOR", dialect, "expression_class",
		                        ExpressionTypeToString(expression->GetExpressionType()), "ExpressionToAliasedString",
		                        "expression class is not implemented by LPTS");
	}
	// A bound column's collation is already supplied by its source expression/table. Re-applying the
	// planner's return-type collation here is not only redundant: DuckDB can unify nested expression types
	// across sibling aggregates/set operations, so that annotation may belong to a sibling rather than this
	// column. Preserve collations only on expressions that actually construct a new VARCHAR value.
	if (IsDuckDBDialect(dialect) && e_class != ExpressionClass::BOUND_CONSTANT &&
	    e_class != ExpressionClass::BOUND_COLUMN_REF && expression->GetReturnType().id() == LogicalTypeId::VARCHAR) {
		const auto &collation = StringType::GetCollation(expression->GetReturnType());
		if (!collation.empty()) {
			string rendered = expr_str.str();
			if (e_class == ExpressionClass::BOUND_FUNCTION) {
				const auto &function = expression->Cast<BoundFunctionExpression>().Function();
				const auto &definition = function.GetDefinition();
				if (definition && definition->GetSignature().GetReturnType().id() != LogicalTypeId::VARCHAR) {
					rendered = "CAST(" + rendered + " AS VARCHAR)";
				}
			}
			return "(" + rendered + ") COLLATE " + SQLIdentifier(collation);
		}
	}
	return expr_str.str();
}

} // namespace duckdb
