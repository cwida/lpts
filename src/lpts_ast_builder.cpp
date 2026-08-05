#include "storage/ducklake_scan.hpp"

#include "lpts_pipeline.hpp"
#include "lpts_helpers.hpp"
#include "lpts_debug.hpp"
#include "dialect_function_map.hpp"
#include "lpts_expression_renderer.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/parser/keyword_helper.hpp"

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
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_window.hpp"
#include "duckdb/planner/operator/logical_any_join.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_set_operation.hpp"
#include "duckdb/planner/operator/logical_order.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/planner/operator/logical_top_n.hpp"
#include "duckdb/planner/operator/logical_distinct.hpp"
#include "duckdb/planner/operator/logical_dummy_scan.hpp"
#include "duckdb/planner/operator/logical_empty_result.hpp"
#include "duckdb/planner/operator/logical_column_data_get.hpp"
#include "duckdb/planner/operator/logical_expression_get.hpp"
#include "duckdb/planner/operator/logical_positional_join.hpp"
#include "duckdb/planner/operator/logical_sample.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_recursive_cte.hpp"
#include "duckdb/planner/operator/logical_delim_get.hpp"
#include "duckdb/planner/operator/logical_dependent_join.hpp"
#include "duckdb/planner/operator/logical_unnest.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"

namespace duckdb {

class AstBuilder {
private:
	/// Wrapper around ColumnBinding for use as std::map key.
	struct MappableColumnBinding {
		ColumnBinding cb;
		explicit MappableColumnBinding(const ColumnBinding &_cb) : cb(_cb) {
		}
		bool operator<(const MappableColumnBinding &other) const {
			return std::tie(cb.table_index, cb.column_index) < std::tie(other.cb.table_index, other.cb.column_index);
		}
	};

	static string SanitizeIdentifierFragment(const string &input) {
		string result;
		result.reserve(input.size());
		for (char c : input) {
			if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') {
				result += c;
			} else {
				result += '_';
			}
		}
		return result.empty() ? "col" : result;
	}

	/// Metadata for one column as it flows through the plan.
	struct ColStruct {
		const idx_t table_index;
		string column_name; ///< Original physical column name (or computed-expression fragment).
		string alias;       ///< Optional alias for expressions. Empty if not set.

		ColStruct(const idx_t _table_index, string _column_name, string _alias)
		    : table_index(_table_index), column_name(std::move(_column_name)), alias(std::move(_alias)) {
		}

		/// Produce "t{table_index}_{alias || column_name}".
		string ToUniqueColumnName() const {
			string base = alias.empty() ? column_name : alias;
			return "t" + std::to_string(table_index) + "_" + SanitizeIdentifierFragment(base);
		}
	};

	// A set of emitted CTE column names with case-insensitive membership. DuckDB resolves identifiers
	// case-insensitively, so two generated names differing only in case (t1_hello vs t1_HeLlO) would
	// collide when referenced. Deduping against this set treats them as equal, so the second gets a suffix.
	struct CaseInsensitiveNameSet {
		unordered_set<string> names;
		bool count(const string &name) const {
			return names.count(StringUtil::Lower(name)) > 0;
		}
		void insert(const string &name) {
			names.insert(StringUtil::Lower(name));
		}
	};

	static unique_ptr<ColStruct> MakeDedupedColumn(idx_t table_index, string column_name, string alias,
	                                               CaseInsensitiveNameSet &seen_names, idx_t suffix_seed) {
		string base_column_name = column_name;
		string base_alias = alias;
		idx_t suffix = suffix_seed;
		while (true) {
			auto candidate = make_uniq<ColStruct>(table_index, column_name, alias);
			string unique_name = candidate->ToUniqueColumnName();
			if (!seen_names.count(unique_name)) {
				seen_names.insert(unique_name);
				return candidate;
			}
			string suffix_text = "_" + std::to_string(suffix++);
			if (base_alias.empty()) {
				column_name = base_column_name + suffix_text;
			} else {
				alias = base_alias + suffix_text;
			}
		}
	}

	static bool HasBinding(const vector<ColumnBinding> &bindings, const ColumnBinding &binding) {
		for (const auto &candidate : bindings) {
			if (candidate == binding) {
				return true;
			}
		}
		return false;
	}

	static void AddUniqueBinding(vector<ColumnBinding> &bindings, const ColumnBinding &binding) {
		if (!HasBinding(bindings, binding)) {
			bindings.push_back(binding);
		}
	}

	static void CollectColumnRefs(Expression &expr, vector<ColumnBinding> &bindings) {
		if (expr.type == ExpressionType::BOUND_COLUMN_REF) {
			AddUniqueBinding(bindings, expr.Cast<BoundColumnRefExpression>().binding);
		}
		ExpressionIterator::EnumerateChildren(expr, [&](unique_ptr<Expression> &child) {
			if (child) {
				CollectColumnRefs(*child, bindings);
			}
		});
	}

	vector<string> OutputColumnNames(LogicalOperator &op, const char *context) const {
		vector<string> result;
		for (const ColumnBinding &cb : op.GetColumnBindings()) {
			result.push_back(FindColumnBinding(cb, context)->ToUniqueColumnName());
		}
		return result;
	}

	static vector<ColumnBinding> ChildBindings(LogicalOperator &op) {
		vector<ColumnBinding> result;
		for (auto &child : op.children) {
			auto bindings = child->GetColumnBindings();
			result.insert(result.end(), bindings.begin(), bindings.end());
		}
		return result;
	}

	static void AppendMappedJoinColumns(const vector<string> &child_cols, const vector<idx_t> &projection_map,
	                                    vector<string> &result) {
		if (projection_map.empty()) {
			result.insert(result.end(), child_cols.begin(), child_cols.end());
			return;
		}
		for (auto idx : projection_map) {
			if (idx >= child_cols.size()) {
				throw InternalException("LPTS JOIN: projection map index %llu out of bounds for %llu child columns",
				                        (unsigned long long)idx, (unsigned long long)child_cols.size());
			}
			result.push_back(child_cols[idx]);
		}
	}

	vector<string> BuildJoinSelectExpressions(LogicalOperator &op, const vector<unique_ptr<AstNode>> &child_nodes,
	                                          idx_t expected_count) const {
		auto *join = dynamic_cast<LogicalJoin *>(&op);
		if (!join || child_nodes.size() != 2 || (!join->HasProjectionMap() && join->join_type != JoinType::MARK)) {
			return {};
		}
		if (join->join_type == JoinType::MARK) {
			return {};
		}

		vector<string> result;
		auto left_cols = child_nodes[0]->OutputColumnNames();
		auto right_cols = child_nodes[1]->OutputColumnNames();
		if (join->join_type == JoinType::RIGHT_SEMI || join->join_type == JoinType::RIGHT_ANTI) {
			AppendMappedJoinColumns(right_cols, join->right_projection_map, result);
		} else {
			AppendMappedJoinColumns(left_cols, join->left_projection_map, result);
			if (join->join_type != JoinType::SEMI && join->join_type != JoinType::ANTI) {
				AppendMappedJoinColumns(right_cols, join->right_projection_map, result);
			}
		}
		if (result.size() != expected_count) {
			return {};
		}
		return result;
	}

	void EnsureJoinChildEmitsColumn(AstJoinNode &join_node, const string &column_name) const {
		if (std::find(join_node.cte_column_names.begin(), join_node.cte_column_names.end(), column_name) !=
		    join_node.cte_column_names.end()) {
			return;
		}
		if (join_node.select_expressions.empty()) {
			join_node.select_expressions = join_node.cte_column_names;
		}
		join_node.cte_column_names.push_back(column_name);
		join_node.select_expressions.push_back(column_name);
	}

	bool TryResolveChildBindingName(const ColumnBinding &binding, string &column_name) const {
		auto it = column_map.find(MappableColumnBinding(binding));
		if (it != column_map.end()) {
			column_name = it->second->ToUniqueColumnName();
			return true;
		}
		return false;
	}

	void EnsureJoinChildReferencedBindings(const vector<unique_ptr<AstNode>> &child_nodes,
	                                       const vector<ColumnBinding> &refs) const {
		if (child_nodes.size() != 1 || child_nodes[0]->NodeType() != "Join") {
			return;
		}
		auto &join_node = static_cast<AstJoinNode &>(*child_nodes[0]);
		auto child_outputs = join_node.OutputColumnNames();
		for (auto &binding : refs) {
			string column_name;
			if (!TryResolveChildBindingName(binding, column_name)) {
				continue;
			}
			if (std::find(child_outputs.begin(), child_outputs.end(), column_name) != child_outputs.end()) {
				continue;
			}
			EnsureJoinChildEmitsColumn(join_node, column_name);
			child_outputs.push_back(column_name);
		}
	}

	void EnsureAggregateChildReferencedBindings(LogicalOperator &op,
	                                            const vector<unique_ptr<AstNode>> &child_nodes) const {
		if (op.type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
			return;
		}
		auto &aggregate = op.Cast<LogicalAggregate>();
		vector<ColumnBinding> refs;
		for (auto &group : aggregate.groups) {
			CollectColumnRefs(*group, refs);
		}
		for (auto &agg_expr : aggregate.expressions) {
			CollectColumnRefs(*agg_expr, refs);
		}
		EnsureJoinChildReferencedBindings(child_nodes, refs);
	}

	void EnsureProjectionChildReferencedBindings(LogicalOperator &op,
	                                             const vector<unique_ptr<AstNode>> &child_nodes) const {
		if (op.type != LogicalOperatorType::LOGICAL_PROJECTION) {
			return;
		}
		auto &projection = op.Cast<LogicalProjection>();
		vector<ColumnBinding> refs;
		for (auto &expr : projection.expressions) {
			CollectColumnRefs(*expr, refs);
		}
		EnsureJoinChildReferencedBindings(child_nodes, refs);
	}

	/// SQL dialect for expression serialization (function renaming, etc.)
	SqlDialect dialect = SqlDialect::DUCKDB;

	/// Client context for runtime queries (e.g. DuckLake current snapshot).
	ClientContext &context;

	/// Re-target emitted table references at the destination system's catalog/schema.
	///
	/// The optimized plan carries the catalog/schema the query was planned against.
	/// When LPTS is used to transpile for another system, those local names are just a
	/// stand-in for the remote schema and must not appear in the output — the target
	/// expects its own qualification (e.g. `spark_catalog`.`bronze`). These come from
	/// `lpts_output_catalog` / `lpts_output_schema`; empty means "keep the planned
	/// name", so unset behaviour is unchanged.
	string output_catalog;
	string output_schema;

	/// Emit table references with no catalog/schema qualification at all, so the
	/// target system resolves them through its own session default (`USE ...`).
	/// Useful when one rendered query must run against several destinations that
	/// agree on table names but not on catalog/schema layout. Takes precedence
	/// over `output_catalog` / `output_schema`.
	bool output_unqualified = false;

	void ApplyOutputQualificationOverrides(string &catalog_name, string &schema_name) const {
		if (output_unqualified) {
			catalog_name.clear();
			schema_name.clear();
			return;
		}
		if (!output_catalog.empty()) {
			catalog_name = output_catalog;
		}
		if (!output_schema.empty()) {
			schema_name = output_schema;
		}
	}

	LptsExpressionRenderer expression_renderer;

	/// Cache: DuckLake catalog name → current snapshot_id.
	/// Populated lazily on first DuckLake scan per catalog.
	unordered_map<string, idx_t> ducklake_current_snapshots;

	static bool IsDuckDBDialect(SqlDialect dialect) {
		return dialect == SqlDialect::DUCKDB;
	}

	static bool IsSupportedSqlJoinType(JoinType join_type) {
		switch (join_type) {
		case JoinType::INNER:
		case JoinType::LEFT:
		case JoinType::RIGHT:
		case JoinType::OUTER:
		case JoinType::SEMI:
		case JoinType::ANTI:
		case JoinType::SINGLE:
		case JoinType::MARK:
		case JoinType::RIGHT_SEMI:
		case JoinType::RIGHT_ANTI:
			return true;
		default:
			return false;
		}
	}

	static void ValidateJoinTypeForDialect(JoinType join_type, SqlDialect dialect, const string &context) {
		if (!IsSupportedSqlJoinType(join_type)) {
			ThrowLptsNotImplemented("LPTS_UNSUPPORTED_JOIN_TYPE", dialect, "join_type", EnumUtil::ToString(join_type),
			                        context, "join type is not implemented by LPTS");
		}
		if (IsDuckDBDialect(dialect)) {
			return;
		}
		if (join_type == JoinType::OUTER && dialect == SqlDialect::MYSQL_MARIADB) {
			ThrowLptsNotImplemented("LPTS_UNSUPPORTED_JOIN_TYPE", dialect, "join_type", "OUTER", context,
			                        "MySQL/MariaDB do not support FULL OUTER JOIN");
		}
		if (join_type == JoinType::SEMI || join_type == JoinType::ANTI || join_type == JoinType::RIGHT_SEMI ||
		    join_type == JoinType::RIGHT_ANTI) {
			ThrowLptsNotImplemented("LPTS_UNSUPPORTED_JOIN_TYPE", dialect, "join_type", EnumUtil::ToString(join_type),
			                        context, "SEMI/ANTI JOIN SQL syntax is not portable for this dialect");
		}
	}

	static string RenderNullLiteral(const LogicalType &type, SqlDialect dialect) {
		if (dialect == SqlDialect::DUCKDB) {
			return "NULL::" + type.ToString();
		}
		switch (type.id()) {
		case LogicalTypeId::BOOLEAN:
			return "CAST(NULL AS BOOLEAN)";
		case LogicalTypeId::TINYINT:
			return "CAST(NULL AS TINYINT)";
		case LogicalTypeId::SMALLINT:
			return "CAST(NULL AS SMALLINT)";
		case LogicalTypeId::INTEGER:
			return "CAST(NULL AS INTEGER)";
		case LogicalTypeId::BIGINT:
			return "CAST(NULL AS BIGINT)";
		case LogicalTypeId::FLOAT:
			return "CAST(NULL AS FLOAT)";
		case LogicalTypeId::DOUBLE:
			return "CAST(NULL AS DOUBLE)";
		case LogicalTypeId::DECIMAL:
			return "CAST(NULL AS " + type.ToString() + ")";
		case LogicalTypeId::VARCHAR:
			return "CAST(NULL AS VARCHAR)";
		case LogicalTypeId::DATE:
			return "CAST(NULL AS DATE)";
		case LogicalTypeId::TIMESTAMP:
			return "CAST(NULL AS TIMESTAMP)";
		default:
			ThrowLptsNotImplemented("LPTS_UNSUPPORTED_TYPE", dialect, "type", type.ToString(), "LOGICAL_EMPTY_RESULT",
			                        "no verified target dialect null literal type mapping");
		}
	}

	string RenderJoinComparison(const string &lhs, const string &rhs, ExpressionType comparison) const {
		if (dialect == SqlDialect::SPARK && comparison == ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
			return "(" + lhs + " <=> " + rhs + ")";
		}
		if (dialect == SqlDialect::SPARK && comparison == ExpressionType::COMPARE_DISTINCT_FROM) {
			return "(NOT (" + lhs + " <=> " + rhs + "))";
		}
		return "(" + lhs + " " + ExpressionTypeToOperator(comparison) + " " + rhs + ")";
	}

	static string SampleMethodToSql(SampleMethod method) {
		switch (method) {
		case SampleMethod::SYSTEM_SAMPLE:
			return "system";
		case SampleMethod::BERNOULLI_SAMPLE:
			return "bernoulli";
		case SampleMethod::RESERVOIR_SAMPLE:
			return "reservoir";
		default:
			throw NotImplementedException("LPTS_UNSUPPORTED_SAMPLE: sample method %s is not implemented",
			                              EnumUtil::ToString(method));
		}
	}

	static string SampleOptionsToSql(const SampleOptions &options) {
		string clause = SampleMethodToSql(options.method);
		clause += "(" + options.sample_size.ToSQLString();
		clause += options.is_percentage ? " PERCENT)" : " ROWS)";
		if (options.seed.IsValid()) {
			clause += " REPEATABLE (" + std::to_string(options.GetSeed()) + ")";
		}
		return clause;
	}

	/// Query the current snapshot_id for a DuckLake catalog.
	idx_t GetDuckLakeCurrentSnapshot(const string &catalog_name) {
		auto it = ducklake_current_snapshots.find(catalog_name);
		if (it != ducklake_current_snapshots.end()) {
			return it->second;
		}
		Connection con(*context.db);
		auto result =
		    con.Query("SELECT id FROM " + KeywordHelper::WriteOptionallyQuoted(catalog_name) + ".current_snapshot()");
		idx_t snap_id = DConstants::INVALID_INDEX;
		if (!result->HasError() && result->RowCount() > 0) {
			snap_id = result->GetValue(0, 0).GetValue<idx_t>();
		}
		ducklake_current_snapshots[catalog_name] = snap_id;
		return snap_id;
	}

