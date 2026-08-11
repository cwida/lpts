#include "lpts_expression_renderer.hpp"
#include "lpts_helpers.hpp"
#include "lpts_date_format.hpp"
#include "dialect_function_map.hpp"

#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"

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
#include "duckdb/function/lambda_functions.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/filter/optional_filter.hpp"

#include <limits>

namespace duckdb {

LptsExpressionRenderer::LptsExpressionRenderer(SqlDialect _dialect, BindingResolver _binding_resolver)
    : dialect(_dialect), binding_resolver(std::move(_binding_resolver)) {
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
		return TypeContainsVariant(ListType::GetChildType(type));
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

// An UNNAMED struct type (a ROW value, e.g. produced by `(0, 0)` or list_zip's entries) cannot be written
// in SQL — `STRUCT(INTEGER, INTEGER)` has no field names and does not parse — so neither a bare literal
// nor a CAST('<text>' AS <type>) can express a constant containing one. Such values are rebuilt
// structurally instead (see RenderConstantContainingUnnamedStruct).
static bool TypeContainsUnnamedStruct(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::LIST:
	case LogicalTypeId::ARRAY:
		return TypeContainsUnnamedStruct(ListType::GetChildType(type));
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
		return TypesEqualIgnoringStructNames(ListType::GetChildType(a), ListType::GetChildType(b));
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
		return TypeContainsNullType(ListType::GetChildType(type));
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

// Constants of these types cannot be expressed as a literal OR as a CAST from their text form: unnamed
// structs have no writable type, and a UNION's text form does not cast back (VARCHAR is not implicitly
// castable to the members). They are rebuilt structurally (RenderConstantContainingUnnamedStruct).
static bool TypeNeedsStructuralRender(const LogicalType &type) {
	if (TypeContainsUnnamedStruct(type)) {
		return true;
	}
	switch (type.id()) {
	case LogicalTypeId::UNION:
	// An untyped-NULL element type renders as `"NULL"` inside the CAST target (e.g. `CAST('[]' AS
	// "NULL"[])` for an empty list literal), which does not parse; the structural form (`[]`) does.
	case LogicalTypeId::SQLNULL:
		return true;
	case LogicalTypeId::LIST:
	case LogicalTypeId::ARRAY:
		return TypeNeedsStructuralRender(ListType::GetChildType(type));
	case LogicalTypeId::STRUCT: {
		for (const auto &child : StructType::GetChildTypes(type)) {
			if (TypeNeedsStructuralRender(child.second)) {
				return true;
			}
		}
		return false;
	}
	case LogicalTypeId::MAP:
		return TypeNeedsStructuralRender(MapType::KeyType(type)) || TypeNeedsStructuralRender(MapType::ValueType(type));
	default:
		return false;
	}
}

static bool ConstantNeedsExplicitCast(const LogicalType &type) {
	// Extension / user types (GEOMETRY, INET, ...): their text form has no literal syntax of its own
	// (bare WKT does not even parse); CAST('<text>' AS <alias>) re-enters through the type's VARCHAR cast.
	if (type.HasAlias()) {
		return true;
	}
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE: // a bare `2.0` re-parses as DECIMAL, not DOUBLE
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::BIT:
	case LogicalTypeId::BIGNUM:
	// ENUM: a folded enum value (e.g. `'happy'::mood`, or the ENUM('num','str') that union_tag(...) returns)
	// renders bare as a plain VARCHAR string, losing the ENUM type. CAST('happy' AS ENUM('sad','ok','happy'))
	// (type.ToString() inlines the full member list) restores the type so the type-sensitive bag-hash matches.
	case LogicalTypeId::ENUM:
	// GEOMETRY: ToSQLString has no case for it, so the value prints as bare WKT (not even a string
	// literal); CAST('<wkt>' AS GEOMETRY) re-enters through the text cast.
	case LogicalTypeId::GEOMETRY:
	// Nested types whose element/field types are not preserved by the bare `[...]`/`{...}` literal
	// (e.g. a DOUBLE[] prints as `[2.0, ...]` and re-parses as DECIMAL[]). CAST('<text>' AS <type>) casts
	// the string form to the exact nested type. MAP values have NO bare literal at all — ToSQLString()
	// yields the display form `{k=v}`, which only parses as a VARCHAR→MAP cast.
	case LogicalTypeId::LIST:
	case LogicalTypeId::ARRAY:
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::MAP:
		return true;
	default:
		return false;
	}
}

// A bare INTEGER literal round-trips except its minimum value: `-2147483648` re-parses as BIGINT, because
// `2147483648` overflows INT32 and is a BIGINT, then negated. (TINYINT/SMALLINT/BIGINT/HUGEINT are always
// cast by ConstantNeedsExplicitCast, so INTEGER is the only signed width with this gap.) Value-conditional,
// so ordinary INTEGER constants stay bare and readable.
static bool IntegerConstantNeedsExplicitCast(const Value &value) {
	return !value.IsNull() && value.type().id() == LogicalTypeId::INTEGER &&
	       value.GetValue<int32_t>() == std::numeric_limits<int32_t>::min();
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
	// Types with no SQL syntax cannot be a cast target in ANY dialect: an unnamed struct
	// (`STRUCT(INTEGER, INTEGER)`, from row(...) values — only reachable here when the cast is NOT a
	// names-only no-op, see BOUND_CAST) and internal aggregate-state types (`AGGREGATE_STATE<sum(...)>`).
	if (TypeContainsUnnamedStruct(type) || type.id() == LogicalTypeId::AGGREGATE_STATE) {
		ThrowLptsNotImplemented("LPTS_UNSUPPORTED_TYPE", dialect, "type", type.ToString(), "BOUND_CAST",
		                        "the cast target type cannot be written in SQL");
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
		// An untyped NULL literal (SQLNULL) has no target-dialect cast type; emit a
		// bare NULL, which is valid in every non-DuckDB dialect. Casting it to its own
		// "NULL" type would hit the RenderCastTargetType whitelist and fail (e.g. the
		// to_date/to_timestamp shim's CASE ... THEN NULL branches). Typed NULLs keep
		// their explicit CAST so downstream type inference is preserved.
		if (value.type().id() == LogicalTypeId::SQLNULL) {
			return "NULL";
		}
		return "CAST(NULL AS " + RenderCastTargetType(value.type(), dialect) + ")";
	}
	switch (value.type().id()) {
	case LogicalTypeId::DATE:
		return "DATE '" + EscapeSingleQuotes(value.ToString()) + "'";
	case LogicalTypeId::TIMESTAMP:
		return "TIMESTAMP '" + EscapeSingleQuotes(value.ToString()) + "'";
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
				// Parenthesize each child: a nested OR child must not lose grouping when joined with AND
				// (AND binds tighter than OR, so `a AND b OR c` would mean `(a AND b) OR c`).
				children.push_back("(" + std::move(child_sql) + ")");
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
			children.push_back("(" + std::move(child_sql) + ")");
		}
		if (children.empty()) {
			return false;
		}
		result = VecToSeparatedList(children, " OR ");
		return true;
	}
	case TableFilterType::CONSTANT_COMPARISON: {
		// Only reroute through the expression renderer when the compared constant needs a type-preserving
		// cast (BIT/ENUM/HUGEINT/INT_MIN/...); e.g. a BIT filter `b = '111'` must emit `CAST('111' AS BIT)`,
		// not the bare `111` that DuckDB's TableFilter::ToString produces (which re-parses as the integer
		// 111). For ordinary constants, keep the compact `col op const` rendering (readability + exact-SQL
		// tests).
		auto &constant_filter = filter.Cast<ConstantFilter>();
		// A VARCHAR constant containing a NUL byte also reroutes: the expression renderer rebuilds it as
		// `'part' || chr(0) || 'part'` (a NUL cannot appear in a SQL literal).
		const bool varchar_with_nul = constant_filter.constant.type().id() == LogicalTypeId::VARCHAR &&
		                              !constant_filter.constant.IsNull() &&
		                              StringValue::Get(constant_filter.constant).find('\0') != string::npos;
		if (ConstantNeedsExplicitCast(constant_filter.constant.type()) ||
		    IntegerConstantNeedsExplicitCast(constant_filter.constant) || varchar_with_nul) {
			auto column_expr = make_uniq<BoundReferenceExpression>(column_name, LogicalType::INVALID, 0);
			auto filter_expr = filter.ToExpression(*column_expr);
			result = ExpressionToAliasedString(filter_expr);
			return true;
		}
		result = filter.ToString(column_name);
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
	return QuantileArgumentFromBindInfo(*aggregate.bind_info);
}

bool LptsExpressionRenderer::QuantileDesc(const BoundAggregateExpression &aggregate) {
	// The `desc` flag is set both for `WITHIN GROUP (ORDER BY x DESC)` and for negative quantile
	// arguments (DuckDB normalizes `quantile_disc(x, -0.5)` to quantile 0.5 with desc=true).
	if (!aggregate.bind_info) {
		return false;
	}
	return reinterpret_cast<const QuantileBindDataLayout *>(aggregate.bind_info.get())->desc;
}

string LptsExpressionRenderer::ApproxQuantileArgument(const BoundAggregateExpression &aggregate) {
	return ApproxQuantileArgumentFromBindInfo(*aggregate.bind_info);
}

string LptsExpressionRenderer::ReservoirQuantileArguments(const BoundAggregateExpression &aggregate) {
	return ReservoirQuantileArgumentsFromBindInfo(*aggregate.bind_info);
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

string LptsExpressionRenderer::ListAggrStringAggSeparatorArg(const BoundFunctionExpression &func_expr) {
	// list_aggregate/list_aggr wrapping string_agg is how `array_to_string(list, sep)` compiles. The
	// separator is not a child of list_aggr — it lives in the nested string_agg's bind data, reachable via
	// ListAggregatesBindData::aggr_expr (the bound string_agg aggregate). Re-emit it as a trailing
	// `, '<sep>'` argument (list_aggregate forwards extra args to the sub-aggregate). Returns "" when this
	// is not a string_agg-wrapping list_aggr. ListAggregatesBindData is defined in a .cpp, so mirror its
	// layout (like StringAggSeparator); keep this the single place that depends on it.
	if ((func_expr.function.name != "list_aggr" && func_expr.function.name != "list_aggregate") ||
	    !func_expr.bind_info) {
		return string();
	}
	struct ListAggregatesBindDataLayout {
		void *vtable;
		LogicalType stype;
		unique_ptr<Expression> aggr_expr;
	};
	const auto &bind_data = *reinterpret_cast<const ListAggregatesBindDataLayout *>(func_expr.bind_info.get());
	if (!bind_data.aggr_expr || bind_data.aggr_expr->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		return string();
	}
	const auto &aggregate = bind_data.aggr_expr->Cast<BoundAggregateExpression>();
	if (aggregate.function.name == "string_agg") {
		return ", '" + EscapeSingleQuotes(StringAggSeparator(aggregate)) + "'";
	}
	// Same recovery for quantile-family sub-aggregates (`list_aggr(l, 'quantile', 0.5)`): the quantile
	// argument was consumed into the nested aggregate's bind data.
	if (aggregate.bind_info) {
		if (IsQuantileAggregate(aggregate.function.name)) {
			return ", " + QuantileArgumentFromBindInfo(*aggregate.bind_info);
		}
		if (aggregate.function.name == "approx_quantile") {
			return ", " + ApproxQuantileArgumentFromBindInfo(*aggregate.bind_info);
		}
		if (aggregate.function.name == "reservoir_quantile") {
			return ", " + ReservoirQuantileArgumentsFromBindInfo(*aggregate.bind_info);
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

string LptsExpressionRenderer::RenderConstantContainingUnnamedStruct(const Value &value) const {
	const LogicalType &type = value.type();
	if (value.IsNull()) {
		// A typed NULL of an unwritable type cannot be expressed; a bare NULL keeps the value and lets the
		// surrounding expression's other operands pin the type.
		return "NULL";
	}
	switch (type.id()) {
	case LogicalTypeId::STRUCT: {
		const auto &children = StructValue::GetChildren(value);
		std::ostringstream out;
		if (StructType::IsUnnamed(type)) {
			out << "row(";
			for (idx_t i = 0; i < children.size(); i++) {
				out << (i ? ", " : "") << RenderConstantContainingUnnamedStruct(children[i]);
			}
			out << ")";
		} else {
			out << "struct_pack(";
			for (idx_t i = 0; i < children.size(); i++) {
				out << (i ? ", " : "") << "\"" << StructType::GetChildName(type, i)
				    << "\" := " << RenderConstantContainingUnnamedStruct(children[i]);
			}
			out << ")";
		}
		return out.str();
	}
	case LogicalTypeId::LIST:
	case LogicalTypeId::ARRAY: {
		const auto &children = ListValue::GetChildren(value);
		std::ostringstream out;
		out << "[";
		for (idx_t i = 0; i < children.size(); i++) {
			out << (i ? ", " : "") << RenderConstantContainingUnnamedStruct(children[i]);
		}
		out << "]";
		return out.str();
	}
	case LogicalTypeId::MAP: {
		// A MAP value is physically a LIST of (key, value) entry structs; rebuild map(keys, values).
		const auto &entries = ListValue::GetChildren(value);
		std::ostringstream keys, vals;
		for (idx_t i = 0; i < entries.size(); i++) {
			const auto &kv = StructValue::GetChildren(entries[i]);
			keys << (i ? ", " : "") << RenderConstantContainingUnnamedStruct(kv[0]);
			vals << (i ? ", " : "") << RenderConstantContainingUnnamedStruct(kv[1]);
		}
		return "map([" + keys.str() + "], [" + vals.str() + "])";
	}
	case LogicalTypeId::UNION: {
		// Rebuild via the union_value constructor, cast to the full union type so the other members —
		// and hence the value's type — are preserved.
		const Value &inner = UnionValue::GetValue(value);
		const union_tag_t tag = UnionValue::GetTag(value);
		const string &member = UnionType::GetMemberName(type, tag);
		return "CAST(union_value(\"" + member + "\" := " + RenderConstantContainingUnnamedStruct(inner) + ") AS " +
		       type.ToString() + ")";
	}
	default: {
		// Leaf: go through the normal constant path so lossy types keep their type-fidelity CASTs.
		unique_ptr<Expression> leaf = make_uniq<BoundConstantExpression>(value);
		return ExpressionToAliasedString(leaf);
	}
	}
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

namespace {

/// Return a constant datetime-unit argument as a bare uppercase keyword, or ""
/// when the argument is not a recognised unit literal.
///
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
	if (constant.value.IsNull() || constant.value.type().id() != LogicalTypeId::VARCHAR) {
		return string();
	}
	static const std::set<string> kDatetimeUnits = {"YEAR",   "QUARTER",     "MONTH",      "WEEK",
	                                                "DAY",    "DAYOFYEAR",   "HOUR",       "MINUTE",
	                                                "SECOND", "MILLISECOND", "MICROSECOND"};
	string unit = StringUtil::Upper(constant.value.GetValue<string>());
	if (kDatetimeUnits.find(unit) == kDatetimeUnits.end()) {
		return string();
	}
	return unit;
}

} // namespace

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
	// The plan stores a RANGE bound as `<order key> - <offset>` / `+ <offset>`; recover the bare offset.
	// DuckDB may wrap the order key in casts inconsistently between the ORDER BY and the frame expression
	// (e.g. `CAST(date AS TIMESTAMP)` for interval arithmetic) — compare the two modulo casts, or falling
	// back to rendering the whole bound expression would re-bind the SUBTRACTION as the offset (a
	// TIMESTAMP), which cannot be added to the order key.
	auto peel_casts = [](const Expression *e) {
		while (e && e->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
			e = e->Cast<BoundCastExpression>().child.get();
		}
		return e;
	};
	if (!window.orders.empty() && expr && expr->GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		const auto &func_expr = expr->Cast<BoundFunctionExpression>();
		const string &func_name = func_expr.function.name;
		if (func_expr.children.size() == 2 && ((preceding && func_name == "-") || (!preceding && func_name == "+"))) {
			const Expression *order_peeled = peel_casts(window.orders[0].expression.get());
			const Expression *left_peeled = peel_casts(func_expr.children[0].get());
			if (order_peeled && left_peeled) {
				unique_ptr<Expression> order_copy = order_peeled->Copy();
				unique_ptr<Expression> left_copy = left_peeled->Copy();
				if (ExpressionToAliasedString(left_copy) == ExpressionToAliasedString(order_copy)) {
					return ExpressionToAliasedString(func_expr.children[1]);
				}
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
	// `WITH ORDINALITY` compiles to a distinctive `row_number() OVER (ROWS BETWEEN UNBOUNDED PRECEDING AND
	// CURRENT ROW)` — no PARTITION BY, no ORDER BY, and a ROWS frame (end = CURRENT_ROW_ROWS). The numbering
	// follows an unspecified scan order, so the rewrite cannot reproduce it. Refuse. This is narrowed to the
	// ROWS-frame signature so a plain `row_number() OVER ()` (which DuckDB gives a RANGE frame,
	// end = CURRENT_ROW_RANGE) is not affected.
	if (window.GetExpressionType() == ExpressionType::WINDOW_ROW_NUMBER && window.orders.empty() &&
	    window.partitions.empty() && window.start == WindowBoundary::UNBOUNDED_PRECEDING &&
	    window.end == WindowBoundary::CURRENT_ROW_ROWS) {
		ThrowLptsNotImplemented("LPTS_UNSUPPORTED_WINDOW", dialect, "window_function", "row_number", "BOUND_WINDOW",
		                        "WITH ORDINALITY (row_number over an unspecified row order) cannot be reproduced");
	}
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
	// Quantile-family aggregates used as window functions: the quantile argument was consumed into the
	// bind data at bind time (the children hold only the input), so it must be re-appended — same recovery
	// as the aggregate path.
	if (window.aggregate && window.bind_info && !window.children.empty()) {
		const string &agg_name = window.aggregate->name;
		if (IsQuantileAggregate(agg_name)) {
			result << ", " << QuantileArgumentFromBindInfo(*window.bind_info);
		} else if (agg_name == "approx_quantile") {
			result << ", " << ApproxQuantileArgumentFromBindInfo(*window.bind_info);
		} else if (agg_name == "reservoir_quantile") {
			result << ", " << ReservoirQuantileArgumentsFromBindInfo(*window.bind_info);
		}
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
	// VARIANT has no faithful, re-parseable SQL literal (Value::ToSQLString emits a `VARIANT(...)` form that
	// does not round-trip), so any expression whose type contains VARIANT (directly or nested in a
	// list/struct/map) is untranslatable — fail rather than emit a value that differs from the original.
	if (TypeContainsVariant(expression->return_type)) {
		ThrowLptsNotImplemented("LPTS_UNSUPPORTED_TYPE", dialect, "type", "VARIANT", "expression",
		                        "VARIANT values have no faithful SQL literal");
	}
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
		// Non-DuckDB dialects render constants through the dialect-aware path (target-specific
		// literal/cast forms, bare NULL for untyped SQLNULL). The faithful, re-parseable rendering
		// below is DuckDB-specific.
		if (!IsDuckDBDialect(dialect)) {
			expr_str << RenderConstantForDialect(constant, dialect);
			break;
		}
		// Render the constant as a re-parseable, type-faithful SQL literal. Value::ToSQLString() quotes and
		// self-types the common cases (BLOB, DATE/TIME/TIMESTAMP, VARCHAR, STRUCT, LIST, VARIANT, ...). For the
		// types it leaves bare and that would re-parse to a different type (TINYINT/HUGEINT/FLOAT/DECIMAL/BIT/
		// BIGNUM/unsigned ints), emit CAST('<text>' AS <type>): the string form casts correctly for all of them
		// (e.g. CAST('00000001' AS BIT) keeps the bitstring, unlike the bare 00000001 which is the integer 1).
		if (constant.value.IsNull()) {
			// A bare NULL re-parses as the untyped SQLNULL; a typed NULL (e.g. NULL::INTEGER, common in
			// view definitions) must keep its type so the result column type — and the type-sensitive
			// bag-hash — match the original.
			if (constant.value.type().id() == LogicalTypeId::SQLNULL) {
				expr_str << "NULL";
			} else {
				expr_str << "CAST(NULL AS " << constant.value.type().ToString() << ")";
			}
		} else if (constant.value.type().id() == LogicalTypeId::TYPE) {
			// A TYPE-typed value (from get_type()/make_type()) has no literal and no VARCHAR cast;
			// reconstruct it as get_type(CAST(NULL AS <the type>)).
			const string type_name = constant.value.ToString();
			if (type_name == "NULL" || type_name == "\"NULL\"") {
				expr_str << "get_type(NULL)";
			} else {
				expr_str << "get_type(CAST(NULL AS " << type_name << "))";
			}
		} else if (TypeNeedsStructuralRender(constant.value.type())) {
			// A value containing an unnamed struct (ROW) or a UNION anywhere in its type (e.g.
			// `(0, 0) < ANY(...)`, list_zip's STRUCT(DATE)[], `true::UNION(...)`) — no literal or
			// text-cast form exists, so rebuild the value structurally.
			expr_str << RenderConstantContainingUnnamedStruct(constant.value);
		} else if (ConstantNeedsExplicitCast(constant.value.type()) ||
		           IntegerConstantNeedsExplicitCast(constant.value)) {
			expr_str << "CAST('" << EscapeSingleQuotes(constant.value.ToString()) << "' AS "
			         << constant.value.type().ToString() << ")";
		} else if (constant.value.type().id() == LogicalTypeId::VARCHAR &&
		           StringValue::Get(constant.value).find('\0') != string::npos) {
			// A NUL byte cannot appear inside a SQL string literal (the parser stops at it). Rebuild the
			// value as a concatenation with chr(0) for each NUL: 'goo' || chr(0) || 'se'.
			const string &s = StringValue::Get(constant.value);
			expr_str << "(";
			bool first_part = true;
			size_t start = 0;
			while (start <= s.size()) {
				size_t nul = s.find('\0', start);
				const string part = s.substr(start, (nul == string::npos ? s.size() : nul) - start);
				if (!first_part) {
					expr_str << " || ";
				}
				expr_str << "'" << EscapeSingleQuotes(part) << "'";
				first_part = false;
				if (nul == string::npos) {
					break;
				}
				expr_str << " || chr(0)";
				start = nul + 1;
			}
			expr_str << ")";
		} else {
			expr_str << constant.value.ToSQLString();
		}
		break;
	}
	case ExpressionClass::BOUND_COMPARISON: {
		const BoundComparisonExpression &cmp = expression->Cast<BoundComparisonExpression>();
		expr_str << RenderComparisonForDialect(ExpressionToAliasedString(cmp.left),
		                                       ExpressionToAliasedString(cmp.right), cmp.GetExpressionType(), dialect);
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
		// A cast to a type containing an UNNAMED struct (e.g. aligning row(...) values, or feeding
		// map_from_entries) cannot be written — the type has no SQL syntax. When the cast only strips
		// field names (same structure and child types), it is a no-op for re-binding: render the child
		// bare and let the binder re-derive whatever internal cast it needs.
		if (TypeContainsUnnamedStruct(cast_expr.return_type) &&
		    TypesEqualIgnoringStructNames(cast_expr.child->return_type, cast_expr.return_type)) {
			expr_str << ExpressionToAliasedString(cast_expr.child);
			break;
		}
		// A cast whose target is (or contains) the untyped-NULL type is a binder-internal artifact of a
		// fully-folded expression (e.g. a lambda body over an empty list): `CAST(x AS "NULL")` does not
		// parse, and dropping it lets the re-bind derive its own typing.
		if (TypeContainsNullType(cast_expr.return_type)) {
			expr_str << ExpressionToAliasedString(cast_expr.child);
			break;
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
		// `alias(expr)` returns the *name* of its argument as a string. LPTS rewrites columns to its own
		// names (t0_*) and re-renders expressions, so the name alias() observes — and therefore its value —
		// cannot match the original. It is untranslatable; fail rather than emit a different value.
		if (func_expr.function.name == "alias") {
			ThrowLptsNotImplemented("LPTS_UNSUPPORTED_FUNCTION", dialect, "function", "alias", "BOUND_FUNCTION",
			                        "alias() reflects column names that LPTS rewrites");
		}
		// list/array slicing with an OMITTED bound (`s[:2]`, `s[2:]`, and the array_pop_back/front
		// rewrites) marks the omitted side with a sentinel constant of the *list's own type* (an empty
		// list value). `array_slice(s, <list>, ...)` does not bind — "bounds must be a BIGINT" — and no
		// function-call form can express the omission; only the bracket syntax can. Render brackets,
		// leaving sentinel sides empty.
		if ((func_expr.function.name == "array_slice" || func_expr.function.name == "list_slice") &&
		    (func_expr.children.size() == 3 || func_expr.children.size() == 4)) {
			auto is_omitted_bound = [](const unique_ptr<Expression> &e) {
				return e->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT &&
				       (e->return_type.id() == LogicalTypeId::LIST || e->return_type.id() == LogicalTypeId::ARRAY);
			};
			// Full-range step -1 slice is how list_reverse compiles; `[::-1]` itself does not parse.
			if (func_expr.children.size() == 4 && is_omitted_bound(func_expr.children[1]) &&
			    is_omitted_bound(func_expr.children[2]) &&
			    func_expr.children[3]->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
				const auto &step_val = func_expr.children[3]->Cast<BoundConstantExpression>().value;
				if (!step_val.IsNull() && step_val.type().IsIntegral() && step_val.GetValue<int64_t>() == -1) {
					expr_str << "list_reverse(" << ExpressionToAliasedString(func_expr.children[0]) << ")";
					break;
				}
			}
			if (is_omitted_bound(func_expr.children[1]) || is_omitted_bound(func_expr.children[2])) {
				const bool has_step = func_expr.children.size() == 4;
				expr_str << "(" << ExpressionToAliasedString(func_expr.children[0]) << ")[";
				if (!is_omitted_bound(func_expr.children[1])) {
					expr_str << ExpressionToAliasedString(func_expr.children[1]);
				}
				expr_str << ":";
				if (!is_omitted_bound(func_expr.children[2])) {
					expr_str << ExpressionToAliasedString(func_expr.children[2]);
				} else if (has_step) {
					// With a step, an omitted end is written as the `-` end marker (`a[start:-:step]`);
					// a bare `start::step` does not parse.
					expr_str << "-";
				}
				if (has_step) {
					expr_str << ":" << ExpressionToAliasedString(func_expr.children[3]);
				}
				expr_str << "]";
				break;
			}
		}
		// The struct variant `date_part(['year','month'], d)` erases its constant list argument at bind
		// time (Function::EraseArgument) — only the date child remains, so a plain rendering emits
		// `date_part(d)`, which does not bind. The part names live on as the STRUCT return type's field
		// names (in order); rebuild the list literal from them.
		if ((func_expr.function.name == "date_part" || func_expr.function.name == "datepart") &&
		    func_expr.return_type.id() == LogicalTypeId::STRUCT && func_expr.children.size() == 1) {
			expr_str << func_expr.function.name << "([";
			for (idx_t i = 0; i < StructType::GetChildCount(func_expr.return_type); i++) {
				if (i > 0) {
					expr_str << ", ";
				}
				expr_str << "'" << EscapeSingleQuotes(StructType::GetChildName(func_expr.return_type, i)) << "'";
			}
			expr_str << "], " << ExpressionToAliasedString(func_expr.children[0]) << ")";
			break;
		}
		ValidateFunctionForDialect(func_expr, dialect);
		if (dialect == SqlDialect::FELDERA && func_expr.children.size() == 1 &&
		    (func_expr.function.name == "year" || func_expr.function.name == "month")) {
			expr_str << "EXTRACT(" << StringUtil::Upper(func_expr.function.name) << " FROM "
			         << ExpressionToAliasedString(func_expr.children[0]) << ")";
			break;
		}
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
			if (IsDuckDBDialect(dialect) && (func_name == "position" || func_name == "substring" ||
			                                 func_name == "overlay" || func_name == "trim")) {
				emit_name = "\"" + func_name + "\"";
			}
			// Spark and Feldera take the datediff unit as a bare keyword, not a string:
			// `datediff('day', a, b)` fails with INVALID_PARAMETER_VALUE.DATETIME_UNIT,
			// it wants `datediff(DAY, a, b)`. Only datediff is affected — Spark's
			// date_trunc/date_part really do take a quoted string, so they are left alone.
			if ((dialect == SqlDialect::SPARK || dialect == SqlDialect::FELDERA) && func_name == "datediff" &&
			    child_count >= 1) {
				string unit = UnquotedDatetimeUnitLiteral(func_expr.children[0]);
				if (!unit.empty()) {
					expr_str << emit_name << "(" << unit;
					for (idx_t i = 1; i < child_count; i++) {
						expr_str << ", " << ExpressionToAliasedString(func_expr.children[i]);
					}
					expr_str << ")";
					break;
				}
			}
			expr_str << emit_name << "(";
			if (UsesFormatFirstDateFunctionArgumentOrder(func_expr.function.name, dialect) && child_count >= 2) {
				expr_str << ExpressionToAliasedString(func_expr.children[1]);
				expr_str << ", ";
				expr_str << ExpressionToAliasedString(func_expr.children[0]);
				for (idx_t i = 2; i < child_count; i++) {
					expr_str << ", ";
					expr_str << ExpressionToAliasedString(func_expr.children[i]);
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
				if (func_expr.function.bind_lambda != nullptr && func_expr.bind_info) {
					auto &bind_data = func_expr.bind_info->Cast<ListLambdaBindData>();
					if (bind_data.lambda_expr) {
						for (idx_t ai = 0; ai < func_expr.function.arguments.size(); ai++) {
							if (func_expr.function.arguments[ai].id() == LogicalTypeId::LAMBDA) {
								lambda_arg_pos = ai;
								break;
							}
						}
						std::ostringstream lam;
						std::map<idx_t, string> param_map;
						CollectLambdaParamNames(*bind_data.lambda_expr, param_map);
						idx_t total_refs = param_map.empty() ? 0 : param_map.rbegin()->first + 1;
						// A lambda that closes over outer columns binds them as trailing "parameters": the real
						// lambda parameters occupy body indices [0, real_count) (reverse declaration order) and the
						// captures occupy [real_count, total_refs), mapping to the function's non-list children
						// (child_count..). Only the real parameters go in the lambda's parameter list; captures are
						// rendered as the outer expression (so LPTS's column renaming applies), else e.g.
						// `x -> x + n` would render `lambda n, x: x + n` and shadow the outer column.
						idx_t capture_count =
						    func_expr.children.size() > child_count ? func_expr.children.size() - child_count : 0;
						idx_t real_count = total_refs > capture_count ? total_refs - capture_count : total_refs;
						// A fully constant-folded body (e.g. reducing an empty list) references no parameters,
						// but the emitted lambda must still declare the arity the function requires —
						// `lambda : body` does not parse. Pad with unused placeholders (only when no captures
						// shift the body indices).
						const idx_t min_params =
						    (func_expr.function.name == "list_reduce" || func_expr.function.name == "array_reduce" ||
						     func_expr.function.name == "reduce")
						        ? 2
						        : 1;
						if ((capture_count == 0 || total_refs == 0) && real_count < min_params) {
							real_count = min_params;
						}
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
							if (cap_child < func_expr.children.size()) {
								captures[idx] = ExpressionToAliasedString(func_expr.children[cap_child]);
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
						lam << ExpressionToAliasedString(bind_data.lambda_expr);
						lambda_captures = std::move(saved_captures);
						lambda_param_names = std::move(saved_params);
						lambda_text = lam.str();
					}
				}
				const bool uses_named_arguments =
				    func_name == "struct_insert" || func_name == "struct_update" || func_name == "write_log";
				idx_t emitted = 0;
				for (idx_t i = 0; i < child_count; i++) {
					if (!lambda_text.empty() && i == lambda_arg_pos) {
						if (emitted++ > 0) {
							expr_str << ", ";
						}
						expr_str << lambda_text;
					}
					if (emitted++ > 0) {
						expr_str << ", ";
					}
					if (is_struct_pack && i < StructType::GetChildCount(func_expr.return_type)) {
						expr_str << "\"" << StructType::GetChildName(func_expr.return_type, i) << "\" := ";
					} else if (uses_named_arguments && i > 0 && func_expr.children[i]->HasAlias() &&
					           func_expr.children[i]->GetAlias().find('(') == string::npos) {
						expr_str << "\"" << func_expr.children[i]->GetAlias() << "\" := ";
					}
					string converted_date_format;
					if (i == 1 && IsDateFormatFunction(func_expr.function.name) &&
					    TryRenderConvertedDateFormat(func_expr.children[i], dialect, converted_date_format)) {
						expr_str << converted_date_format;
					} else if (func_name == "equi_width_bins" &&
					           func_expr.children[i]->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT &&
					           func_expr.children[i]->return_type.id() == LogicalTypeId::VARCHAR) {
						// equi_width_bins is overloaded on VARCHAR vs numeric/timestamp; a bare string
						// literal re-binds as STRING_LITERAL (castable to all) — "could not choose a best
						// candidate". Pin the VARCHAR overload with an explicit cast.
						expr_str << "CAST(" << ExpressionToAliasedString(func_expr.children[i]) << " AS VARCHAR)";
					} else {
						expr_str << ExpressionToAliasedString(func_expr.children[i]);
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
		auto cap = lambda_captures.find(ref.index);
		if (cap != lambda_captures.end()) {
			expr_str << cap->second;
		} else {
			auto par = lambda_param_names.find(ref.index);
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