	/// Global map: ColumnBinding → ColStruct.
	/// Populated bottom-up; each operator registers its output columns here.
	std::map<MappableColumnBinding, unique_ptr<ColStruct>> column_map;

	/// Maps DELIM_GET table_index → source column names (from the outer/left CTE).
	/// Populated by PreregisterDelimGetColumns before the right subtree is traversed.
	unordered_map<idx_t, vector<string>> delim_get_source_col_names;

	/// Maps LogicalMaterializedCTE::table_index → the actual body output column names.
	/// DuckDB can prune unused CTE body columns while LogicalCTERef::bound_columns still
	/// carries the original user-visible names, so CTE refs must follow the body output arity.
	unordered_map<idx_t, vector<string>> materialized_cte_body_column_names;

	/// Maps LogicalProjection nodes to lower-scope bindings that must be carried through
	/// as hidden pass-through columns. Some optimizer/extension rewrites leave aggregate
	/// arguments bound to a pre-projection column; without carrying that binding upward, LPTS
	/// can emit a stale CTE column name in the aggregate SELECT list.
	unordered_map<const LogicalOperator *, vector<ColumnBinding>> extra_projection_outputs;

	const unique_ptr<ColStruct> &FindColumnBinding(const ColumnBinding &binding, const char *context) const {
		auto it = column_map.find(MappableColumnBinding(binding));
		if (it != column_map.end()) {
			return it->second;
		}
		throw NotImplementedException(
		    "LPTS_UNSUPPORTED_COLUMN_REF: %s column ref (%llu,%llu) is not implemented because it is not in "
		    "column_map",
		    context, (unsigned long long)binding.table_index, (unsigned long long)binding.column_index);
	}

	void RegisterChildBindingFallbacks(Expression &expr, const vector<ColumnBinding> &child_bindings) {
		if (expr.type == ExpressionType::BOUND_COLUMN_REF) {
			auto &bcr = expr.Cast<BoundColumnRefExpression>();
			if (column_map.find(MappableColumnBinding(bcr.binding)) == column_map.end() &&
			    bcr.binding.column_index < child_bindings.size()) {
				auto &src = FindColumnBinding(child_bindings[bcr.binding.column_index], "projection fallback");
				column_map[MappableColumnBinding(bcr.binding)] =
				    make_uniq<ColStruct>(src->table_index, src->column_name, src->alias);
			}
		}
		ExpressionIterator::EnumerateChildren(expr, [&](unique_ptr<Expression> &child) {
			if (child) {
				RegisterChildBindingFallbacks(*child, child_bindings);
			}
		});
	}

	bool EnsureBindingAvailableFrom(LogicalOperator *op, const ColumnBinding &binding) {
		if (!op) {
			return false;
		}
		if (HasBinding(op->GetColumnBindings(), binding)) {
			return true;
		}
		if (op->type != LogicalOperatorType::LOGICAL_PROJECTION || op->children.empty()) {
			return false;
		}
		if (!EnsureBindingAvailableFrom(op->children[0].get(), binding)) {
			return false;
		}
		AddUniqueBinding(extra_projection_outputs[op], binding);
		return true;
	}

	void MarkAggregateReferencedBindings(LogicalOperator *op) {
		if (!op) {
			return;
		}
		if (op->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY && !op->children.empty()) {
			auto &agg = op->Cast<LogicalAggregate>();
			vector<ColumnBinding> refs;
			for (const auto &group : agg.groups) {
				CollectColumnRefs(*group, refs);
			}
			for (const auto &expr : agg.expressions) {
				CollectColumnRefs(*expr, refs);
				if (expr->type == ExpressionType::BOUND_AGGREGATE) {
					auto &bound_agg = expr->Cast<BoundAggregateExpression>();
					if (bound_agg.filter) {
						CollectColumnRefs(*bound_agg.filter, refs);
					}
					if (bound_agg.order_bys) {
						for (auto &order : bound_agg.order_bys->orders) {
							CollectColumnRefs(*order.expression, refs);
						}
					}
				}
			}
			auto *child = op->children[0].get();
			const auto child_bindings = child->GetColumnBindings();
			for (const auto &ref : refs) {
				if (!HasBinding(child_bindings, ref)) {
					EnsureBindingAvailableFrom(child, ref);
				}
			}
		}
		for (auto &child : op->children) {
			MarkAggregateReferencedBindings(child.get());
		}
	}

	void MarkProjectionReferencedBindings(LogicalOperator *op) {
		if (!op) {
			return;
		}
		if (op->type == LogicalOperatorType::LOGICAL_PROJECTION && !op->children.empty()) {
			auto &proj = op->Cast<LogicalProjection>();
			vector<ColumnBinding> refs;
			for (const auto &expr : proj.expressions) {
				CollectColumnRefs(*expr, refs);
			}
			auto *child = op->children[0].get();
			const auto child_bindings = child->GetColumnBindings();
			for (const auto &ref : refs) {
				if (!HasBinding(child_bindings, ref)) {
					EnsureBindingAvailableFrom(child, ref);
				}
			}
		}
		for (auto &child : op->children) {
			MarkProjectionReferencedBindings(child.get());
		}
	}

	string ExpressionToAliasedString(const unique_ptr<Expression> &expression) const {
		return expression_renderer.ExpressionToAliasedString(expression);
	}

	// Partition a MARK join's conditions into the single NULL-propagating membership comparison and the
	// null-safe correlation links, so the renderer can build a 3-valued mark. A decorrelated IN/ANY/ALL
	// mark join's conditions split into null-safe correlation keys (`IS NOT DISTINCT FROM`, the
	// outer↔subquery link) and the NULL-propagating membership comparison (`=`, `<`, ...). When there is
	// exactly one such comparison, this captures its operand expressions (lhs_key/rhs_key) plus the
	// rendered correlation conditions. EXISTS subqueries are 2-valued (no NULL-propagating comparison) and
	// leave the keys empty. Used by both LOGICAL_COMPARISON_JOIN and the DELIM/DEPENDENT join path.
	void ExtractMarkComparison(const vector<JoinCondition> &conditions, string &lhs_key, string &rhs_key,
	                           vector<string> &correlation_conditions, vector<string> &membership_conditions,
	                           vector<string> &membership_comparisons, vector<string> &membership_lhs,
	                           vector<string> &membership_rhs, bool &has_equality) {
		has_equality = false;
		for (const auto &cc : conditions) {
			const ExpressionType cmp = cc.comparison;
			const string l = ExpressionToAliasedString(cc.left);
			const string r = ExpressionToAliasedString(cc.right);
			const string rendered = "(" + l + " " + ExpressionTypeToOperator(cmp) + " " + r + ")";
			if (cmp == ExpressionType::COMPARE_EQUAL || cmp == ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
				// Mirrors LogicalComparisonJoin::HasEquality — decides hash join (per-row AND) vs nested-loop
				// (per-condition independent OR) execution, and hence which SQL rendering is faithful.
				has_equality = true;
			}
			if (cmp == ExpressionType::COMPARE_NOT_DISTINCT_FROM || cmp == ExpressionType::COMPARE_DISTINCT_FROM) {
				// Null-safe correlation link (the decorrelated outer↔subquery key).
				correlation_conditions.push_back(rendered);
			} else {
				// A NULL-propagating membership comparison (`=`, `<`, ...). For the 3-valued mark, a row is
				// "indeterminate" (its per-row membership predicate is NULL, contributing NULL to the OR over
				// rows) when this comparison is not definitely false — i.e. it holds, or an operand is NULL.
				// AND-ing one such clause per comparison generalizes single- and multi-column IN alike; for a
				// single `=` it reduces to the plain `(lhs IS NULL OR rhs IS NULL)` null-check on non-matching
				// rows (a TRUE comparison is caught by the `matched` branch first).
				membership_conditions.push_back("(" + rendered + " OR (" + l + ") IS NULL OR (" + r + ") IS NULL)");
				membership_comparisons.push_back(rendered);
				membership_lhs.push_back(l);
				membership_rhs.push_back(r);
				lhs_key = l;
				rhs_key = r;
			}
		}
	}

	// Render a struct field-extraction pushdown. When DuckDB pushes `col.field[.subfield...]` into the
	// scan, the projected ColumnIndex keeps the base column as its primary index and carries a single-path
	// child chain naming the field path. Build a nested `struct_extract(...)` expression and report the
	// leaf field name (used as the column alias). Field positions are resolved to names via the column's
	// STRUCT LogicalType (walking down one level per child). Returns false if the path can't be resolved
	// (e.g. a non-STRUCT level), so the caller can fall back to the base column.
	bool RenderStructExtractPath(const string &base_col, const LogicalType &base_type, const ColumnIndex &ci,
	                             string &out_expr, string &out_leaf) {
		if (!ci.HasChildren()) {
			return false;
		}
		string expr = DialectQuoteIdent(base_col, dialect);
		string leaf = base_col;
		LogicalType cur_type = base_type;
		const ColumnIndex *node = &ci;
		while (node->HasChildren()) {
			const ColumnIndex &child = node->GetChildIndex(0);
			string field_name;
			if (child.HasPrimaryIndex()) {
				if (cur_type.id() != LogicalTypeId::STRUCT) {
					// A field path through a non-STRUCT level (list/map element) — not reproducible as a
					// plain struct_extract. Refuse rather than emit the wrong column.
					ThrowLptsNotImplemented("LPTS_STRUCT_EXTRACT_PUSHDOWN", dialect, "expression",
					                        "struct field extraction", "LOGICAL_GET",
					                        "field-extraction pushdown through a non-STRUCT type is not "
					                        "implemented");
				}
				const auto &child_types = StructType::GetChildTypes(cur_type);
				const idx_t field_pos = child.GetPrimaryIndex();
				if (field_pos >= child_types.size()) {
					ThrowLptsNotImplemented("LPTS_STRUCT_EXTRACT_PUSHDOWN", dialect, "expression",
					                        "struct field extraction", "LOGICAL_GET",
					                        "field-extraction pushdown references an out-of-range struct field");
				}
				field_name = StructType::GetChildName(cur_type, field_pos);
				cur_type = child_types[field_pos].second;
				expr = "struct_extract(" + expr + ", '" + EscapeSingleQuotes(field_name) + "')";
			} else {
				// Field referenced by name — VARIANT navigation. struct_extract would walk the VARIANT's
				// physical layout (keys/children/values) and fail; SQL bracket access `v['field']` performs
				// the logical navigation and yields VARIANT, matching the scan's output.
				field_name = child.GetFieldName();
				expr = expr + "['" + EscapeSingleQuotes(field_name) + "']";
				cur_type = LogicalType(LogicalTypeId::INVALID);
			}
			leaf = field_name;
			node = &child;
		}
		// A pushdown extract can carry a cast/restructure on the extracted value (e.g.
		// `s.nested_struct.a::BIGINT`, or `s.nested_struct::STRUCT(b BOOL, a INTEGER)` which reorders the
		// fields). The plain struct_extract above reproduces neither, so when the scan's emitted type differs
		// from the natural extracted type, wrap it in an explicit CAST to the emitted type. (For VARIANT
		// navigation — cur_type INVALID — the natural result is VARIANT; cast only when the scan emits a
		// shredded typed value instead.)
		if (ci.IsPushdownExtract() && ci.HasType() &&
		    ((cur_type.id() != LogicalTypeId::INVALID && ci.GetScanType() != cur_type) ||
		     (cur_type.id() == LogicalTypeId::INVALID && ci.GetScanType().id() != LogicalTypeId::VARIANT))) {
			expr = "CAST(" + expr + " AS " + ci.GetScanType().ToString() + ")";
		}
		out_expr = std::move(expr);
		out_leaf = std::move(leaf);
		return true;
	}

	string OrderByToAliasedString(const BoundOrderByNode &order) const {
		return expression_renderer.OrderByToAliasedString(order);
	}

	string WindowExpressionToAliasedString(const BoundWindowExpression &window) const {
		return expression_renderer.WindowExpressionToAliasedString(window);
	}

	bool TableFilterToSql(const TableFilter &filter, const string &column_name, string &result) const {
		return expression_renderer.TableFilterToSql(filter, column_name, result);
	}

	static bool ExpressionContainsColumnRef(const Expression &expr) {
		return LptsExpressionRenderer::ExpressionContainsColumnRef(expr);
	}

	static string QuantileArgument(const BoundAggregateExpression &aggregate) {
		return LptsExpressionRenderer::QuantileArgument(aggregate);
	}

	// Render a table-function parameter Value as a re-parseable, type-faithful SQL literal by routing it
	// through the constant renderer (so a typed NULL like `NULL::INT[]` becomes `CAST(NULL AS INTEGER[])`
	// rather than a bare `NULL` that changes what the function does — e.g. test_vector_types(NULL::INT[])).
	string RenderParameterValue(const Value &value) {
		unique_ptr<Expression> constant = make_uniq<BoundConstantExpression>(value);
		return ExpressionToAliasedString(constant);
	}

	static bool QuantileDesc(const BoundAggregateExpression &aggregate) {
		return LptsExpressionRenderer::QuantileDesc(aggregate);
	}

	static string ApproxQuantileArgument(const BoundAggregateExpression &aggregate) {
		return LptsExpressionRenderer::ApproxQuantileArgument(aggregate);
	}

	static string ReservoirQuantileArguments(const BoundAggregateExpression &aggregate) {
		return LptsExpressionRenderer::ReservoirQuantileArguments(aggregate);
	}

	static bool IsQuantileAggregate(const string &agg_name) {
		return LptsExpressionRenderer::IsQuantileAggregate(agg_name);
	}

	static string StripTablePrefix(const string &cte_column_name) {
		return LptsExpressionRenderer::StripTablePrefix(cte_column_name);
	}

	static string StringAggSeparator(const BoundAggregateExpression &aggregate) {
		return LptsExpressionRenderer::StringAggSeparator(aggregate);
	}

	static string GroupingSetsToClause(const vector<string> &group_names, const vector<GroupingSet> &grouping_sets) {
		return LptsExpressionRenderer::GroupingSetsToClause(group_names, grouping_sets);
	}

	void AppendJoinPredicateCondition(const unique_ptr<Expression> &predicate,
	                                  const vector<ColumnBinding> &child_bindings, vector<string> &conditions) {
		if (!predicate) {
			return;
		}
		RegisterChildBindingFallbacks(*predicate, child_bindings);
		conditions.push_back("(" + ExpressionToAliasedString(predicate) + ")");
	}

	//--------------------------------------------------------------------------
	// IsCompressedMaterializationProjection
	//
	// Returns true if the projection contains only __internal_compress_* or
	// __internal_decompress_* function calls (plus pass-through column refs).
	// These are DuckDB optimizer-internal compressed materialization nodes
	// that cannot appear in user-facing SQL.
	//--------------------------------------------------------------------------
	static bool IsCompressedMaterializationProjection(const LogicalProjection &proj) {
		if (proj.expressions.empty()) {
			return false;
		}
		bool has_internal_func = false;
		for (auto &expr : proj.expressions) {
			if (expr->type == ExpressionType::BOUND_COLUMN_REF) {
				continue; // pass-through is fine
			}
			if (expr->type == ExpressionType::BOUND_FUNCTION) {
				auto &func = expr->Cast<BoundFunctionExpression>();
				const string &name = func.function.name;
				if (name.rfind("__internal_compress_", 0) == 0 || name.rfind("__internal_decompress_", 0) == 0) {
					has_internal_func = true;
					continue;
				}
			}
			return false; // non-passthrough, non-internal expression
		}
		return has_internal_func;
	}

	//--------------------------------------------------------------------------
	// IsOrderPreservingCompressedProjection
	//
	// True iff expression i of the compressed-materialization projection (after
	// unwrapping the compress/decompress call) is a column ref to the child's
	// binding at ordinal i. Only then is the pass-through skip valid: skipping a
	// projection that reorders columns breaks the positional contract of the
	// emitted CTE (its consumers, e.g. INSERT ... SELECT *, read by position).
	//--------------------------------------------------------------------------
	static bool IsOrderPreservingCompressedProjection(const LogicalProjection &proj) {
		if (proj.children.empty()) {
			return false;
		}
		const auto child_bindings = proj.children[0]->GetColumnBindings();
		if (proj.expressions.size() != child_bindings.size()) {
			return false;
		}
		for (idx_t i = 0; i < proj.expressions.size(); i++) {
			const Expression *expr = proj.expressions[i].get();
			if (expr->type == ExpressionType::BOUND_FUNCTION) {
				auto &func = expr->Cast<BoundFunctionExpression>();
				if (func.children.empty()) {
					return false;
				}
				expr = func.children[0].get();
			}
			if (expr->type != ExpressionType::BOUND_COLUMN_REF) {
				return false;
			}
			if (expr->Cast<BoundColumnRefExpression>().binding != child_bindings[i]) {
				return false;
			}
		}
		return true;
	}

	bool TryResolveProjectionColumnRef(const LogicalProjection &proj, const unique_ptr<Expression> &expr, idx_t ordinal,
	                                   ColumnBinding &resolved) const {
		if (expr->type != ExpressionType::BOUND_COLUMN_REF || proj.children.empty()) {
			return false;
		}
		const auto child_bindings = proj.children[0]->GetColumnBindings();
		auto &bcr = expr->Cast<BoundColumnRefExpression>();
		resolved = bcr.binding;
		if (column_map.find(MappableColumnBinding(resolved)) == column_map.end()) {
			if (resolved.column_index >= child_bindings.size()) {
				return false;
			}
			resolved = child_bindings[resolved.column_index];
		}
		if (ordinal >= child_bindings.size()) {
			return false;
		}
		const auto &expected = child_bindings[ordinal];
		return resolved.table_index == expected.table_index && resolved.column_index == expected.column_index;
	}

	bool TrySkipIdentityProjection(const LogicalOperator *op, const LogicalProjection &proj) {
		if (proj.children.size() != 1) {
			return false;
		}
		auto extra_it = extra_projection_outputs.find(op);
		if (extra_it != extra_projection_outputs.end() && !extra_it->second.empty()) {
			return false;
		}
		const auto child_bindings = proj.children[0]->GetColumnBindings();
		if (proj.expressions.size() != child_bindings.size()) {
			return false;
		}

		vector<ColumnBinding> resolved_bindings;
		resolved_bindings.reserve(proj.expressions.size());
		for (idx_t i = 0; i < proj.expressions.size(); i++) {
			ColumnBinding resolved;
			if (!TryResolveProjectionColumnRef(proj, proj.expressions[i], i, resolved)) {
				return false;
			}
			resolved_bindings.push_back(resolved);
		}

		for (idx_t i = 0; i < resolved_bindings.size(); i++) {
			const auto &src = FindColumnBinding(resolved_bindings[i], "identity projection");
			column_map[MappableColumnBinding(ColumnBinding(proj.table_index, i))] =
			    make_uniq<ColStruct>(src->table_index, src->column_name, src->alias);
		}
		LPTS_DEBUG_PRINT("[LPTS-AST] Skipping identity projection (table_index=" + std::to_string(proj.table_index) +
		                 ", columns=" + std::to_string(proj.expressions.size()) + ")");
		return true;
	}

	//--------------------------------------------------------------------------
	// CollectDelimGetTableIndices
	//
	// Walk the inner subtree of a DELIM_JOIN, collecting table_index values of
	// all LOGICAL_DELIM_GET nodes. Stops at nested LOGICAL_DELIM_JOIN nodes
	// (they own their own DELIM_GETs and handle them separately).
	//--------------------------------------------------------------------------
	static void CollectDelimGetTableIndices(const LogicalOperator *subtree, vector<idx_t> &out) {
		if (!subtree) {
			return;
		}
		if (subtree->type == LogicalOperatorType::LOGICAL_DELIM_GET) {
			out.push_back(subtree->Cast<LogicalDelimGet>().table_index);
			return;
		}
		if (subtree->type == LogicalOperatorType::LOGICAL_DELIM_JOIN ||
		    subtree->type == LogicalOperatorType::LOGICAL_DEPENDENT_JOIN) {
			// A nested delim join owns DelimGets in its inner child, but its outer
			// child can still contain DelimGets that belong to the current parent
			// correlation scope.
			const auto &nested = subtree->Cast<LogicalComparisonJoin>();
			const idx_t nested_outer_idx = nested.delim_flipped ? 1 : 0;
			if (nested_outer_idx < subtree->children.size()) {
				CollectDelimGetTableIndices(subtree->children[nested_outer_idx].get(), out);
			}
			return;
		}
		for (const auto &child : subtree->children) {
			CollectDelimGetTableIndices(child.get(), out);
		}
	}

	//--------------------------------------------------------------------------
	// PreregisterDelimGetColumns
	//
	// Before recursing into the right subtree of a DELIM_JOIN, walk it looking
	// for DELIM_GET nodes. For each found, register its output columns in
	// column_map (using left-side column names from duplicate_eliminated_columns)
	// and record source column names for Phase 2 CTE generation.
	//--------------------------------------------------------------------------
	void PreregisterDelimGetColumns(const LogicalOperator *subtree, const vector<unique_ptr<Expression>> &dup_cols) {
		if (!subtree) {
			return;
		}
		if (subtree->type == LogicalOperatorType::LOGICAL_DELIM_GET) {
			const LogicalDelimGet &dg = subtree->Cast<LogicalDelimGet>();
			const idx_t dg_ti = dg.table_index;
			vector<string> source_names;
			// Two duplicate-eliminated correlation columns can share a source column name (e.g. a LATERAL
			// referencing both x.q1 and y.q1), which would give the delim-get output header two columns
			// both named t{dg_ti}_q1 — DuckDB then resolves join-condition references to the first, silently
			// collapsing the correlation key. Dedup so each delim-get output column gets a distinct name.
			CaseInsensitiveNameSet seen_delim_names;

			for (size_t i = 0; i < dg.chunk_types.size(); i++) {
				string col_name = "c" + std::to_string(i);
				string source_col_name = col_name;

				if (i < dup_cols.size() && dup_cols[i]->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
					const auto &bcr = dup_cols[i]->Cast<BoundColumnRefExpression>();
					auto it = column_map.find(MappableColumnBinding(bcr.binding));
					if (it != column_map.end()) {
						col_name = it->second->column_name;
						source_col_name = it->second->ToUniqueColumnName();
					}
				}

				source_names.push_back(source_col_name);
				column_map[MappableColumnBinding(ColumnBinding(dg_ti, i))] =
				    MakeDedupedColumn(dg_ti, col_name, "", seen_delim_names, i);
			}

			delim_get_source_col_names[dg_ti] = std::move(source_names);
			return;
		}
		if (subtree->type == LogicalOperatorType::LOGICAL_DELIM_JOIN ||
		    subtree->type == LogicalOperatorType::LOGICAL_DEPENDENT_JOIN) {
			// Only the nested outer child can contain DelimGets that belong to
			// the current parent. The nested inner child is owned by the nested
			// delim join and will be registered when that join is processed.
			const auto &nested = subtree->Cast<LogicalComparisonJoin>();
			const idx_t nested_outer_idx = nested.delim_flipped ? 1 : 0;
			if (nested_outer_idx < subtree->children.size()) {
				PreregisterDelimGetColumns(subtree->children[nested_outer_idx].get(), dup_cols);
			}
			return;
		}
		for (const auto &child : subtree->children) {
			PreregisterDelimGetColumns(child.get(), dup_cols);
		}
	}

	//--------------------------------------------------------------------------
	// BuildNode
	//
	// Creates an AstNode for the given operator. Children must already be
	// processed and attached. Returns the AstNode with column_map updated for
	// this operator's output columns so parent operators can resolve them.
	// Returns nullptr for nodes that should be skipped (e.g. compressed
	// materialization projections).
	//--------------------------------------------------------------------------
	unique_ptr<AstNode> BuildNode(unique_ptr<LogicalOperator> &op, bool is_root) {
		switch (op->type) {
		//----------------------------------------------------------------------
		case LogicalOperatorType::LOGICAL_GET: {
			const LogicalGet &get = op->Cast<LogicalGet>();
			auto catalog_entry = get.GetTable();
			const idx_t table_index = get.table_index;
			string catalog_name;
			string schema_name;
			string table_name;

			// Meta/dynamic table functions cannot be faithfully reproduced as static SQL: `query()` /
			// `query_table()` run a SQL string assembled at runtime, and `sniff_csv()` returns the result of
			// CSV auto-detection. Fail explicitly (NotImplemented) rather than emit a query that silently
			// reads/produces something different.
			{
				static const std::set<string> untranslatable_table_functions = {"query", "query_table", "sniff_csv"};
				if (untranslatable_table_functions.count(StringUtil::Lower(get.function.name))) {
					ThrowLptsNotImplemented("LPTS_UNSUPPORTED_FUNCTION", dialect, "table_function", get.function.name,
					                        "LOGICAL_GET",
					                        "meta/dynamic table function cannot be reproduced as static SQL");
				}
			}

			LPTS_DEBUG_PRINT("[LPTS-AST] GET: function.name='" + get.function.name +
			                 "' params=" + std::to_string(get.parameters.size()) +
			                 " named_params=" + std::to_string(get.named_parameters.size()) +
			                 " catalog_entry=" + string(catalog_entry ? "valid" : "null"));
			for (size_t pi = 0; pi < get.parameters.size(); pi++) {
				LPTS_DEBUG_PRINT("[LPTS-AST] GET:   param[" + std::to_string(pi) +
				                 "]=" + get.parameters[pi].ToString());
			}
			for (auto &np : get.named_parameters) {
				LPTS_DEBUG_PRINT("[LPTS-AST] GET:   named '" + np.first + "'=" + np.second.ToString());
			}
			auto params_map = get.ParamsToString();
			for (auto &ps : params_map) {
				LPTS_DEBUG_PRINT("[LPTS-AST] GET:   ParamsToString '" + ps.first + "'='" + ps.second + "'");
			}

			// Check for DuckLake snapshot-range scans (insertions/deletions) and time travel.
			bool is_ducklake_change_scan = false;
			bool is_ducklake_time_travel = false;
			idx_t ducklake_snapshot_id = 0;
			if (get.function.name == "ducklake_scan" && get.function.function_info) {
				auto &func_info = get.function.function_info->Cast<DuckLakeFunctionInfo>();
				// AT VERSION: only emit for explicit time-travel scans, NOT for
				// regular current-snapshot scans. Every ducklake_scan has a valid
				// snapshot_id (the current transaction's), but emitting AT VERSION
				// for those would pin stored queries (e.g. MV definitions) to a
				// specific snapshot instead of reading current data.
				// Detection: compare the scan's snapshot against the transaction's
				// current snapshot (same pattern as ducklake_scan.cpp:158).
				if (func_info.scan_type == DuckLakeScanType::SCAN_TABLE) {
					// Detect explicit time travel by comparing the scan's snapshot
					// against the catalog's current snapshot. Regular scans use the
					// current snapshot; AT VERSION scans use a historical one.
					auto catalog_entry = get.GetTable();
					if (catalog_entry) {
						string cat_name = catalog_entry->ParentCatalog().GetName();
						idx_t current_snap = GetDuckLakeCurrentSnapshot(cat_name);
						if (current_snap != DConstants::INVALID_INDEX &&
						    func_info.snapshot.snapshot_id != current_snap) {
							is_ducklake_time_travel = true;
							ducklake_snapshot_id = func_info.snapshot.snapshot_id;
						}
					}
				}
				if (func_info.scan_type == DuckLakeScanType::SCAN_INSERTIONS ||
				    func_info.scan_type == DuckLakeScanType::SCAN_DELETIONS) {
					is_ducklake_change_scan = true;
					string func_name = (func_info.scan_type == DuckLakeScanType::SCAN_INSERTIONS)
					                       ? "ducklake_table_insertions"
					                       : "ducklake_table_deletions";
					std::ostringstream func_str;
					func_str << func_name << "(";
					for (size_t i = 0; i < get.parameters.size(); i++) {
						if (i > 0) {
							func_str << ", ";
						}
						func_str << RenderParameterValue(get.parameters[i]);
					}
					func_str << ")";
					table_name = func_str.str();
					LPTS_DEBUG_PRINT("[LPTS-AST] GET: DuckLake change scan -> " + table_name);
				}
			}

			if (!is_ducklake_change_scan) {
				if (catalog_entry) {
					catalog_name = catalog_entry->schema.ParentCatalog().GetName();
					schema_name = catalog_entry->schema.name;
					ApplyOutputQualificationOverrides(catalog_name, schema_name);
					table_name = catalog_entry.get()->name;
					if (is_ducklake_time_travel) {
						table_name += " AT (VERSION => " + std::to_string(ducklake_snapshot_id) + ")";
					}
				} else {
					// Table function without catalog entry (e.g. range(), read_csv())
					std::ostringstream func_str;
					func_str << get.function.name << "(";
					bool first_arg = true;
					bool takes_table_argument = false;
					for (const auto &arg_type : get.function.arguments) {
						if (arg_type.id() == LogicalTypeId::TABLE) {
							takes_table_argument = true;
							break;
						}
					}
					if (takes_table_argument && !op->children.empty()) {
						// A TABLE-argument function (`summary((SELECT ...))`): the child subtree IS the
						// argument. Emit a placeholder; GetNode::ToQuery substitutes the child CTE
						// (`(SELECT * FROM <child>)`) instead of the comma-lateral form.
						func_str << "%LPTS_TABLE_ARG%";
						first_arg = false;
					} else if (!op->children.empty() && get.parameters.empty()) {
						const auto child_bindings = op->children[0]->GetColumnBindings();
						idx_t arg_count = get.function.arguments.size();
						if (arg_count > child_bindings.size()) {
							arg_count = child_bindings.size();
						}
						for (idx_t i = 0; i < arg_count; i++) {
							if (!first_arg) {
								func_str << ", ";
							}
							first_arg = false;
							func_str
							    << FindColumnBinding(child_bindings[i], "table function input")->ToUniqueColumnName();
						}
					} else {
						for (size_t i = 0; i < get.parameters.size(); i++) {
							if (!first_arg) {
								func_str << ", ";
							}
							first_arg = false;
							func_str << RenderParameterValue(get.parameters[i]);
						}
					}
					// hive_partitioning / filename add columns derived from the file *path* (partition keys,
					// the source filename). LPTS reads back specific resolved files, so it cannot reproduce
					// those path-derived columns — fail explicitly rather than emit a query that reads
					// different columns.
					for (const auto &named_param : get.named_parameters) {
						const string lower_name = StringUtil::Lower(named_param.first);
						if ((lower_name == "hive_partitioning" || lower_name == "filename") &&
						    !named_param.second.IsNull() && named_param.second.GetValue<bool>()) {
							ThrowLptsNotImplemented("LPTS_UNSUPPORTED_FUNCTION", dialect, "table_function_option",
							                        named_param.first, "LOGICAL_GET",
							                        "path-derived columns (hive partitioning / filename) cannot be "
							                        "reproduced as static SQL");
						}
					}
					// Named parameters (read_csv header=, types=, delim=, columns=, ...). Dropping these
					// changes what the function reads (e.g. without header=0 the first row is treated as a
					// header), so reproduce each as `name = <literal>` to round-trip faithfully.
					//
					// union_by_name MUST be preserved: it unifies differing per-file schemas into a superset
					// (missing columns → NULL, structs widened), so the bound column TYPES depend on it.
					// Dropping it makes read_parquet use only the first file's schema and coerce the rest,
					// changing types/values. The positional `_tf(...)` alias below is built from the bound
					// (union) output positions, so re-emitting union_by_name round-trips correctly.
					//
					// hive_partitioning must be preserved when explicitly FALSE: read_parquet auto-detects
					// hive partitioning by default, so dropping `hive_partitioning=0` lets a path like
					// `.../x=1/...` silently turn `x` into a path-derived column (different values). An
					// explicit TRUE already failed above (path-derived columns are unreproducible), so only a
					// FALSE reaches here and re-emitting it disables the auto-detection. filename / hive_types
					// are path-derived (filename=TRUE failed above; a stray FALSE / hive_types alongside
					// hive_partitioning=FALSE is inert) — keep skipping them.
					static const std::set<string> structural_table_function_params = {"filename", "hive_types",
					                                                                  "hive_types_autocast"};
					for (const auto &named_param : get.named_parameters) {
						if (structural_table_function_params.count(StringUtil::Lower(named_param.first))) {
							continue;
						}
						if (!first_arg) {
							func_str << ", ";
						}
						first_arg = false;
						func_str << named_param.first << " = " << named_param.second.ToSQLString();
					}
					func_str << ")";
					// `range(x) WITH ORDINALITY AS t(v, o)`: the ordinality column is part of the scan's
					// output (get.ordinality_idx) but not of the function's own schema — the modifier must
					// be re-emitted or the `_tf(...)` alias has one name too many.
					if (get.ordinality_idx.IsValid()) {
						func_str << " WITH ORDINALITY";
					}
					table_name = func_str.str();
				}
			}

			vector<string> column_names;
			vector<string> cte_column_names;
			vector<string> table_filters;
			// Parallel to column_names: true where the entry is a struct field-extraction expression.
			vector<bool> column_is_expr;

			const vector<ColumnBinding> col_binds = op->GetColumnBindings();
			const auto col_ids = get.GetColumnIds();
			// For an in-out (lateral) scan, only the function's OWN outputs (get.names, incl. a WITH
			// ORDINALITY column) belong in the `_tf(...)` alias — col_ids can additionally carry
			// passthrough entries for the correlated child columns.
			const idx_t table_function_output_count = (!op->children.empty() && catalog_entry == nullptr)
			                                              ? std::min<idx_t>(col_ids.size(), get.names.size())
			                                              : DConstants::INVALID_INDEX;

			LPTS_DEBUG_PRINT("[LPTS-AST] GET: col_binds=" + std::to_string(col_binds.size()) + " col_ids=" +
			                 std::to_string(col_ids.size()) + " names=" + std::to_string(get.names.size()));
			for (size_t di = 0; di < col_ids.size(); di++) {
				LPTS_DEBUG_PRINT("[LPTS-AST] GET:   col_id[" + std::to_string(di) +
				                 "] primary=" + std::to_string(col_ids[di].GetPrimaryIndex()) +
				                 " virtual=" + std::to_string(col_ids[di].IsVirtualColumn()));
			}

			idx_t table_function_passthrough_idx = 0;
			// True while every projected column is a real, named output column of the source (no virtual
			// or passthrough/rowid columns). When true we can build a correct output-ordered `_tf(...)`
			// alias for a table function. native_out_positions[i] is the function-output (file) position of
			// the i-th projected column.
			bool all_columns_native = true;
			vector<idx_t> native_out_positions;
			// Unique CTE column names already emitted by this scan, so struct field-extraction columns
			// (which alias to the leaf field name) don't collide — e.g. SELECT s.x.a, s.x.a, or two fields
			// whose leaf names match a plain column.
			CaseInsensitiveNameSet seen_cte_names;
			for (size_t i = 0; i < col_binds.size(); ++i) {
				const ColumnBinding &cb = col_binds[i];
				// An in-out (lateral) scan's PASSTHROUGH columns (`projected_input`) are the CHILD's own
				// bindings, appended verbatim after the function's outputs (LogicalGet::GetColumnBindings)
				// — recognizable by their foreign table_index. They are already registered in column_map by
				// the child; just select them under their existing name.
				if (cb.table_index != get.table_index) {
					all_columns_native = false;
					auto &src = FindColumnBinding(cb, "table function passthrough");
					const string passthrough_name = src->ToUniqueColumnName();
					column_names.push_back(passthrough_name);
					column_is_expr.push_back(false);
					cte_column_names.push_back(passthrough_name);
					table_function_passthrough_idx++;
					continue;
				}
				// The binding's column_index tells us which entry in col_ids
				// this output column corresponds to. When projection_ids is set
				// (optimizer removed unused columns), the binding index may
				// differ from the loop index.
				const idx_t col_id_idx = cb.column_index;
				if (col_id_idx >= col_ids.size()) {
					all_columns_native = false;
					string col_name = "rowid";
					string cte_col_name = "rowid";
					if (!op->children.empty() && get.parameters.empty()) {
						const auto child_bindings = op->children[0]->GetColumnBindings();
						idx_t child_col_idx = get.function.arguments.size() + table_function_passthrough_idx;
						if (child_col_idx < child_bindings.size()) {
							auto &src = FindColumnBinding(child_bindings[child_col_idx], "table function passthrough");
							col_name = src->ToUniqueColumnName();
							cte_col_name = col_name;
						}
					}
					table_function_passthrough_idx++;
					auto col_struct = make_uniq<ColStruct>(table_index, cte_col_name, "");
					column_names.push_back(col_name);
					column_is_expr.push_back(false);
					cte_column_names.push_back(col_struct->ToUniqueColumnName());
					column_map[MappableColumnBinding(cb)] = std::move(col_struct);
					continue;
				}
				string col_name;
				string col_alias; // non-empty only for struct field-extraction columns (the leaf field name)
				if (col_ids[col_id_idx].IsVirtualColumn()) {
					all_columns_native = false;
					const idx_t virtual_id = col_ids[col_id_idx].GetPrimaryIndex();
					// The multi-file reader's path/reader-derived virtual columns (filename, file_row_number,
					// file_index) are materialized from the file path / scan position. LPTS reads back specific
					// resolved files as a plain scan, so it cannot reproduce them (it would alias a real data
					// column under the virtual name and read the wrong values) — refuse rather than emit WRONG.
					if (virtual_id == MultiFileReader::COLUMN_IDENTIFIER_FILENAME ||
					    virtual_id == MultiFileReader::COLUMN_IDENTIFIER_FILE_ROW_NUMBER ||
					    virtual_id == MultiFileReader::COLUMN_IDENTIFIER_FILE_INDEX) {
						auto vit = get.virtual_columns.find(virtual_id);
						ThrowLptsNotImplemented(
						    "LPTS_UNSUPPORTED_VIRTUAL_COLUMN", dialect, "virtual_column",
						    vit != get.virtual_columns.end() ? vit->second.name : "filename", "LOGICAL_GET",
						    "path/reader-derived virtual columns (filename / file_row_number / file_index) "
						    "cannot be reproduced as static SQL");
					}
					// Virtual columns (snapshot_id, rowid, etc.) — look up in virtual_columns map
					auto vit = get.virtual_columns.find(col_ids[col_id_idx].GetPrimaryIndex());
					col_name = (vit != get.virtual_columns.end()) ? vit->second.name : "";
					if (col_name.empty()) {
						col_name = "rowid";
					}
				} else {
					const idx_t idx = col_ids[col_id_idx].GetPrimaryIndex();
					const ColumnIndex &ci = col_ids[col_id_idx];
					string extract_expr;
					string extract_leaf;
					const LogicalType &base_type = (idx < get.returned_types.size())
					                                   ? get.returned_types[idx]
					                                   : LogicalType(LogicalTypeId::INVALID);
					// Only when the extraction REPLACES the scan output (IsPushdownExtract). A ColumnIndex can
					// also carry children as struct-field PRUNING info while the scan still emits the whole
					// struct — the expressions above then re-extract, so the scan must render the base column.
					if (ci.HasChildren() && ci.IsPushdownExtract() &&
					    RenderStructExtractPath(get.names[idx], base_type, ci, extract_expr, extract_leaf)) {
						// DuckDB pushed a struct field access into the scan: emit struct_extract(...) and alias
						// to the leaf field name. Not a plain native column, so it can't feed the `_tf` alias.
						// A table function (read_parquet/read_csv/...) exposes its columns only through that
						// `_tf(...)` alias, so the base column referenced inside struct_extract would be
						// unresolved — refuse rather than emit a query that reads the wrong column.
						if (catalog_entry == nullptr) {
							ThrowLptsNotImplemented(
							    "LPTS_STRUCT_EXTRACT_PUSHDOWN", dialect, "expression", "struct field extraction",
							    "LOGICAL_GET",
							    "struct field-extraction pushdown over a table function is not implemented "
							    "(the base column is only reachable via the _tf(...) alias)");
						}
						all_columns_native = false;
						col_name = std::move(extract_expr);
						col_alias = std::move(extract_leaf);
					} else {
						col_name = get.names[idx];
						native_out_positions.push_back(idx);
					}
				}
				unique_ptr<ColStruct> col_struct;
				if (!col_alias.empty()) {
					col_struct = MakeDedupedColumn(table_index, col_name, col_alias, seen_cte_names, 1);
				} else {
					// Plain native column. Distinct source columns can still collide as CTE identifiers when
					// their names sanitize to the same fragment (e.g. different unicode column names all
					// reduce to "_____"). Keep the real name in the SELECT body (column_name) but give a
					// colliding column a distinct alias so the CTE column list stays unambiguous.
					col_struct = make_uniq<ColStruct>(table_index, col_name, "");
					if (seen_cte_names.count(col_struct->ToUniqueColumnName())) {
						idx_t suffix = i;
						do {
							col_struct =
							    make_uniq<ColStruct>(table_index, col_name, col_name + "_" + std::to_string(suffix++));
						} while (seen_cte_names.count(col_struct->ToUniqueColumnName()));
					}
					seen_cte_names.insert(col_struct->ToUniqueColumnName());
				}
				column_names.push_back(col_name);
				column_is_expr.push_back(!col_alias.empty());
				cte_column_names.push_back(col_struct->ToUniqueColumnName());
				column_map[MappableColumnBinding(cb)] = std::move(col_struct);
			}

			// COUNT(*) scans can have no projected columns. Emit a dummy column
			// only in that case. Virtual-column-only scans (e.g. rowid) must keep
			// the virtual column because parents may reference its CTE alias.
			if (column_names.empty()) {
				column_names.clear();
				cte_column_names.clear();
				column_is_expr.clear();
				column_names.push_back("1");
				column_is_expr.push_back(false);
				cte_column_names.push_back("t" + std::to_string(table_index) + "_dummy");
			}

			// Pushdown table filters (rare, but present in some plans).
			if (!get.table_filters.filters.empty()) {
				for (auto &entry : get.table_filters.filters) {
					// A filter can target a VIRTUAL column (e.g. `WHERE rowid = 0`): its key is the virtual
					// column id, not an index into get.names. Resolve via virtual_columns — `rowid` in a WHERE
					// over the same base table is faithful.
					string filter_col_name;
					if (entry.first < get.names.size()) {
						filter_col_name = get.names[entry.first];
					} else {
						auto vit = get.virtual_columns.find(entry.first);
						filter_col_name = (vit != get.virtual_columns.end()) ? vit->second.name : "rowid";
					}
					string filter_str;
					if (!TableFilterToSql(*entry.second, DialectQuoteIdent(filter_col_name, dialect), filter_str)) {
						continue;
					}
					table_filters.push_back(std::move(filter_str));
				}
			}

			// Hive-partition / complex file-pruning filters (e.g. `WHERE key='a'` on a partition column) are
			// consumed during scanning and dropped from the structured plan — the predicate survives only as
			// a display string in extra_info.file_filters, built from the filter's ToString(). Re-apply it as
			// a scan filter so the pruning is reproduced (LPTS reads all files then filters — same result set;
			// the partition column is re-exposed by auto-detected hive partitioning). Multiple pruned filters
			// are concatenated with no separator (see HivePartitioning::ApplyFiltersToFileList), so only
			// re-apply a single, well-formed parenthesized predicate — otherwise leave it (round-trip check
			// will surface the drop). NULL-value partitions render as the string 'NULL'; equality still holds.
			const string &file_filters = get.extra_info.file_filters;
			if (!file_filters.empty() && file_filters.front() == '(') {
				int depth = 0;
				size_t first_group_end = string::npos;
				for (size_t i = 0; i < file_filters.size(); i++) {
					if (file_filters[i] == '(') {
						depth++;
					} else if (file_filters[i] == ')' && --depth == 0) {
						first_group_end = i;
						break;
					}
				}
				if (first_group_end == file_filters.size() - 1) {
					table_filters.push_back(file_filters);
				}
			}

			// For a catalog-less table function whose columns are all real outputs, build the `_tf(...)`
			// alias in the function's OUTPUT order: alias[out_pos] = the projected column's name. The SELECT
			// references the names (projected order); placing each at its output position makes the
			// positional alias correct even when projection pushdown reordered or subset the columns
			// (otherwise a `WHERE` on a non-first column scrambles which name maps to which physical column).
			vector<string> table_function_alias;
			if (catalog_entry == nullptr && op->children.empty() && all_columns_native &&
			    table_name.find('(') != string::npos && native_out_positions.size() == column_names.size()) {
				// Name EVERY output position with its real bound name (an identity rename): positions and
				// names then stay correct for projected columns, filter-only columns, and columns the
				// re-executed function derives itself (e.g. auto-detected hive partition keys — a partial
				// alias with placeholders can collide with those or scramble their positions). Projected
				// positions keep the (possibly aliased) projected name.
				table_function_alias = get.names;
				for (size_t i = 0; i < column_names.size(); i++) {
					if (native_out_positions[i] < table_function_alias.size()) {
						table_function_alias[native_out_positions[i]] = column_names[i];
					}
				}
			}
			auto get_node = make_uniq<AstGetNode>(catalog_name, schema_name, table_name, table_index,
			                                      std::move(column_names), std::move(cte_column_names),
			                                      std::move(table_filters), table_function_output_count);
			get_node->table_function_alias = std::move(table_function_alias);
			// Only carry the flags when at least one column is a struct-extract expression; an all-false
			// vector is equivalent to "empty" and the renderers treat empty as all plain identifiers.
			for (auto is_expr : column_is_expr) {
				if (is_expr) {
					get_node->column_is_expression = std::move(column_is_expr);
					break;
				}
			}
			return get_node;
		}

		//----------------------------------------------------------------------
		case LogicalOperatorType::LOGICAL_FILTER: {
			const LogicalFilter &filter_op = op->Cast<LogicalFilter>();
			vector<string> conditions;
			for (const unique_ptr<Expression> &expr : filter_op.expressions) {
				conditions.emplace_back(ExpressionToAliasedString(expr));
			}
			return make_uniq<AstFilterNode>(std::move(conditions), filter_op.projection_map);
		}

		//----------------------------------------------------------------------
		case LogicalOperatorType::LOGICAL_WINDOW: {
			const LogicalWindow &window = op->Cast<LogicalWindow>();
			const idx_t table_index = window.window_index;

			LPTS_DEBUG_PRINT("[LPTS-AST] Building LOGICAL_WINDOW (table_index=" + std::to_string(table_index) + ")");

			vector<string> expressions;
			vector<string> cte_column_names;
			CaseInsensitiveNameSet seen_names;

			for (const ColumnBinding &binding : window.children[0]->GetColumnBindings()) {
				const unique_ptr<ColStruct> &src = FindColumnBinding(binding, "window child output");
				const string passthrough_name = src->ToUniqueColumnName();
				expressions.push_back(passthrough_name);
				cte_column_names.push_back(passthrough_name);
				seen_names.insert(passthrough_name);
			}

			for (idx_t i = 0; i < window.expressions.size(); i++) {
				if (window.expressions[i]->GetExpressionClass() != ExpressionClass::BOUND_WINDOW) {
					ThrowLptsNotImplemented("LPTS_UNSUPPORTED_OPERATOR", dialect, "expression_class",
					                        ExpressionTypeToString(window.expressions[i]->type), "LOGICAL_WINDOW",
					                        "LOGICAL_WINDOW only supports BOUND_WINDOW expressions");
				}
				const BoundWindowExpression &window_expr = window.expressions[i]->Cast<BoundWindowExpression>();
				const string expr_str = WindowExpressionToAliasedString(window_expr);
				expressions.push_back(expr_str);

				string alias = window.expressions[i]->HasAlias() ? window.expressions[i]->GetAlias()
				                                                 : "window_" + std::to_string(i);
				string unique_name = "t" + std::to_string(table_index) + "_" + alias;
				for (idx_t suffix = i; seen_names.count(unique_name); suffix++) {
					alias = "window_" + std::to_string(i) + "_" + std::to_string(suffix);
					unique_name = "t" + std::to_string(table_index) + "_" + alias;
				}
				seen_names.insert(unique_name);

				auto new_col = make_uniq<ColStruct>(table_index, expr_str, std::move(alias));
				cte_column_names.push_back(new_col->ToUniqueColumnName());
				column_map[MappableColumnBinding(ColumnBinding(table_index, i))] = std::move(new_col);
			}

			LPTS_DEBUG_PRINT("[LPTS-AST] Built LOGICAL_WINDOW with " + std::to_string(window.expressions.size()) +
			                 " window expressions");
			// Mark as a window projection so pipeline fusion treats it as a boundary
			// (window/OVER expressions must not be folded into WHERE/GROUP BY contexts).
			return make_uniq<AstProjectNode>(std::move(expressions), std::move(cte_column_names), table_index,
			                                 /*is_window=*/true);
		}

		//----------------------------------------------------------------------
		case LogicalOperatorType::LOGICAL_PROJECTION: {
			const LogicalProjection &proj = op->Cast<LogicalProjection>();
			const idx_t table_index = proj.table_index;

			// Skip compressed materialization projections (compress/decompress).
			// These contain __internal_compress_* or __internal_decompress_* functions
			// that are internal-only and cannot appear in user-facing SQL.
			// We remap bindings to point through to the source columns and return nullptr
			// to signal RecursiveTraversal to pass through the child node directly.
			//
			// The pass-through is only valid when the projection preserves its child's column ORDER
			// (modulo the compress/decompress wrappers). Plans rewritten after optimization (e.g.
			// OpenIVM's delta rewrite appends a multiplicity column to the projection while the
			// aggregate below emits it as a group column in the middle of its output) can make a
			// decompress projection genuinely reorder columns; skipping it then breaks the positional
			// contract of the emitted CTE (e.g. INSERT ... SELECT *). In that case fall through to the
			// regular projection path below — ExpressionToAliasedString strips the internal wrappers.
			if (IsCompressedMaterializationProjection(proj) && IsOrderPreservingCompressedProjection(proj)) {
				LPTS_DEBUG_PRINT("[LPTS-AST] Skipping compressed materialization projection (table_index=" +
				                 std::to_string(table_index) + ")");
				for (size_t i = 0; i < proj.expressions.size(); ++i) {
					const ColumnBinding new_cb(table_index, i);
					const unique_ptr<Expression> &expr = proj.expressions[i];
					if (expr->type == ExpressionType::BOUND_FUNCTION) {
						// compress/decompress: remap to child's source column,
						// preserving the source's table_index so the column name
						// matches the CTE that actually defines it.
						auto &func = expr->Cast<BoundFunctionExpression>();
						D_ASSERT(!func.children.empty());
						auto &child = func.children[0];
						if (child->type == ExpressionType::BOUND_COLUMN_REF) {
							auto &bcr = child->Cast<BoundColumnRefExpression>();
							auto &src = FindColumnBinding(bcr.binding, "compressed projection");
							column_map[MappableColumnBinding(new_cb)] =
							    make_uniq<ColStruct>(src->table_index, src->column_name, src->alias);
						}
					} else if (expr->type == ExpressionType::BOUND_COLUMN_REF) {
						// pass-through column ref: preserve source's table_index
						auto &bcr = expr->Cast<BoundColumnRefExpression>();
						auto &src = FindColumnBinding(bcr.binding, "compressed projection");
						column_map[MappableColumnBinding(new_cb)] =
						    make_uniq<ColStruct>(src->table_index, src->column_name, src->alias);
					}
				}
				return nullptr;
			}

			if (!is_root && TrySkipIdentityProjection(op.get(), proj)) {
				return nullptr;
			}

			vector<string> expressions;
			vector<string> cte_column_names;
			CaseInsensitiveNameSet seen_names;

			for (size_t i = 0; i < proj.expressions.size(); ++i) {
				const unique_ptr<Expression> &expr = proj.expressions[i];
				const ColumnBinding new_cb = ColumnBinding(table_index, i);

				if (expr->type == ExpressionType::BOUND_COLUMN_REF) {
					BoundColumnRefExpression &bcr = expr->Cast<BoundColumnRefExpression>();
					ColumnBinding lookup_binding = bcr.binding;
					if (column_map.find(MappableColumnBinding(lookup_binding)) == column_map.end() &&
					    !proj.children.empty()) {
						auto child_bindings = proj.children[0]->GetColumnBindings();
						if (lookup_binding.column_index < child_bindings.size()) {
							lookup_binding = child_bindings[lookup_binding.column_index];
						}
					}
					const unique_ptr<ColStruct> &desc = FindColumnBinding(lookup_binding, "projection");
					const string src_name = desc->ToUniqueColumnName();
					expressions.push_back(src_name);
					// DuckDB auto-populates a column ref's alias with either the source column name or the
					// original expression rendering (which contains '('). Treat the alias as a genuine user
					// rename only when it is paren-free AND differs from the column's own name; such a rename
					// gets a fresh t{index}_alias, otherwise reuse the source column's identity unchanged.
					const string origin_base = SanitizeIdentifierFragment(StripTablePrefix(src_name));
					unique_ptr<ColStruct> new_col;
					if (expr->HasAlias() && expr->GetAlias().find('(') == string::npos &&
					    SanitizeIdentifierFragment(expr->GetAlias()) != origin_base) {
						new_col = MakeDedupedColumn(table_index, expr->GetAlias(), "", seen_names, i);
					} else if (!seen_names.count(src_name)) {
						seen_names.insert(src_name);
						new_col = make_uniq<ColStruct>(desc->table_index, desc->column_name, desc->alias);
					} else {
						// Same source column projected twice (e.g. SELECT a, a): mint a fresh prefixed name.
						new_col = MakeDedupedColumn(table_index, origin_base, "", seen_names, i);
					}
					cte_column_names.push_back(new_col->ToUniqueColumnName());
					column_map[MappableColumnBinding(new_cb)] = std::move(new_col);
				} else {
					if (!proj.children.empty()) {
						RegisterChildBindingFallbacks(*expr, proj.children[0]->GetColumnBindings());
					}
					string expr_str = ExpressionToAliasedString(expr);
					expressions.emplace_back(expr_str);
					// A real user alias (paren-free; DuckDB auto-aliases with the rendered expression,
					// which contains '(') gets t{index}_alias; otherwise an invented t{index}_scalar_Y.
					const string base_alias = (expr->HasAlias() && expr->GetAlias().find('(') == string::npos)
					                              ? expr->GetAlias()
					                              : "scalar_" + std::to_string(i);
					string scalar_alias = base_alias;
					string unique_name = "t" + to_string(table_index) + "_" + SanitizeIdentifierFragment(scalar_alias);
					for (size_t suffix = i; seen_names.count(unique_name); suffix++) {
						scalar_alias = base_alias + "_" + to_string(suffix);
						unique_name = "t" + to_string(table_index) + "_" + SanitizeIdentifierFragment(scalar_alias);
					}
					seen_names.insert(unique_name);
					auto new_col = make_uniq<ColStruct>(table_index, expr_str, std::move(scalar_alias));
					cte_column_names.push_back(new_col->ToUniqueColumnName());
					column_map[MappableColumnBinding(new_cb)] = std::move(new_col);
				}
			}
			auto extra_it = extra_projection_outputs.find(op.get());
			const idx_t visible_column_count = cte_column_names.size();
			if (extra_it != extra_projection_outputs.end()) {
				for (const auto &binding : extra_it->second) {
					const unique_ptr<ColStruct> &src = FindColumnBinding(binding, "projection extra output");
					const string passthrough_name = src->ToUniqueColumnName();
					if (seen_names.count(passthrough_name)) {
						continue;
					}
					seen_names.insert(passthrough_name);
					expressions.push_back(passthrough_name);
					cte_column_names.push_back(passthrough_name);
				}
			}

			return make_uniq<AstProjectNode>(std::move(expressions), std::move(cte_column_names), table_index,
			                                 /*is_window=*/false, visible_column_count);
		}

		//----------------------------------------------------------------------
		case LogicalOperatorType::LOGICAL_UNNEST: {
			const LogicalUnnest &unnest = op->Cast<LogicalUnnest>();
			const idx_t table_index = unnest.unnest_index;

			vector<string> expressions;
			vector<string> cte_column_names;

			// Pass the child's columns through unconditionally: a recursive-UNNEST chain stacks one
			// LOGICAL_UNNEST per level (each its own CTE — see the fusion boundary below), and the next
			// level's passthrough references THIS level's passthrough columns — including the dummy-scan
			// column for an UNNEST over a constant.
			{
				auto child_bindings = unnest.children[0]->GetColumnBindings();
				for (auto &binding : child_bindings) {
					auto &src = FindColumnBinding(binding, "unnest");
					expressions.push_back(src->ToUniqueColumnName());
					cte_column_names.push_back(src->ToUniqueColumnName());
				}
			}

			for (idx_t i = 0; i < unnest.expressions.size(); i++) {
				string expr_str = ExpressionToAliasedString(unnest.expressions[i]);
				expressions.push_back(expr_str);
				auto col_struct = make_uniq<ColStruct>(table_index, "unnest_" + std::to_string(i), "");
				cte_column_names.push_back(col_struct->ToUniqueColumnName());
				column_map[MappableColumnBinding(ColumnBinding(table_index, i))] = std::move(col_struct);
			}

			// Mark as a fusion boundary (like window projections): recursive UNNEST plans stack one
			// LOGICAL_UNNEST per level, and fusing two levels into one SELECT would substitute one
			// UNNEST(...) inside another — "Nested UNNEST calls are not supported".
			return make_uniq<AstProjectNode>(std::move(expressions), std::move(cte_column_names), table_index,
			                                 /*is_window=*/true);
		}

		//----------------------------------------------------------------------
		case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY: {
			const LogicalAggregate &agg = op->Cast<LogicalAggregate>();
			const idx_t group_table_index = agg.group_index;
			const idx_t agg_table_index = agg.aggregate_index;
			vector<string> group_names;
			vector<string> agg_expressions;
			vector<string> cte_column_names;
			CaseInsensitiveNameSet seen_names;

			// GROUP BY columns
			for (size_t i = 0; i < agg.groups.size(); ++i) {
				const unique_ptr<Expression> &g = agg.groups[i];
				if (!op->children.empty()) {
					RegisterChildBindingFallbacks(*g, op->children[0]->GetColumnBindings());
				}
				// Grouping by a bare constant is a decorrelation artifact (e.g. lateral GROUPING SETS over a
				// correlated `SELECT 1, 2 ...`, where the projected constants become group keys). It renders
				// as `GROUP BY 2`, which SQL re-reads as an ordinal (2nd select column) — a different grouping.
				// Refuse rather than emit that.
				if (g->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
					ThrowLptsNotImplemented("LPTS_UNSUPPORTED_GROUP_BY", dialect, "group_by", "constant",
					                        "LOGICAL_AGGREGATE_AND_GROUP_BY",
					                        "grouping by a constant (a correlated/lateral grouping-sets "
					                        "decorrelation artifact) cannot be reproduced as static SQL");
				}
				if (g->type == ExpressionType::BOUND_COLUMN_REF) {
					BoundColumnRefExpression &bcr = g->Cast<BoundColumnRefExpression>();
					auto it = column_map.find(MappableColumnBinding(bcr.binding));
					if (it == column_map.end()) {
						throw NotImplementedException("LPTS_UNSUPPORTED_GROUP_BY: column ref (%llu,%llu) is not "
						                              "implemented because it is not in column_map",
						                              (unsigned long long)bcr.binding.table_index,
						                              (unsigned long long)bcr.binding.column_index);
					}
					const unique_ptr<ColStruct> &desc = it->second;
					// A group key whose underlying column is an inlined constant (rendered as a bare integer,
					// e.g. a lateral grouping-sets artifact) would, after CTE de-prefixing, appear as a bare
					// integer in GROUP BY and be re-read as an ordinal. Refuse.
					if (agg.grouping_sets.size() > 1 && !desc->column_name.empty() &&
					    desc->column_name.find_first_not_of("0123456789") == string::npos) {
						ThrowLptsNotImplemented("LPTS_UNSUPPORTED_GROUP_BY", dialect, "grouping_sets",
						                        desc->column_name, "LOGICAL_AGGREGATE_AND_GROUP_BY",
						                        "a grouping-set key over an inlined constant would be misread as "
						                        "an ordinal position and cannot be reproduced");
					}
					group_names.push_back(desc->ToUniqueColumnName());
					auto new_col = MakeDedupedColumn(group_table_index, desc->column_name, desc->alias, seen_names, i);
					cte_column_names.push_back(new_col->ToUniqueColumnName());
					column_map[MappableColumnBinding(ColumnBinding(group_table_index, i))] = std::move(new_col);
				} else {
					// Non-column GROUP BY expression (COALESCE, CASE, function, etc.)
					string expr_str = ExpressionToAliasedString(g);
					string alias = g->HasAlias() ? g->GetAlias() : ("grp_" + std::to_string(i));
					group_names.push_back(expr_str);
					auto new_col = MakeDedupedColumn(group_table_index, expr_str, std::move(alias), seen_names, i);
					cte_column_names.push_back(new_col->ToUniqueColumnName());
					column_map[MappableColumnBinding(ColumnBinding(group_table_index, i))] = std::move(new_col);
				}
			}

			// Aggregate expressions
			for (size_t i = 0; i < agg.expressions.size(); ++i) {
				const unique_ptr<Expression> &expr = agg.expressions[i];
				if (expr->type != ExpressionType::BOUND_AGGREGATE) {
					ThrowLptsNotImplemented("LPTS_UNSUPPORTED_OPERATOR", dialect, "expression_class",
					                        ExpressionTypeToString(expr->type), "LOGICAL_AGGREGATE_AND_GROUP_BY",
					                        "aggregate operator only supports BOUND_AGGREGATE expressions");
				}
				const BoundAggregateExpression &ba = expr->Cast<BoundAggregateExpression>();
				std::ostringstream agg_str;
				// Replace internal aggregate variants with their user-facing equivalents.
				// sum_no_overflow is used by compressed materialization but is internal-only.
				string agg_name = ba.function.name;
				if (agg_name == "sum_no_overflow") {
					agg_name = "sum";
				}
				// `SUM(x) EXPORT_STATE` binds to an internal "aggregate_state_export_sum" — the internal name
				// is not callable; re-emit the base aggregate with the EXPORT_STATE modifier.
				static const string kExportStatePrefix = "aggregate_state_export_";
				const bool is_export_state = agg_name.rfind(kExportStatePrefix, 0) == 0;
				if (is_export_state) {
					agg_name = agg_name.substr(kExportStatePrefix.size());
					// Parenthesized so the modifier survives any surrounding context (function argument
					// lists, casts): `combine(sum(d) EXPORT_STATE, ...)` does not parse, `(sum(d)
					// EXPORT_STATE)` does.
					agg_str << "(";
				}
				agg_str << agg_name << "(";
				if (ba.IsDistinct()) {
					agg_str << "DISTINCT ";
				}
				vector<string> child_exprs;
				for (const unique_ptr<Expression> &c : ba.children) {
					if (!op->children.empty()) {
						RegisterChildBindingFallbacks(*c, op->children[0]->GetColumnBindings());
					}
					child_exprs.push_back(ExpressionToAliasedString(c));
				}
				// Join child expressions with commas
				for (size_t ci = 0; ci < child_exprs.size(); ++ci) {
					if (ci > 0)
						agg_str << ", ";
					agg_str << child_exprs[ci];
				}
				if (agg_name == "string_agg") {
					string separator = StringAggSeparator(ba);
					if (!separator.empty()) {
						if (!child_exprs.empty()) {
							agg_str << ", ";
						}
						agg_str << "'" << EscapeSingleQuotes(separator) << "'";
					}
				} else if (IsQuantileAggregate(agg_name) && child_exprs.size() == 1 && ba.bind_info) {
					agg_str << ", " << QuantileArgument(ba);
					// A descending quantile (`WITHIN GROUP (ORDER BY x DESC)`, or a negative quantile which
					// DuckDB normalizes to abs(p) + desc) must keep its direction, else the bare
					// quantile_cont(x, p) computes from the wrong end. DuckDB accepts the in-aggregate
					// `ORDER BY` form: quantile_cont(x, p ORDER BY x DESC).
					if (QuantileDesc(ba)) {
						agg_str << " ORDER BY " << child_exprs[0] << " DESC";
					}
				} else if (agg_name == "approx_quantile" && child_exprs.size() == 1 && ba.bind_info) {
					agg_str << ", " << ApproxQuantileArgument(ba);
				} else if (agg_name == "reservoir_quantile" && child_exprs.size() == 1 && ba.bind_info) {
					agg_str << ", " << ReservoirQuantileArguments(ba);
				}
				// Preserve intra-aggregate ORDER BY — matters for LIST, STRING_AGG,
				// and other order-sensitive aggregates. Drop it only for aggregates
				// whose result is order-independent (sum/count/min/max/avg), since
				// rendering ORDER BY for those would change the plan for no reason.
				if (ba.order_bys && !ba.order_bys->orders.empty() && agg_name != "sum" && agg_name != "count" &&
				    agg_name != "count_star" && agg_name != "min" && agg_name != "max" && agg_name != "avg" &&
				    !IsQuantileAggregate(agg_name)) {
					agg_str << " ORDER BY ";
					for (size_t oi = 0; oi < ba.order_bys->orders.size(); ++oi) {
						const BoundOrderByNode &ob = ba.order_bys->orders[oi];
						if (!op->children.empty()) {
							RegisterChildBindingFallbacks(*ob.expression, op->children[0]->GetColumnBindings());
						}
						if (oi > 0)
							agg_str << ", ";
						agg_str << ExpressionToAliasedString(ob.expression);
						if (ob.type == OrderType::DESCENDING) {
							agg_str << " DESC";
						} else if (ob.type == OrderType::ASCENDING) {
							agg_str << " ASC";
						}
						if (ob.null_order == OrderByNullType::NULLS_FIRST) {
							agg_str << " NULLS FIRST";
						} else if (ob.null_order == OrderByNullType::NULLS_LAST) {
							agg_str << " NULLS LAST";
						}
					}
				}
				agg_str << ")";
				if (is_export_state) {
					agg_str << " EXPORT_STATE)";
				}
				// Preserve FILTER (WHERE predicate) clause. Without this, a view query
				// with `COUNT(*) FILTER (WHERE x > 0)` round-trips to `count_star()`,
				// silently producing a total row count instead of a conditional count.
				if (ba.filter) {
					if (!op->children.empty()) {
						RegisterChildBindingFallbacks(*ba.filter, op->children[0]->GetColumnBindings());
					}
					agg_str << " FILTER (WHERE " << ExpressionToAliasedString(ba.filter) << ")";
				}
				agg_expressions.push_back(agg_str.str());
				// Name (always t{index}_ prefixed for global uniqueness): a real user alias wins;
				// otherwise "{func}_{input col}" — e.g. count_star, count_distinct_l_quantity, sum_a
				// (just the func name for complex/multi args). DuckDB auto-populates the alias with the
				// function rendering (which contains '('), so treat only a paren-free alias as user-set.
				string agg_fragment;
				if (expr->HasAlias() && expr->GetAlias().find('(') == string::npos) {
					agg_fragment = expr->GetAlias();
				} else {
					agg_fragment = StringUtil::Lower(agg_name);
					if (ba.IsDistinct()) {
						agg_fragment += "_distinct";
					}
					if (ba.children.size() == 1 && ba.children[0]->type == ExpressionType::BOUND_COLUMN_REF &&
					    !child_exprs.empty()) {
						agg_fragment += "_" + LptsExpressionRenderer::StripTablePrefix(child_exprs[0]);
					}
				}
				auto new_col = MakeDedupedColumn(agg_table_index, agg_fragment, "", seen_names, i);
				cte_column_names.push_back(new_col->ToUniqueColumnName());
				column_map[MappableColumnBinding(ColumnBinding(agg_table_index, i))] = std::move(new_col);
			}

			for (size_t i = 0; i < agg.grouping_functions.size(); i++) {
				vector<string> grouping_args;
				for (idx_t group_idx : agg.grouping_functions[i]) {
					if (group_idx >= group_names.size()) {
						throw NotImplementedException("LPTS_UNSUPPORTED_GROUPING: argument index %llu is out of range",
						                              (unsigned long long)group_idx);
					}
					grouping_args.push_back(group_names[group_idx]);
				}
				string grouping_expr = "GROUPING(" + VecToSeparatedList(grouping_args) + ")";
				string grouping_alias = "grouping_" + std::to_string(i);
				agg_expressions.push_back(grouping_expr);
				auto new_col =
				    MakeDedupedColumn(agg.groupings_index, grouping_expr, std::move(grouping_alias), seen_names, i);
				cte_column_names.push_back(new_col->ToUniqueColumnName());
				column_map[MappableColumnBinding(ColumnBinding(agg.groupings_index, i))] = std::move(new_col);
			}

			// Remap source bindings → aggregate group output bindings.
			// Parent ORDER BY / FILTER / MARK JOIN nodes may still reference the pre-aggregate
			// bindings (e.g. from a replaced DISTINCT that used to pass through its child's
			// bindings). Without this, ExpressionToAliasedString returns the pre-aggregate
			// CTE alias (e.g. t8_col) which no longer exists downstream of the aggregate CTE.
			// Must run AFTER group_names and agg_expressions are populated so the aggregate's
			// own GROUP BY/SELECT still uses the child's column names (e.g. SUM(t0_val) when
			// val is also a GROUP BY column).
			for (size_t i = 0; i < agg.groups.size(); ++i) {
				const unique_ptr<Expression> &g = agg.groups[i];
				if (g->type != ExpressionType::BOUND_COLUMN_REF) {
					continue;
				}
				BoundColumnRefExpression &bcr = g->Cast<BoundColumnRefExpression>();
				ColumnBinding new_cb(group_table_index, i);
				if (bcr.binding == new_cb) {
					continue;
				}
				const auto &new_col = FindColumnBinding(new_cb, "aggregate remap");
				column_map[MappableColumnBinding(bcr.binding)] =
				    make_uniq<ColStruct>(new_col->table_index, new_col->column_name, new_col->alias);
			}

			// Duplicate grouping columns + multiple grouping sets (CUBE/ROLLUP/GROUPING SETS) are not
			// faithfully reproducible. DuckDB's duplicate-group optimizer collapses equal group keys (e.g.
			// `col3` = `col1` from a join condition) to the same expression, so two grouping *dimensions*
			// render to the same column name. SQL grouping sets reference columns by name, so the two
			// collapsed dimensions become indistinguishable — a set that includes one but not the other can
			// no longer be expressed, and the super-aggregate NULLs land on the wrong column. Refuse rather
			// than emit a plausible-but-wrong result. (A single grouping set — plain GROUP BY — is fine:
			// every dimension is always present, so a duplicate column just groups redundantly.)
			if (agg.grouping_sets.size() > 1) {
				std::set<string> seen_group_names;
				for (const string &gn : group_names) {
					if (!seen_group_names.insert(gn).second) {
						ThrowLptsNotImplemented(
						    "LPTS_DUPLICATE_GROUPING_SET_COLUMN", dialect, "grouping_sets", gn,
						    "LOGICAL_AGGREGATE_AND_GROUP_BY",
						    "duplicate grouping columns across multiple grouping sets (from the duplicate-group "
						    "optimizer collapsing equal join keys) cannot be reproduced as static SQL");
					}
					// A group key that renders as a bare integer (e.g. a correlated/lateral grouping-sets
					// artifact where a projected constant became a group key) is re-read by SQL as an ordinal
					// (`GROUP BY 2` = 2nd select column), a different grouping. Refuse.
					if (!gn.empty() && gn.find_first_not_of("0123456789") == string::npos) {
						ThrowLptsNotImplemented("LPTS_UNSUPPORTED_GROUP_BY", dialect, "grouping_sets", gn,
						                        "LOGICAL_AGGREGATE_AND_GROUP_BY",
						                        "a grouping-set key that renders as a bare integer would be "
						                        "misread as an ordinal position and cannot be reproduced");
					}
				}
			}

			string group_by_clause = GroupingSetsToClause(group_names, agg.grouping_sets);
			return make_uniq<AstAggregateNode>(std::move(group_names), std::move(group_by_clause),
			                                   std::move(agg_expressions), std::move(cte_column_names));
		}

		//----------------------------------------------------------------------
		case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
		case LogicalOperatorType::LOGICAL_ASOF_JOIN: {
			const LogicalComparisonJoin &join_op = op->Cast<LogicalComparisonJoin>();
			const bool is_asof = op->type == LogicalOperatorType::LOGICAL_ASOF_JOIN;
			ValidateJoinTypeForDialect(join_op.join_type, dialect,
			                           is_asof ? "LOGICAL_ASOF_JOIN" : "LOGICAL_COMPARISON_JOIN");
			if (is_asof && dialect != SqlDialect::DUCKDB) {
				ThrowLptsNotImplemented("LPTS_UNSUPPORTED_OPERATOR", dialect, "logical_operator",
				                        LogicalOperatorToString(op->type), "AstBuilder",
				                        "ASOF JOIN SQL syntax is DuckDB-specific");
			}
			vector<string> conditions;
			vector<ColumnBinding> child_bindings = ChildBindings(*op);
			for (const auto &cond : join_op.conditions) {
				RegisterChildBindingFallbacks(*cond.left, child_bindings);
				RegisterChildBindingFallbacks(*cond.right, child_bindings);
				string lhs = ExpressionToAliasedString(cond.left);
				string rhs = ExpressionToAliasedString(cond.right);
				conditions.push_back(RenderJoinComparison(lhs, rhs, cond.comparison));
			}
			AppendJoinPredicateCondition(join_op.predicate, child_bindings, conditions);

			// MARK joins produce an extra boolean column indicating match existence.
			// In SQL: LEFT JOIN + (right_key IS NOT NULL) as a computed column.
			// We register it with a clean alias so parent CTEs can reference it.
			// The right side is wrapped in SELECT DISTINCT at SQL generation time
			// to prevent left-row duplication when the RHS has repeated values.
			if (join_op.join_type == JoinType::MARK) {
				LPTS_DEBUG_PRINT("[LPTS-AST] MARK join detected: mark_index=" + std::to_string(join_op.mark_index) +
				                 " conditions=" + std::to_string(join_op.conditions.size()));
				ColumnBinding mark_cb(join_op.mark_index, 0);
				string mark_expr;
				if (!join_op.conditions.empty()) {
					mark_expr = "(" + ExpressionToAliasedString(join_op.conditions[0].right) + " IS NOT NULL)";
				} else {
					mark_expr = "true";
				}
				LPTS_DEBUG_PRINT("[LPTS-AST] MARK join: registering mark column as '" + mark_expr + "'");
				auto mark_col = make_uniq<ColStruct>(join_op.mark_index, mark_expr, "_mark");
				column_map[MappableColumnBinding(mark_cb)] = std::move(mark_col);
			}

			vector<string> cte_column_names = OutputColumnNames(*op, "join output");

			// Convert MARK join to LEFT join for SQL output
			JoinType sql_join_type = join_op.join_type;
			string mark_expr;
			if (sql_join_type == JoinType::MARK) {
				sql_join_type = JoinType::LEFT;
				// Extract the mark expression we registered in column_map
				ColumnBinding mark_cb(join_op.mark_index, 0);
				auto it = column_map.find(MappableColumnBinding(mark_cb));
				if (it != column_map.end()) {
					mark_expr = it->second->column_name; // contains the IS NOT NULL expression
				}
				LPTS_DEBUG_PRINT("[LPTS-AST] MARK join: converting to LEFT join, mark_expr='" + mark_expr + "'");
			}
			auto join_node = make_uniq<AstJoinNode>(sql_join_type, std::move(conditions), std::move(cte_column_names),
			                                        std::move(mark_expr), is_asof);
			// IN/ANY/ALL mark joins need a 3-valued mark (NULL when the membership comparison is
			// indeterminate). A mark join's conditions split into null-safe correlation keys
			// (`IS NOT DISTINCT FROM`, the decorrelated outer↔subquery link) and the NULL-propagating
			// membership comparison (`=`, `<`, ...). When there is exactly one comparison condition, capture
			// its key expressions plus the rendered correlation conditions so the renderer can build the
			// 3-valued mark: indeterminate iff a correlated row exists where a comparison operand is NULL.
			// (EXISTS subqueries are 2-valued and have no NULL-propagating comparison → keys left empty.)
			if (join_op.join_type == JoinType::MARK) {
				ExtractMarkComparison(join_op.conditions, join_node->mark_lhs_key, join_node->mark_rhs_key,
				                      join_node->mark_correlation_conditions, join_node->mark_membership_conditions,
				                      join_node->mark_membership_comparisons, join_node->mark_membership_lhs,
				                      join_node->mark_membership_rhs, join_node->mark_join_has_equality);
			}
			return join_node;
		}

		//----------------------------------------------------------------------
		// LOGICAL_ANY_JOIN: join with an arbitrary expression condition (single
		// expression, not a list of comparisons). Produced when the optimizer
		// can't decompose the ON clause into conjunction of comparisons —
		// e.g. `ON a.x * 2 > b.y`, or CROSS JOIN + WHERE combined into a join.
		// Serialize as a normal JOIN with the condition as an opaque predicate.
		case LogicalOperatorType::LOGICAL_ANY_JOIN: {
			const LogicalAnyJoin &any_join = op->Cast<LogicalAnyJoin>();
			ValidateJoinTypeForDialect(any_join.join_type, dialect, "LOGICAL_ANY_JOIN");
			vector<string> conditions;
			if (any_join.condition) {
				conditions.push_back("(" + ExpressionToAliasedString(any_join.condition) + ")");
			} else {
				conditions.push_back("(TRUE)");
			}
			vector<string> cte_column_names = OutputColumnNames(*op, "any join output");
			return make_uniq<AstJoinNode>(any_join.join_type, std::move(conditions), std::move(cte_column_names));
		}

		case LogicalOperatorType::LOGICAL_CROSS_PRODUCT: {
			vector<string> cross_condition = {"(TRUE)"};
			vector<string> cte_column_names = OutputColumnNames(*op, "cross product output");
			return make_uniq<AstJoinNode>(JoinType::INNER, std::move(cross_condition), std::move(cte_column_names));
		}

		//----------------------------------------------------------------------
		case LogicalOperatorType::LOGICAL_POSITIONAL_JOIN: {
			if (dialect != SqlDialect::DUCKDB) {
				ThrowLptsNotImplemented("LPTS_UNSUPPORTED_OPERATOR", dialect, "logical_operator",
				                        LogicalOperatorToString(op->type), "AstBuilder",
				                        "POSITIONAL JOIN SQL syntax is DuckDB-specific");
			}
			vector<string> cte_column_names = OutputColumnNames(*op, "positional join output");
			return make_uniq<AstPositionalJoinNode>(std::move(cte_column_names));
		}

		//----------------------------------------------------------------------
		case LogicalOperatorType::LOGICAL_SAMPLE: {
			if (dialect != SqlDialect::DUCKDB) {
				ThrowLptsNotImplemented("LPTS_UNSUPPORTED_OPERATOR", dialect, "logical_operator",
				                        LogicalOperatorToString(op->type), "AstBuilder",
				                        "USING SAMPLE SQL syntax is DuckDB-specific");
			}
			const LogicalSample &sample = op->Cast<LogicalSample>();
			if (!sample.sample_options) {
				throw InternalException("LPTS SAMPLE: missing sample options");
			}
			vector<string> cte_column_names = OutputColumnNames(*op, "sample output");
			return make_uniq<AstSampleNode>(SampleOptionsToSql(*sample.sample_options), std::move(cte_column_names));
		}

		//----------------------------------------------------------------------
		case LogicalOperatorType::LOGICAL_UNION: {
			const LogicalSetOperation &set_op = op->Cast<LogicalSetOperation>();
			const idx_t table_index = set_op.table_index;
			vector<string> cte_column_names;
			const auto &lhs_bindings = op->children[0]->GetColumnBindings();
			const auto &union_bindings = op->GetColumnBindings();
			// Two union output columns can derive from source columns with the same name (e.g.
			// SELECT t1.a, t2.a ... UNION ...), which would emit a header like (t7_a, t7_a) — DuckDB
			// resolves later references to the first, silently dropping the second column. Dedup so each
			// output column gets a distinct generated name.
			CaseInsensitiveNameSet seen_names;
			for (size_t i = 0; i < lhs_bindings.size(); ++i) {
				const unique_ptr<ColStruct> &lhs_col = FindColumnBinding(lhs_bindings[i], "union lhs");
				auto new_col = MakeDedupedColumn(table_index, lhs_col->column_name, lhs_col->alias, seen_names, 1);
				cte_column_names.push_back(new_col->ToUniqueColumnName());
				column_map[MappableColumnBinding(union_bindings[i])] = std::move(new_col);
			}
			return make_uniq<AstUnionNode>(set_op.setop_all, std::move(cte_column_names));
		}

		case LogicalOperatorType::LOGICAL_EXCEPT:
		case LogicalOperatorType::LOGICAL_INTERSECT: {
			const LogicalSetOperation &set_op = op->Cast<LogicalSetOperation>();
			const idx_t table_index = set_op.table_index;
			vector<string> cte_column_names;
			const auto &lhs_bindings = op->children[0]->GetColumnBindings();
			const auto &setop_bindings = op->GetColumnBindings();
			if (lhs_bindings.size() < setop_bindings.size()) {
				throw InternalException(
				    "LPTS set operation: left child exposes fewer columns than set operation output");
			}
			// Dedup output column names (see LOGICAL_UNION): a header like (t6_c0, t6_c0) makes the second
			// column unreferenceable, so EXCEPT/INTERSECT would silently drop it.
			CaseInsensitiveNameSet seen_names;
			for (size_t i = 0; i < setop_bindings.size(); ++i) {
				const unique_ptr<ColStruct> &lhs_col = FindColumnBinding(lhs_bindings[i], "setop lhs");
				auto new_col = MakeDedupedColumn(table_index, lhs_col->column_name, lhs_col->alias, seen_names, 1);
				cte_column_names.push_back(new_col->ToUniqueColumnName());
				column_map[MappableColumnBinding(setop_bindings[i])] = std::move(new_col);
			}
			string op_name = op->type == LogicalOperatorType::LOGICAL_EXCEPT ? "EXCEPT" : "INTERSECT";
			return make_uniq<AstSetOperationNode>(std::move(op_name), set_op.setop_all, std::move(cte_column_names));
		}

		//----------------------------------------------------------------------
		case LogicalOperatorType::LOGICAL_ORDER_BY: {
			const LogicalOrder &order_op = op->Cast<LogicalOrder>();
			vector<string> order_items;
			for (const BoundOrderByNode &order : order_op.orders) {
				order_items.push_back(OrderByToAliasedString(order));
			}
			vector<string> cte_column_names = OutputColumnNames(*op, "order by output");
			return make_uniq<AstOrderNode>(std::move(order_items), std::move(cte_column_names),
			                               order_op.projection_map);
		}

		//----------------------------------------------------------------------
		case LogicalOperatorType::LOGICAL_LIMIT: {
			const LogicalLimit &limit_op = op->Cast<LogicalLimit>();
			string limit_str;
			bool limit_needs_child_scalar = false;
			if (limit_op.limit_val.Type() == LimitNodeType::CONSTANT_VALUE) {
				limit_str = std::to_string(limit_op.limit_val.GetConstantValue());
			} else if (limit_op.limit_val.Type() == LimitNodeType::EXPRESSION_VALUE) {
				auto &limit_expr = const_cast<BoundLimitNode &>(limit_op.limit_val).GetExpression();
				limit_str = ExpressionToAliasedString(limit_expr);
				limit_needs_child_scalar = ExpressionContainsColumnRef(*limit_expr);
			} else if (limit_op.limit_val.Type() != LimitNodeType::UNSET) {
				ThrowLptsNotImplemented("LPTS_UNSUPPORTED_OPERATOR", dialect, "limit_node_type",
				                        std::to_string(static_cast<int>(limit_op.limit_val.Type())), "LOGICAL_LIMIT",
				                        "LIMIT node type is not implemented by LPTS");
			}
			string offset_str;
			bool offset_needs_child_scalar = false;
			if (limit_op.offset_val.Type() == LimitNodeType::CONSTANT_VALUE) {
				offset_str = std::to_string(limit_op.offset_val.GetConstantValue());
			} else if (limit_op.offset_val.Type() == LimitNodeType::EXPRESSION_VALUE) {
				auto &offset_expr = const_cast<BoundLimitNode &>(limit_op.offset_val).GetExpression();
				offset_str = ExpressionToAliasedString(offset_expr);
				offset_needs_child_scalar = ExpressionContainsColumnRef(*offset_expr);
			} else if (limit_op.offset_val.Type() != LimitNodeType::UNSET) {
				ThrowLptsNotImplemented("LPTS_UNSUPPORTED_OPERATOR", dialect, "offset_node_type",
				                        std::to_string(static_cast<int>(limit_op.offset_val.Type())), "LOGICAL_LIMIT",
				                        "OFFSET node type is not implemented by LPTS");
			}
			vector<string> cte_column_names = OutputColumnNames(*op, "limit output");
			return make_uniq<AstLimitNode>(std::move(limit_str), std::move(offset_str), limit_needs_child_scalar,
			                               offset_needs_child_scalar, std::move(cte_column_names));
		}

		//----------------------------------------------------------------------
		case LogicalOperatorType::LOGICAL_TOP_N: {
			const LogicalTopN &topn_op = op->Cast<LogicalTopN>();
			vector<string> order_items;
			for (const BoundOrderByNode &order : topn_op.orders) {
				order_items.push_back(OrderByToAliasedString(order));
			}
			vector<string> cte_column_names = OutputColumnNames(*op, "top_n output");
			return make_uniq<AstTopNNode>(std::move(order_items), topn_op.limit, topn_op.offset,
			                              std::move(cte_column_names));
		}

		//----------------------------------------------------------------------
		case LogicalOperatorType::LOGICAL_DISTINCT: {
			// LogicalDistinct passes bindings unchanged from child.
			const LogicalDistinct &distinct_op = op->Cast<LogicalDistinct>();
			vector<string> cte_column_names = OutputColumnNames(*op, "distinct output");
			const idx_t output_column_count = cte_column_names.size();
			auto distinct_node = make_uniq<AstDistinctNode>(std::move(cte_column_names));
			// When the dedup key (distinct_targets) is a PROPER SUBSET of the output columns, plain
			// SELECT DISTINCT would dedup on too many columns. This covers `DISTINCT ON (targets)` and
			// `SELECT DISTINCT a ... ORDER BY b` (DuckDB dedups on `a` but carries `b` for the ORDER BY).
			// Render it as a row_number() filter partitioned by the targets. When targets are empty or
			// cover all output columns, a plain SELECT DISTINCT is correct (and more readable).
			if (!distinct_op.distinct_targets.empty() && distinct_op.distinct_targets.size() < output_column_count) {
				distinct_node->is_distinct_on = true;
				for (const unique_ptr<Expression> &target : distinct_op.distinct_targets) {
					distinct_node->distinct_on_targets.push_back(ExpressionToAliasedString(target));
				}
				if (distinct_op.order_by) {
					for (const BoundOrderByNode &order : distinct_op.order_by->orders) {
						distinct_node->distinct_on_orders.push_back(OrderByToAliasedString(order));
					}
				}
			}
			return distinct_node;
		}

		//----------------------------------------------------------------------
		case LogicalOperatorType::LOGICAL_INSERT: {
			const LogicalInsert &insert_op = op->Cast<LogicalInsert>();
			return make_uniq<AstInsertNode>(insert_op.table.name, insert_op.on_conflict_info.action_type);
		}

		//----------------------------------------------------------------------
		case LogicalOperatorType::LOGICAL_DUMMY_SCAN: {
			// DUMMY_SCAN is DuckDB's single-row input for scalar constant
			// expressions. Serialize it as a one-row subquery with a dummy column;
			// parent projections will replace the dummy with their constants.
			const LogicalDummyScan &dummy = op->Cast<LogicalDummyScan>();
			const idx_t table_index = dummy.table_index;
			vector<string> column_names = {"1"};
			vector<string> cte_column_names = {"t" + std::to_string(table_index) + "_dummy"};
			// The registered name must match the CTE header ("t{ti}_dummy"), or any operator that passes the
			// dummy binding through (e.g. an uncorrelated mark join's output list) renders "t{ti}_1" — a
			// column the CTE never defines.
			column_map[MappableColumnBinding(ColumnBinding(table_index, 0))] =
			    make_uniq<ColStruct>(table_index, "dummy", "");
			return make_uniq<AstGetNode>("", "", "(SELECT 1)", table_index, std::move(column_names),
			                             std::move(cte_column_names), vector<string>());
		}

		//----------------------------------------------------------------------
		case LogicalOperatorType::LOGICAL_EMPTY_RESULT: {
			// The optimizer replaced a subtree with an empty result (e.g. LIMIT 0).
			// Generate a scan-like node that returns zero rows by adding WHERE false.
			const LogicalEmptyResult &empty = op->Cast<LogicalEmptyResult>();
			vector<string> column_names;
			vector<string> cte_column_names;
			for (size_t i = 0; i < empty.bindings.size(); i++) {
				string col_name = "c" + std::to_string(i);
				auto col_struct = make_uniq<ColStruct>(empty.bindings[i].table_index, col_name, "");
				cte_column_names.push_back(col_struct->ToUniqueColumnName());
				column_names.push_back(RenderNullLiteral(empty.return_types[i], dialect));
				column_map[MappableColumnBinding(empty.bindings[i])] = std::move(col_struct);
			}
			// Emit a real zero-row subquery. Using a fake table name here leaks
			// through when the empty result is nested inside a larger plan.
			vector<string> filters = {"false"};
			return make_uniq<AstGetNode>("", "", "(SELECT 1)", 0, std::move(column_names), std::move(cte_column_names),
			                             std::move(filters));
		}

		//----------------------------------------------------------------------
		case LogicalOperatorType::LOGICAL_CHUNK_GET: {
			// CHUNK_GET holds a materialized ColumnDataCollection of constant scalar values.
			// Created by the optimizer for IN lists with ≥6 constant elements.
			// We emit the values as a (VALUES ...) subquery so GetNode::ToQuery() renders it
			// as: SELECT c0 FROM (VALUES (v1), (v2), ...) _tf(c0)
			const LogicalColumnDataGet &chunk_get = op->Cast<LogicalColumnDataGet>();
			const idx_t table_index = chunk_get.table_index;
			const auto &types = chunk_get.chunk_types;

			LPTS_DEBUG_PRINT("[LPTS-AST] CHUNK_GET: table_index=" + std::to_string(table_index) + " types=" +
			                 std::to_string(types.size()) + " rows=" + std::to_string(chunk_get.collection->Count()));

			// Synthetic column names c0, c1, … for the VALUES columns.
			vector<string> column_names;
			vector<string> cte_column_names;
			for (size_t i = 0; i < types.size(); ++i) {
				string col_name = "c" + std::to_string(i);
				auto col_struct = make_uniq<ColStruct>(table_index, col_name, "");
				cte_column_names.push_back(col_struct->ToUniqueColumnName());
				column_names.push_back(col_name);
				column_map[MappableColumnBinding(ColumnBinding(table_index, i))] = std::move(col_struct);
			}

			// Materialize VALUES from the collection.
			auto rows = chunk_get.collection->GetRows();
			std::ostringstream values_str;
			values_str << "(VALUES ";
			for (idx_t row_idx = 0; row_idx < rows.size(); ++row_idx) {
				if (row_idx > 0) {
					values_str << ", ";
				}
				values_str << "(";
				for (idx_t col_idx = 0; col_idx < types.size(); ++col_idx) {
					if (col_idx > 0) {
						values_str << ", ";
					}
					values_str << rows.GetValue(col_idx, row_idx).ToSQLString();
				}
				values_str << ")";
			}
			values_str << ")";

			LPTS_DEBUG_PRINT("[LPTS-AST] CHUNK_GET: emitting VALUES: " + values_str.str());

			// GetNode::ToQuery detects '(' in table_name and appends _tf(col_names) alias.
			return make_uniq<AstGetNode>("", "", values_str.str(), table_index, std::move(column_names),
			                             std::move(cte_column_names), vector<string>());
		}

		//----------------------------------------------------------------------
		case LogicalOperatorType::LOGICAL_EXPRESSION_GET: {
			// EXPRESSION_GET backs VALUES clauses. Emit it as a VALUES table function
			// so constant relation joins can round-trip through SQL.
			const LogicalExpressionGet &expr_get = op->Cast<LogicalExpressionGet>();
			const idx_t table_index = expr_get.table_index;

			vector<string> column_names;
			vector<string> cte_column_names;
			for (size_t i = 0; i < expr_get.expr_types.size(); ++i) {
				string col_name = "c" + std::to_string(i);
				auto col_struct = make_uniq<ColStruct>(table_index, col_name, "");
				cte_column_names.push_back(col_struct->ToUniqueColumnName());
				column_names.push_back(col_name);
				column_map[MappableColumnBinding(ColumnBinding(table_index, i))] = std::move(col_struct);
			}

			std::ostringstream values_str;
			values_str << "(VALUES ";
			for (idx_t row_idx = 0; row_idx < expr_get.expressions.size(); row_idx++) {
				if (row_idx > 0) {
					values_str << ", ";
				}
				values_str << "(";
				for (idx_t col_idx = 0; col_idx < expr_get.expressions[row_idx].size(); col_idx++) {
					if (col_idx > 0) {
						values_str << ", ";
					}
					values_str << ExpressionToAliasedString(expr_get.expressions[row_idx][col_idx]);
				}
				values_str << ")";
			}
			values_str << ")";

			return make_uniq<AstGetNode>("", "", values_str.str(), table_index, std::move(column_names),
			                             std::move(cte_column_names), vector<string>());
		}

		//----------------------------------------------------------------------
		case LogicalOperatorType::LOGICAL_CTE_REF: {
			// CTE_REF is a scan of a materialized CTE body (used when a WITH clause CTE
			// is referenced more than once). The actual body CTE name is resolved in Phase 2.
			const LogicalCTERef &cte_ref = op->Cast<LogicalCTERef>();
			// A reference to the *recurring* table of a USING KEY recursive CTE (e.g. `recurring.fail`) reads
			// the iteration's latest-row-per-key snapshot — LPTS has no CTE it can point that at. Refuse.
			if (cte_ref.is_recurring) {
				ThrowLptsNotImplemented("LPTS_UNSUPPORTED_RECURSIVE_CTE", dialect, "recursive_cte",
				                        "recurring table reference", "LOGICAL_CTE_REF",
				                        "a reference to a USING KEY recursive CTE's recurring table cannot be "
				                        "reproduced as static SQL");
			}
			const idx_t table_index = cte_ref.table_index;
			const idx_t cte_index = cte_ref.cte_index;

			LPTS_DEBUG_PRINT("[LPTS-AST] CTE_REF: table_index=" + std::to_string(table_index) + " cte_index=" +
			                 std::to_string(cte_index) + " columns=" + std::to_string(cte_ref.bound_columns.size()));

			// Register output columns using the materialized body's actual output names when available.
			// DuckDB can prune the body to the columns referenced by the outer query, while bound_columns
			// still lists the original CTE aliases. Parent bindings are reindexed to the pruned body order.
			// Dedup either way: a CTE can expose the same column name twice (e.g. a recursive CTE whose
			// anchor projects one source column into two outputs) — distinct outputs need distinct names.
			vector<string> cte_column_names;
			CaseInsensitiveNameSet seen_ref_names;
			auto body_cols_it = materialized_cte_body_column_names.find(cte_index);
			if (body_cols_it != materialized_cte_body_column_names.end()) {
				for (idx_t i = 0; i < body_cols_it->second.size(); i++) {
					string col_name = StripTablePrefix(body_cols_it->second[i]);
					auto col_struct = MakeDedupedColumn(table_index, std::move(col_name), "", seen_ref_names, i);
					cte_column_names.push_back(col_struct->ToUniqueColumnName());
					column_map[MappableColumnBinding(ColumnBinding(table_index, i))] = std::move(col_struct);
				}
				return make_uniq<AstCteRefNode>(cte_index, std::move(cte_column_names));
			}

			for (idx_t i = 0; i < cte_ref.bound_columns.size(); ++i) {
				const string &col_name = cte_ref.bound_columns[i];
				auto col_struct = MakeDedupedColumn(table_index, col_name, "", seen_ref_names, i);
				cte_column_names.push_back(col_struct->ToUniqueColumnName());
				column_map[MappableColumnBinding(ColumnBinding(table_index, i))] = std::move(col_struct);
			}

			return make_uniq<AstCteRefNode>(cte_index, std::move(cte_column_names));
		}

		//----------------------------------------------------------------------
		case LogicalOperatorType::LOGICAL_MATERIALIZED_CTE: {
			// MATERIALIZED_CTE wraps a CTE body (children[0]) and outer query (children[1]).
			// AstMaterializedCteNode preserves the ordering so Phase 2 can flatten the body
			// first and make the body CTE name available for CteRef resolution.
			const LogicalMaterializedCTE &mat_cte = op->Cast<LogicalMaterializedCTE>();
			LPTS_DEBUG_PRINT("[LPTS-AST] MATERIALIZED_CTE: table_index=" + std::to_string(mat_cte.table_index) +
			                 " ctename='" + mat_cte.ctename + "'");
			return make_uniq<AstMaterializedCteNode>(mat_cte.table_index);
		}

		//----------------------------------------------------------------------
		// LOGICAL_DELIM_GET: a duplicate-eliminated scan driven by a parent DELIM_JOIN.
		// Columns are pre-registered in column_map by PreregisterDelimGetColumns before
		// the right subtree is traversed. We just collect them here and build the node.
		case LogicalOperatorType::LOGICAL_DELIM_GET: {
			const LogicalDelimGet &dg = op->Cast<LogicalDelimGet>();
			const idx_t table_index = dg.table_index;

			LPTS_DEBUG_PRINT("[LPTS-AST] DELIM_GET: table_index=" + std::to_string(table_index) +
			                 " cols=" + std::to_string(dg.chunk_types.size()));

			vector<string> cte_column_names;
			for (size_t i = 0; i < dg.chunk_types.size(); i++) {
				auto it = column_map.find(MappableColumnBinding(ColumnBinding(table_index, i)));
				if (it != column_map.end()) {
					cte_column_names.push_back(it->second->ToUniqueColumnName());
				} else {
					throw NotImplementedException(
					    "LPTS_UNSUPPORTED_DELIM_GET: column (%llu,%llu) is not implemented because "
					    "it was not pre-registered by its parent DELIM_JOIN",
					    (unsigned long long)table_index, (unsigned long long)i);
				}
			}

			vector<string> source_col_names;
			auto src_it = delim_get_source_col_names.find(table_index);
			if (src_it != delim_get_source_col_names.end()) {
				source_col_names = src_it->second;
			} else {
				throw NotImplementedException(
				    "LPTS_UNSUPPORTED_DELIM_GET: table_index %llu is not implemented because its "
				    "source columns were not registered by its parent DELIM_JOIN",
				    (unsigned long long)table_index);
			}

			return make_uniq<AstDelimGetNode>(table_index, std::move(cte_column_names), std::move(source_col_names));
		}

		//----------------------------------------------------------------------
		// LOGICAL_DELIM_JOIN: a duplicate-eliminating join used to decorrelate subqueries.
		// children[0] = outer (left), children[1] = inner (right, contains DELIM_GET).
		// The DELIM_GET in the right subtree has already been pre-registered in
		// PreregisterDelimGetColumns before right child traversal (in RecursiveTraversal).
		case LogicalOperatorType::LOGICAL_DELIM_JOIN:
		case LogicalOperatorType::LOGICAL_DEPENDENT_JOIN: {
			const LogicalComparisonJoin &dj = op->Cast<LogicalComparisonJoin>();
			ValidateJoinTypeForDialect(dj.join_type, dialect, LogicalOperatorToString(op->type));
			const idx_t inner_child_idx = dj.delim_flipped ? 0 : 1;

			// Collect ALL DELIM_GET table_indices from the inner subtree.
			// There can be more than one when the Deliminator keeps multiple inner joins.
			vector<idx_t> delim_tis;
			CollectDelimGetTableIndices(op->children[inner_child_idx].get(), delim_tis);

			LPTS_DEBUG_PRINT("[LPTS-AST] DELIM_JOIN: join_type=" + EnumUtil::ToString(dj.join_type) +
			                 " conditions=" + std::to_string(dj.conditions.size()) +
			                 " dup_elim_cols=" + std::to_string(dj.duplicate_eliminated_columns.size()) +
			                 " delim_flipped=" + std::to_string(dj.delim_flipped) + " delim_get_count=" +
			                 std::to_string(delim_tis.size()) + " mark_index=" + std::to_string(dj.mark_index));

			// For MARK-type DELIM_JOIN (correlated EXISTS), register the mark boolean column
			// in column_map before building conditions, so any condition expression that
			// references mark_index (or parent FILTERs checking the mark) can resolve it.
			// Use "true" as the mark expression — the right CTE already contains only
			// matching rows (via SELECT DISTINCT from the outer CTE), so any left row
			// that appears in the RIGHT CTE is a match, and IS NOT NULL holds.
			// For a decorrelated IN/ANY/ALL mark, capture the membership comparison's operands and the
			// null-safe correlation keys so the JoinNode renderer can emit a 3-valued mark (NULL when the
			// comparison is indeterminate). Empty for a 2-valued (EXISTS) mark.
			string mark_lhs_key;
			string mark_rhs_key;
			vector<string> mark_correlation_conditions;
			vector<string> mark_membership_conditions;
			vector<string> mark_membership_comparisons;
			vector<string> mark_membership_lhs;
			vector<string> mark_membership_rhs;
			bool mark_join_has_equality = false;
			if (dj.join_type == JoinType::MARK) {
				ExtractMarkComparison(dj.conditions, mark_lhs_key, mark_rhs_key, mark_correlation_conditions,
				                      mark_membership_conditions, mark_membership_comparisons, mark_membership_lhs,
				                      mark_membership_rhs, mark_join_has_equality);
			}

			if (dj.join_type == JoinType::MARK) {
				ColumnBinding mark_cb(dj.mark_index, 0);
				string mark_expr = "true";
				if (!dj.conditions.empty()) {
					// Try to build the RHS expression for the IS NOT NULL check.
					// Print the binding before attempting the lookup so we can diagnose failures.
					if (dj.conditions[0].right->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
						auto &bcr = dj.conditions[0].right->Cast<BoundColumnRefExpression>();
						LPTS_DEBUG_PRINT("[LPTS-AST] DELIM_JOIN MARK: cond[0].right binding=(" +
						                 std::to_string(bcr.binding.table_index) + "," +
						                 std::to_string(bcr.binding.column_index) + ")");
						auto it = column_map.find(MappableColumnBinding(bcr.binding));
						if (it != column_map.end()) {
							mark_expr = "(" + it->second->ToUniqueColumnName() + " IS NOT NULL)";
						} else {
							LPTS_DEBUG_PRINT("[LPTS-AST] DELIM_JOIN MARK: cond[0].right binding NOT in column_map");
						}
					}
					if (dj.conditions[0].left->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
						auto &bcr = dj.conditions[0].left->Cast<BoundColumnRefExpression>();
						LPTS_DEBUG_PRINT("[LPTS-AST] DELIM_JOIN MARK: cond[0].left binding=(" +
						                 std::to_string(bcr.binding.table_index) + "," +
						                 std::to_string(bcr.binding.column_index) + ")");
						auto it2 = column_map.find(MappableColumnBinding(bcr.binding));
						if (it2 == column_map.end()) {
							LPTS_DEBUG_PRINT("[LPTS-AST] DELIM_JOIN MARK: cond[0].left binding NOT in column_map");
						}
					}
				}
				LPTS_DEBUG_PRINT("[LPTS-AST] DELIM_JOIN MARK: registering mark column mark_index=" +
				                 std::to_string(dj.mark_index) + " expr='" + mark_expr + "'");
				column_map[MappableColumnBinding(mark_cb)] = make_uniq<ColStruct>(dj.mark_index, mark_expr, "_mark");
			}

			vector<string> conditions;
			vector<ColumnBinding> child_bindings = ChildBindings(*op);
			for (const auto &cond : dj.conditions) {
				RegisterChildBindingFallbacks(*cond.left, child_bindings);
				RegisterChildBindingFallbacks(*cond.right, child_bindings);
				if (cond.left->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
					auto &bcr = cond.left->Cast<BoundColumnRefExpression>();
					LPTS_DEBUG_PRINT(
					    "[LPTS-AST] DELIM_JOIN cond.left binding=(" + std::to_string(bcr.binding.table_index) + "," +
					    std::to_string(bcr.binding.column_index) +
					    ") in_map=" + std::to_string(column_map.count(MappableColumnBinding(bcr.binding))));
				}
				if (cond.right->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
					auto &bcr = cond.right->Cast<BoundColumnRefExpression>();
					LPTS_DEBUG_PRINT(
					    "[LPTS-AST] DELIM_JOIN cond.right binding=(" + std::to_string(bcr.binding.table_index) + "," +
					    std::to_string(bcr.binding.column_index) +
					    ") in_map=" + std::to_string(column_map.count(MappableColumnBinding(bcr.binding))));
				}
				string lhs = ExpressionToAliasedString(cond.left);
				string rhs = ExpressionToAliasedString(cond.right);
				conditions.push_back(RenderJoinComparison(lhs, rhs, cond.comparison));
			}
			AppendJoinPredicateCondition(dj.predicate, child_bindings, conditions);
			if (op->type == LogicalOperatorType::LOGICAL_DEPENDENT_JOIN) {
				const LogicalDependentJoin &dependent_join = op->Cast<LogicalDependentJoin>();
				AppendJoinPredicateCondition(dependent_join.join_condition, child_bindings, conditions);
				for (const auto &expr : dependent_join.arbitrary_expressions) {
					AppendJoinPredicateCondition(expr, child_bindings, conditions);
				}
			}

			// Normalize join type for SQL output. RecursiveTraversal already ensures outer
			// is children[0] (left) and inner is children[1] (right) regardless of
			// delim_flipped, so RIGHT_SEMI (outer=right) becomes SEMI (outer=left).
			string mark_col_expr;
			JoinType sql_join_type = dj.join_type;
			if (sql_join_type == JoinType::MARK) {
				sql_join_type = JoinType::LEFT;
				ColumnBinding mark_cb(dj.mark_index, 0);
				auto it = column_map.find(MappableColumnBinding(mark_cb));
				if (it != column_map.end()) {
					mark_col_expr = it->second->column_name; // the IS NOT NULL expression
				}
			} else if (sql_join_type == JoinType::SINGLE) {
				// Keep SINGLE (do not flatten to LEFT here): a scalar subquery must yield at most one row
				// per outer row, but the decorrelated RHS can carry duplicate rows per correlation key
				// (a non-aggregated subquery over a multi-row table). JoinNode renders SINGLE as a LEFT join
				// but deduplicates the RHS, so the outer row is not multiplied.
			} else if (sql_join_type == JoinType::RIGHT_SEMI) {
				// delim_flipped=1: outer was physical-right, now normalized to SQL-left.
				sql_join_type = JoinType::SEMI;
			} else if (sql_join_type == JoinType::RIGHT_ANTI) {
				// delim_flipped=1: outer was physical-right, now normalized to SQL-left.
				sql_join_type = JoinType::ANTI;
			}

			vector<string> cte_column_names;
			for (const ColumnBinding &cb : op->GetColumnBindings()) {
				auto it = column_map.find(MappableColumnBinding(cb));
				if (it != column_map.end()) {
					cte_column_names.push_back(it->second->ToUniqueColumnName());
				}
			}

			auto delim_join_node =
			    make_uniq<AstDelimJoinNode>(sql_join_type, std::move(conditions), std::move(cte_column_names),
			                                std::move(delim_tis), std::move(mark_col_expr));
			delim_join_node->mark_lhs_key = std::move(mark_lhs_key);
			delim_join_node->mark_rhs_key = std::move(mark_rhs_key);
			delim_join_node->mark_correlation_conditions = std::move(mark_correlation_conditions);
			delim_join_node->mark_membership_conditions = std::move(mark_membership_conditions);
			delim_join_node->mark_membership_comparisons = std::move(mark_membership_comparisons);
			delim_join_node->mark_membership_lhs = std::move(mark_membership_lhs);
			delim_join_node->mark_membership_rhs = std::move(mark_membership_rhs);
			delim_join_node->mark_join_has_equality = mark_join_has_equality;
			return delim_join_node;
		}

		case LogicalOperatorType::LOGICAL_PIVOT:
		case LogicalOperatorType::LOGICAL_JOIN:
		case LogicalOperatorType::LOGICAL_DELETE:
		case LogicalOperatorType::LOGICAL_UPDATE:
		case LogicalOperatorType::LOGICAL_MERGE_INTO:
			ThrowLptsNotImplemented("LPTS_UNSUPPORTED_OPERATOR", dialect, "logical_operator",
			                        LogicalOperatorToString(op->type), "AstBuilder",
			                        "logical operator is not implemented by LPTS");
		default:
			ThrowLptsNotImplemented("LPTS_UNSUPPORTED_OPERATOR", dialect, "logical_operator",
			                        LogicalOperatorToString(op->type), "AstBuilder",
			                        "logical operator is not implemented by LPTS");
		}
	}

	//--------------------------------------------------------------------------
	// RecursiveTraversal
	//
	// Bottom-up: process all children first, attach them to the current node,
	// then create the current node.
	//--------------------------------------------------------------------------
	unique_ptr<AstNode> RecursiveTraversal(unique_ptr<LogicalOperator> &op, bool is_root = false) {
		// 1. Recurse into children first (post-order).
		vector<unique_ptr<AstNode>> child_nodes;
		if ((op->type == LogicalOperatorType::LOGICAL_UNION || op->type == LogicalOperatorType::LOGICAL_EXCEPT ||
		     op->type == LogicalOperatorType::LOGICAL_INTERSECT) &&
		    op->children.size() >= 2) {
			// Set operations: scope column_map to prevent sibling children from overwriting
			// each other's entries when subtrees share table indices.
			child_nodes.push_back(RecursiveTraversal(op->children[0]));
			// Save column_map after first child; restore before each subsequent child
			std::map<MappableColumnBinding, unique_ptr<ColStruct>> saved_map;
			for (auto &entry : column_map) {
				saved_map[entry.first] =
				    make_uniq<ColStruct>(entry.second->table_index, entry.second->column_name, entry.second->alias);
			}
			for (size_t ci = 1; ci < op->children.size(); ci++) {
				child_nodes.push_back(RecursiveTraversal(op->children[ci]));
			}
			column_map = std::move(saved_map);
		} else if (op->type == LogicalOperatorType::LOGICAL_DELIM_JOIN ||
		           op->type == LogicalOperatorType::LOGICAL_DEPENDENT_JOIN) {
			// DELIM_JOIN: process outer child first (builds column_map for outer cols),
			// then pre-register DELIM_GET columns, then process inner child.
			// Always store: children[0] = outer, children[1] = inner — regardless of
			// delim_flipped. Phase 2 FlattenNode("DelimJoin") relies on this order.
			const LogicalComparisonJoin &dj = op->Cast<LogicalComparisonJoin>();
			const idx_t outer_idx = dj.delim_flipped ? 1 : 0;
			const idx_t inner_idx = dj.delim_flipped ? 0 : 1;

			child_nodes.resize(2);
			child_nodes[0] = RecursiveTraversal(op->children[outer_idx]);
			PreregisterDelimGetColumns(op->children[inner_idx].get(), dj.duplicate_eliminated_columns);
			child_nodes[1] = RecursiveTraversal(op->children[inner_idx]);
		} else if (op->type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE) {
			const LogicalMaterializedCTE &mat_cte = op->Cast<LogicalMaterializedCTE>();
			child_nodes.resize(2);
			child_nodes[0] = RecursiveTraversal(op->children[0]);
			materialized_cte_body_column_names[mat_cte.table_index] = child_nodes[0]->OutputColumnNames();
			child_nodes[1] = RecursiveTraversal(op->children[1]);
		} else if (op->type == LogicalOperatorType::LOGICAL_RECURSIVE_CTE) {
			const LogicalRecursiveCTE &rec_cte = op->Cast<LogicalRecursiveCTE>();
			// `WITH RECURSIVE cte(...) USING KEY (...)` keeps only the latest row per key across iterations —
			// distinct from the plain UNION [ALL] recursion LPTS emits, so the row set diverges. Refuse.
			if (!rec_cte.key_targets.empty()) {
				ThrowLptsNotImplemented("LPTS_UNSUPPORTED_RECURSIVE_CTE", dialect, "recursive_cte", "USING KEY",
				                        "LOGICAL_RECURSIVE_CTE",
				                        "recursive CTE USING KEY (latest-row-per-key semantics) cannot be "
				                        "reproduced as a plain recursive CTE");
			}
			LPTS_DEBUG_PRINT("[LPTS-AST] RECURSIVE_CTE: table_index=" + std::to_string(rec_cte.table_index) +
			                 " ctename='" + rec_cte.ctename + "' union_all=" + std::to_string(rec_cte.union_all));
			child_nodes.resize(2);

			// Traverse anchor first to learn the actual output column names.
			child_nodes[0] = RecursiveTraversal(op->children[0]);
			const auto anchor_cols = child_nodes[0]->OutputColumnNames();

			// Register the recursive CTE's output columns in column_map so parent nodes
			// (Projections, Filters above the RecursiveCTE) can reference them.
			// Also pre-register them in materialized_cte_body_column_names so that
			// self-referencing LogicalCTERef nodes in the recursive step resolve correctly.
			vector<string> output_col_names;
			// Dedup: two anchor outputs can strip to the same bare name (e.g. an anchor projecting one
			// source column twice) — the recursive CTE header must not declare duplicate columns.
			CaseInsensitiveNameSet seen_rec_names;
			for (idx_t i = 0; i < anchor_cols.size(); i++) {
				string col_name = StripTablePrefix(anchor_cols[i]);
				auto col_struct = MakeDedupedColumn(rec_cte.table_index, col_name, "", seen_rec_names, i);
				output_col_names.push_back(col_struct->ToUniqueColumnName());
				column_map[MappableColumnBinding(ColumnBinding(rec_cte.table_index, i))] = std::move(col_struct);
			}
			materialized_cte_body_column_names[rec_cte.table_index] = anchor_cols;

			// Traverse recursive step; self-referencing CteRef nodes will use the registered names.
			child_nodes[1] = RecursiveTraversal(op->children[1]);

			// Build the AstRecursiveCteNode directly (bypass the generic BuildNode path).
			auto rec_node = make_uniq<AstRecursiveCteNode>(rec_cte.table_index, rec_cte.ctename, rec_cte.union_all,
			                                               std::move(output_col_names));
			rec_node->children.push_back(std::move(child_nodes[0]));
			rec_node->children.push_back(std::move(child_nodes[1]));
			return std::move(rec_node);
		} else {
			for (auto &child : op->children) {
				child_nodes.push_back(RecursiveTraversal(child, false));
			}
		}
		// Preserve child bindings that this aggregate references before BuildNode remaps
		// group bindings to the aggregate output columns.
		EnsureAggregateChildReferencedBindings(*op, child_nodes);
		EnsureProjectionChildReferencedBindings(*op, child_nodes);
		// 2. Build this node (column_map is now populated by children).
		unique_ptr<AstNode> node = BuildNode(op, is_root);
		// A nullptr return means this node should be skipped (e.g. compressed
		// materialization projection). Pass through the single child directly.
		if (!node) {
			D_ASSERT(child_nodes.size() == 1);
			return std::move(child_nodes[0]);
		}
		if (node->NodeType() == "Join") {
			auto &join_node = static_cast<AstJoinNode &>(*node);
			join_node.select_expressions =
			    BuildJoinSelectExpressions(*op, child_nodes, join_node.cte_column_names.size());
		}
		// 3. Attach children to preserve the tree structure.
		for (auto &c : child_nodes) {
			node->children.push_back(std::move(c));
		}
		return node;
	}

public:
	AstBuilder(ClientContext &_context, SqlDialect _dialect = SqlDialect::DUCKDB)
	    : dialect(_dialect), context(_context),
	      expression_renderer(_dialect, [this](const ColumnBinding &binding, const char *context) {
		      return FindColumnBinding(binding, context)->ToUniqueColumnName();
	      }) {
		output_catalog = ReadOutputQualification(_context, "lpts_output_catalog");
		output_schema = ReadOutputQualification(_context, "lpts_output_schema");
		Value unqualified;
		if (_context.TryGetCurrentSetting("lpts_output_unqualified", unqualified) && !unqualified.IsNull()) {
			output_unqualified = unqualified.GetValue<bool>();
		}
	}

	/// Read one of the output-qualification overrides. Absent or empty means
	/// "keep the catalog/schema the plan was built against".
	static string ReadOutputQualification(ClientContext &context, const char *setting_name) {
		Value setting;
		if (context.TryGetCurrentSetting(setting_name, setting) && !setting.IsNull()) {
			return setting.GetValue<string>();
		}
		return string();
	}

	/// Entry point: walk the plan and return the AST root.
	unique_ptr<AstNode> Build(unique_ptr<LogicalOperator> &plan) {
		MarkAggregateReferencedBindings(plan.get());
		MarkProjectionReferencedBindings(plan.get());
		return RecursiveTraversal(plan, true);
	}
};

//==============================================================================
// Phase 1 entry point
//==============================================================================
unique_ptr<AstNode> LogicalPlanToAst(ClientContext &context, unique_ptr<LogicalOperator> &plan, SqlDialect dialect) {
	AstBuilder builder(context, dialect);
	return builder.Build(plan);
}

} // namespace duckdb
