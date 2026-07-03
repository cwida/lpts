#include "lpts_pipeline.hpp"
#include "lpts_helpers.hpp"
#include "lpts_debug.hpp"

namespace duckdb {

//==============================================================================
// AstFlattener — Phase 2 helper
//
// Walks the AST in post-order and produces a flat CteList that is identical
// as CTE node objects that can be serialized to SQL.
//
// Each AstNode type maps to a specific CteNode type. CTE names are assigned
// using a monotonically increasing counter, in the form t{N}_{operator}
// (t0_scan, t1_filter, t2_projection, t3_block, ...).
//==============================================================================
class AstFlattener {
private:
	size_t node_count = 0;
	vector<unique_ptr<CteNode>> cte_nodes;
	SqlDialect dialect;             // Controls dialect-specific SQL rendering.
	bool emit_spark_hints;          // Emits optimizer hints for Spark when the plan shape is known.
	bool merge_pipeline = true;     // Fuse single-child pipeline chains into one flat SELECT.
	bool has_recursive_cte = false; // True when a RecursiveCteNode has been pushed.

	/// Maps LogicalMaterializedCTE::table_index → (lpts_cte_name, lpts_cte_column_list)
	/// of the last CTE generated for the body. Populated when flattening AstMaterializedCteNode;
	/// consumed when flattening AstCteRefNode.
	/// For RecursiveCteNode: stores {recursive_cte_name, stripped_col_names} so that
	/// self-referencing CteRef nodes inside the recursive step resolve to the recursive CTE.
	unordered_map<idx_t, pair<string, vector<string>>> cte_index_to_body_info;

	/// Maps AstDelimGetNode::table_index → name of the outer left CTE to SELECT DISTINCT from.
	/// Populated when flattening AstDelimJoinNode (before the right subtree); consumed by AstDelimGetNode.
	unordered_map<idx_t, string> delim_get_source_cte;

	static string InlineJoinKeyword(JoinType join_type, const string &context) {
		switch (join_type) {
		case JoinType::INNER:
			return "INNER";
		case JoinType::LEFT:
			return "LEFT";
		case JoinType::RIGHT:
			return "RIGHT";
		case JoinType::OUTER:
			return "FULL OUTER";
		case JoinType::SEMI:
			return "SEMI";
		case JoinType::ANTI:
			return "ANTI";
		case JoinType::SINGLE:
			return "LEFT";
		case JoinType::RIGHT_SEMI:
		case JoinType::RIGHT_ANTI:
			return "";
		default:
			throw NotImplementedException(
			    "LPTS_UNSUPPORTED_RECURSIVE_STEP: %s type %s not supported in recursive CTE step", context,
			    EnumUtil::ToString(join_type));
		}
	}

	static bool IsOpenIvmDeltaTable(const string &table_name) {
		return table_name.rfind("openivm_delta_", 0) == 0;
	}

	static string InlineJoinSql(JoinType join_type, vector<string> select_cols, const string &left_sql,
	                            const string &right_sql, const vector<string> &conditions,
	                            const string &mark_expression, const string &context) {
		if (!mark_expression.empty() && !select_cols.empty()) {
			select_cols.back() = mark_expression + " AS " + select_cols.back();
		}
		string sql;
		if (join_type == JoinType::RIGHT_SEMI || join_type == JoinType::RIGHT_ANTI) {
			sql = "SELECT " + VecToSeparatedList(select_cols) + " FROM (" + right_sql + ") " +
			      (join_type == JoinType::RIGHT_SEMI ? "SEMI" : "ANTI") + " JOIN (" + left_sql + ")";
		} else {
			sql = "SELECT " + VecToSeparatedList(select_cols) + " FROM (" + left_sql + ") " +
			      InlineJoinKeyword(join_type, context) + " JOIN (" + right_sql + ")";
		}
		if (!conditions.empty()) {
			sql += " ON " + VecToSeparatedList(conditions, " AND ");
		}
		return sql;
	}

	//--------------------------------------------------------------------------
	// AstToInlineSQL: generate inline (non-CTE) SQL for a subtree.
	//
	// Used for the recursive step of WITH RECURSIVE: the recursive step may
	// contain self-referencing CteRef nodes that would create forward references
	// if expressed as flat CTEs. Instead, the entire step is serialized as a
	// nested subquery with all column aliases expressed inline.
	//--------------------------------------------------------------------------
	string AstToInlineSQL(const AstNode &ast_node) const {
		std::map<idx_t, string> inline_delim_sources;
		return AstToInlineSQL(ast_node, inline_delim_sources);
	}

	string AstToInlineSQL(const AstNode &ast_node, std::map<idx_t, string> &inline_delim_sources) const {
		const string &type = ast_node.NodeType();

		if (type == "Get") {
			const AstGetNode &get = static_cast<const AstGetNode &>(ast_node);
			string sql = "SELECT ";
			if (get.column_names.empty()) {
				sql += "*";
			} else {
				for (size_t i = 0; i < get.column_names.size(); i++) {
					if (i > 0) {
						sql += ", ";
					}
					const bool is_expr = i < get.column_is_expression.size() && get.column_is_expression[i];
					string source_col = (get.table_name == "(SELECT 1)" || is_expr)
					                        ? get.column_names[i]
					                        : DialectQuoteIdent(get.column_names[i], dialect);
					sql += source_col + " AS " + get.cte_column_names[i];
				}
			}
			sql += " FROM ";
			if (!get.catalog.empty()) {
				sql += DialectQualifiedTableName(get.catalog, get.schema, get.table_name, dialect);
			} else {
				// An in-out (lateral) table function has the delim/correlation source as its AST child:
				// inline it as the left comma-join input (mirrors GetNode's input_cte_name handling).
				if (!ast_node.children.empty()) {
					sql += "(" + AstToInlineSQL(*ast_node.children[0], inline_delim_sources) + "), ";
				}
				sql += get.table_name;
				if (get.table_name.find('(') != string::npos && get.table_name != "(SELECT 1)" &&
				    !get.column_names.empty() && get.table_name.find("ducklake_table_") == string::npos) {
					vector<string> table_function_columns;
					if (!get.table_function_alias.empty()) {
						// Output-ordered alias (same as GetNode::ToQuery): names every output position, so
						// filter-only and function-derived columns (e.g. hive partition keys) stay correct.
						for (const string &name : get.table_function_alias) {
							table_function_columns.push_back(DialectQuoteIdent(name, dialect));
						}
					} else {
						idx_t alias_count = get.table_function_output_count == DConstants::INVALID_INDEX
						                        ? get.column_names.size()
						                        : get.table_function_output_count;
						for (idx_t i = 0; i < alias_count && i < get.column_names.size(); i++) {
							table_function_columns.push_back(DialectQuoteIdent(get.column_names[i], dialect));
						}
					}
					sql += " _tf(" + VecToSeparatedList(table_function_columns) + ")";
				}
			}
			if (!get.table_filters.empty()) {
				sql += " WHERE " + VecToSeparatedList(get.table_filters, " AND ");
			}
			return sql;
		}

		if (type == "CteRef") {
			// Self-reference (cte_index = rec CTE table_index) or reference to a flat CTE.
			const AstCteRefNode &cte_ref = static_cast<const AstCteRefNode &>(ast_node);
			auto it = cte_index_to_body_info.find(cte_ref.cte_table_index);
			if (it == cte_index_to_body_info.end()) {
				throw InternalException("AstToInlineSQL: CteRef references unknown CTE index %llu",
				                        (unsigned long long)cte_ref.cte_table_index);
			}
			const string &src_name = it->second.first;
			const vector<string> &src_cols = it->second.second;
			string sql = "SELECT ";
			for (size_t i = 0; i < src_cols.size(); i++) {
				if (i > 0) {
					sql += ", ";
				}
				// The source columns are user-facing (stripped) names — quote reserved words.
				sql += QuoteIdentifier(src_cols[i]) + " AS " + cte_ref.cte_column_names[i];
			}
			return sql + " FROM " + src_name;
		}

		if (type == "Filter") {
			const AstFilterNode &filter = static_cast<const AstFilterNode &>(ast_node);
			D_ASSERT(ast_node.children.size() == 1);
			// Honor a COLUMN_LIFETIME projection_map: select only the kept child columns instead of
			// SELECT *, so pruned columns don't leak out of an inlined filter subtree.
			string select_list = "*";
			if (!filter.projection_map.empty()) {
				auto child_cols = ast_node.children[0]->OutputColumnNames();
				vector<string> pruned;
				pruned.reserve(filter.projection_map.size());
				for (auto idx : filter.projection_map) {
					pruned.push_back(child_cols[idx]);
				}
				select_list = VecToSeparatedList(pruned);
			}
			string sql =
			    "SELECT " + select_list + " FROM (" + AstToInlineSQL(*ast_node.children[0], inline_delim_sources) + ")";
			if (!filter.conditions.empty()) {
				sql += " WHERE ";
				for (size_t i = 0; i < filter.conditions.size(); i++) {
					if (i > 0) {
						sql += " AND ";
					}
					sql += "(" + filter.conditions[i] + ")";
				}
			}
			return sql;
		}

		if (type == "Project") {
			const AstProjectNode &proj = static_cast<const AstProjectNode &>(ast_node);
			D_ASSERT(ast_node.children.size() == 1);
			string sql = "SELECT ";
			for (size_t i = 0; i < proj.expressions.size(); i++) {
				if (i > 0) {
					sql += ", ";
				}
				sql += proj.expressions[i] + " AS " + proj.cte_column_names[i];
			}
			return sql + " FROM (" + AstToInlineSQL(*ast_node.children[0], inline_delim_sources) + ")";
		}

		if (type == "Join") {
			const AstJoinNode &join = static_cast<const AstJoinNode &>(ast_node);
			D_ASSERT(ast_node.children.size() == 2);
			string left_sql = AstToInlineSQL(*ast_node.children[0], inline_delim_sources);
			string right_sql = AstToInlineSQL(*ast_node.children[1], inline_delim_sources);
			if (join.is_asof) {
				if (join.join_type != JoinType::INNER && join.join_type != JoinType::LEFT) {
					throw NotImplementedException(
					    "LPTS_UNSUPPORTED_RECURSIVE_STEP: ASOF JOIN type %s is not supported in a recursive CTE step",
					    EnumUtil::ToString(join.join_type));
				}
				string join_kw = join.join_type == JoinType::LEFT ? "ASOF LEFT JOIN" : "ASOF JOIN";
				string sql = "SELECT " + VecToSeparatedList(join.cte_column_names) + " FROM (" + left_sql + ") " +
				             join_kw + " (" + right_sql + ")";
				if (!join.conditions.empty()) {
					sql += " ON " + VecToSeparatedList(join.conditions, " AND ");
				}
				return sql;
			}
			return InlineJoinSql(join.join_type, join.cte_column_names, left_sql, right_sql, join.conditions,
			                     join.mark_expression, "JOIN");
		}

		if (type == "PositionalJoin") {
			const AstPositionalJoinNode &join = static_cast<const AstPositionalJoinNode &>(ast_node);
			D_ASSERT(ast_node.children.size() == 2);
			string left_sql = AstToInlineSQL(*ast_node.children[0], inline_delim_sources);
			string right_sql = AstToInlineSQL(*ast_node.children[1], inline_delim_sources);
			return "SELECT " + VecToSeparatedList(join.cte_column_names) + " FROM (" + left_sql +
			       ") POSITIONAL JOIN (" + right_sql + ")";
		}

		if (type == "Sample") {
			const AstSampleNode &sample = static_cast<const AstSampleNode &>(ast_node);
			D_ASSERT(ast_node.children.size() == 1);
			return "SELECT " + VecToSeparatedList(sample.cte_column_names) + " FROM (" +
			       AstToInlineSQL(*ast_node.children[0], inline_delim_sources) + ") USING SAMPLE " +
			       sample.sample_clause;
		}

		if (type == "DelimJoin") {
			const AstDelimJoinNode &join = static_cast<const AstDelimJoinNode &>(ast_node);
			D_ASSERT(ast_node.children.size() == 2);
			string left_sql = AstToInlineSQL(*ast_node.children[0], inline_delim_sources);

			std::map<idx_t, string> saved_sources;
			for (const idx_t table_index : join.delim_table_indices) {
				auto existing = inline_delim_sources.find(table_index);
				if (existing != inline_delim_sources.end()) {
					saved_sources[table_index] = existing->second;
				}
				inline_delim_sources[table_index] = left_sql;
			}
			string right_sql = AstToInlineSQL(*ast_node.children[1], inline_delim_sources);
			for (const idx_t table_index : join.delim_table_indices) {
				auto saved = saved_sources.find(table_index);
				if (saved != saved_sources.end()) {
					inline_delim_sources[table_index] = saved->second;
				} else {
					inline_delim_sources.erase(table_index);
				}
			}

			return InlineJoinSql(join.join_type, join.cte_column_names, left_sql, right_sql, join.conditions,
			                     join.mark_expression, "DELIM_JOIN");
		}

		if (type == "DelimGet") {
			const AstDelimGetNode &delim_get = static_cast<const AstDelimGetNode &>(ast_node);
			auto it = inline_delim_sources.find(delim_get.table_index);
			if (it == inline_delim_sources.end()) {
				throw NotImplementedException(
				    "LPTS_UNSUPPORTED_RECURSIVE_STEP: DelimGet table_index=%llu has no inline DELIM_JOIN source",
				    (unsigned long long)delim_get.table_index);
			}
			string sql = "SELECT DISTINCT ";
			for (size_t i = 0; i < delim_get.source_col_names.size(); i++) {
				if (i > 0) {
					sql += ", ";
				}
				sql += delim_get.source_col_names[i] + " AS " + delim_get.cte_column_names[i];
			}
			return sql + " FROM (" + it->second + ")";
		}

		if (type == "Aggregate") {
			const AstAggregateNode &agg = static_cast<const AstAggregateNode &>(ast_node);
			D_ASSERT(ast_node.children.size() == 1);
			string sql = "SELECT ";
			vector<string> select_items;
			for (size_t i = 0; i < agg.group_by_columns.size(); i++) {
				select_items.push_back(agg.group_by_columns[i] + " AS " + agg.cte_column_names[i]);
			}
			for (size_t i = 0; i < agg.aggregate_expressions.size(); i++) {
				select_items.push_back(agg.aggregate_expressions[i] + " AS " +
				                       agg.cte_column_names[agg.group_by_columns.size() + i]);
			}
			sql += VecToSeparatedList(select_items);
			sql += " FROM (" + AstToInlineSQL(*ast_node.children[0], inline_delim_sources) + ")";
			if (!agg.group_by_clause.empty()) {
				sql += " GROUP BY " + agg.group_by_clause;
			}
			return sql;
		}

		if (type == "Distinct") {
			D_ASSERT(ast_node.children.size() == 1);
			const AstDistinctNode &d = static_cast<const AstDistinctNode &>(ast_node);
			const auto &cols =
			    d.cte_column_names.empty() ? ast_node.children[0]->OutputColumnNames() : d.cte_column_names;
			const string child_sql = "(" + AstToInlineSQL(*ast_node.children[0], inline_delim_sources) + ")";
			if (d.is_distinct_on && !d.distinct_on_targets.empty()) {
				return BuildDistinctOnQuery(cols, d.distinct_on_targets, d.distinct_on_orders, child_sql);
			}
			return "SELECT DISTINCT " + VecToSeparatedList(cols) + " FROM " + child_sql;
		}

		if (type == "Order") {
			const AstOrderNode &o = static_cast<const AstOrderNode &>(ast_node);
			D_ASSERT(ast_node.children.size() == 1);
			return "SELECT * FROM (" + AstToInlineSQL(*ast_node.children[0], inline_delim_sources) + ") ORDER BY " +
			       VecToSeparatedList(o.order_items);
		}

		if (type == "Limit") {
			const AstLimitNode &l = static_cast<const AstLimitNode &>(ast_node);
			D_ASSERT(ast_node.children.size() == 1);
			string sql = "SELECT * FROM (" + AstToInlineSQL(*ast_node.children[0], inline_delim_sources) + ")";
			if (!l.limit_str.empty()) {
				sql += " LIMIT " + l.limit_str;
			}
			if (!l.offset_str.empty()) {
				sql += " OFFSET " + l.offset_str;
			}
			return sql;
		}

		if (type == "TopN") {
			const AstTopNNode &topn = static_cast<const AstTopNNode &>(ast_node);
			D_ASSERT(ast_node.children.size() == 1);
			string sql = "SELECT * FROM (" + AstToInlineSQL(*ast_node.children[0], inline_delim_sources) +
			             ") ORDER BY " + VecToSeparatedList(topn.order_items) + " LIMIT " + std::to_string(topn.limit);
			if (topn.offset > 0) {
				sql += " OFFSET " + std::to_string(topn.offset);
			}
			return sql;
		}

		if (type == "Union") {
			const AstUnionNode &u = static_cast<const AstUnionNode &>(ast_node);
			D_ASSERT(ast_node.children.size() == 2);
			string union_kw = u.is_union_all ? " UNION ALL " : " UNION ";
			return "(" + AstToInlineSQL(*ast_node.children[0], inline_delim_sources) + ")" + union_kw + "(" +
			       AstToInlineSQL(*ast_node.children[1], inline_delim_sources) + ")";
		}

		throw NotImplementedException(
		    "LPTS_UNSUPPORTED_RECURSIVE_STEP: node type '%s' cannot be serialized as inline SQL", type);
	}

	//--------------------------------------------------------------------------
	// TryFlattenMergedPipeline: fuse a maximal chain of single-child pipeline
	// operators into ONE flat SELECT (a MergedSelectNode), instead of emitting
	// one CTE per operator.
	//
	// Canonical operator order, OUTER → INNER:
	//   Limit → OrderBy → Project(top) → Aggregate → Project(bottom) → Filter
	// plus, at the bottom, an absorbed pushdown-free base-table scan (DuckDB).
	//
	// Realizing one flat SELECT requires FOLDING: each operator's expressions
	// reference its child's output column names, so we substitute those names
	// with their defining expressions while walking inner → outer, keeping a
	// `col_map` of output_col_name → expr (in terms of the FROM-source columns).
	//
	// Returns nullptr when `ast_node` does not begin a mergeable chain (zero
	// operators consumed) — the caller then falls through to the per-operator path.
	//--------------------------------------------------------------------------
	unique_ptr<CteNode> TryFlattenMergedPipeline(const AstNode &ast_node) {
		// Fixed slots in canonical outer→inner order. Project appears twice (above and below the
		// aggregate). A filter appears twice too: a HAVING filter sits directly above the aggregate
		// (post-aggregation), and a WHERE filter sits below it (pre-aggregation).
		enum { S_LIMIT = 0, S_ORDER, S_PROJ_TOP, S_HAVING, S_AGG, S_PROJ_BOTTOM, S_FILTER };

		const AstLimitNode *limit_n = nullptr;
		const AstOrderNode *order_n = nullptr;
		const AstProjectNode *proj_top = nullptr;
		const AstFilterNode *having_n = nullptr;
		const AstAggregateNode *agg_n = nullptr;
		const AstProjectNode *proj_bottom = nullptr;
		const AstFilterNode *filter_n = nullptr;

		// 1. Walk down, assigning each operator to the next available slot in
		//    canonical order. Stop (boundary) on out-of-order/duplicate slots,
		//    non-pipeline operators, window projections, or scalar-limits.
		int next_slot = S_LIMIT;
		int consumed = 0;
		const AstNode *cur = &ast_node;
		while (cur->children.size() == 1) {
			const string &t = cur->NodeType();
			int chosen = -1;
			if (t == "Limit") {
				const auto &l = static_cast<const AstLimitNode &>(*cur);
				// A child-scalar LIMIT/OFFSET needs a separate child CTE to read from — cannot fold.
				if (l.limit_needs_child_scalar || l.offset_needs_child_scalar) {
					break;
				}
				if (next_slot <= S_LIMIT) {
					chosen = S_LIMIT;
					limit_n = &l;
				}
			} else if (t == "Order") {
				if (next_slot <= S_ORDER) {
					chosen = S_ORDER;
					order_n = &static_cast<const AstOrderNode &>(*cur);
				}
			} else if (t == "Aggregate") {
				if (next_slot <= S_AGG) {
					chosen = S_AGG;
					agg_n = &static_cast<const AstAggregateNode &>(*cur);
				}
			} else if (t == "Filter") {
				// A filter directly above an aggregate is a HAVING filter (post-aggregation); only
				// admit it when there is a GROUP BY/aggregate below. Otherwise it is a WHERE filter.
				const bool is_having = cur->children[0]->NodeType() == "Aggregate" && next_slot <= S_HAVING;
				if (is_having) {
					chosen = S_HAVING;
					having_n = &static_cast<const AstFilterNode &>(*cur);
				} else if (next_slot <= S_FILTER) {
					chosen = S_FILTER;
					filter_n = &static_cast<const AstFilterNode &>(*cur);
				}
			} else if (t == "Project") {
				const auto &p = static_cast<const AstProjectNode &>(*cur);
				if (p.is_window) {
					break; // window/OVER expressions must not be folded — keep separate.
				}
				if (next_slot <= S_PROJ_TOP) {
					chosen = S_PROJ_TOP;
					proj_top = &p;
				} else if (next_slot <= S_PROJ_BOTTOM) {
					chosen = S_PROJ_BOTTOM;
					proj_bottom = &p;
				}
			} else {
				break; // non-pipeline operator → boundary (the FROM input).
			}
			if (chosen < 0) {
				break; // out-of-order or duplicate slot → boundary.
			}
			next_slot = chosen + 1;
			consumed++;
			cur = cur->children[0].get();
		}

		if (consumed == 0) {
			return nullptr; // not a mergeable chain — let the per-operator path handle it.
		}

		// 2. Decide whether the bottom scan can be absorbed into the FROM clause. Absorb only a
		//    simple physical table with no pushdown (DuckDB dialect): everything else (table
		//    functions, "(SELECT 1)", ducklake, pushdown scans, other dialects) keeps
		//    GetNode::ToQuery as the single source of truth and stays its own CTE.
		//    Compute this first, with no side effects, so the early-out below is clean.
		const AstNode &input = *cur;
		const AstGetNode *absorb_get = nullptr;
		if (input.NodeType() == "Get") {
			const auto &g = static_cast<const AstGetNode &>(input);
			const bool simple_table = dialect == SqlDialect::DUCKDB && !g.catalog.empty() && g.table_filters.empty() &&
			                          g.table_name.find('(') == string::npos && g.table_name != "(SELECT 1)" &&
			                          g.table_name.find("ducklake_table_") == string::npos &&
			                          g.table_function_output_count == DConstants::INVALID_INDEX &&
			                          g.column_names.size() == g.cte_column_names.size();
			if (simple_table) {
				absorb_get = &g;
			}
		}

		// Only build a merged block when we actually fuse ≥2 components: pipeline operators plus
		// an absorbed scan. A single operator with no scan to absorb has nothing to merge, so we
		// return nullptr and let the per-operator path emit its normally-named CTE
		// (filter_/projection_/aggregate_/order_/limit_).
		const int n_ops = (limit_n ? 1 : 0) + (order_n ? 1 : 0) + (proj_top ? 1 : 0) + (having_n ? 1 : 0) +
		                  (agg_n ? 1 : 0) + (proj_bottom ? 1 : 0) + (filter_n ? 1 : 0);
		if (n_ops + (absorb_get ? 1 : 0) < 2) {
			return nullptr;
		}

		// An UNGROUPED aggregate (no GROUP BY keys, e.g. an implicit single-group `count_star()` from a
		// HAVING-without-GROUP-BY) collapses its input to exactly one row purely by virtue of being an
		// aggregate. If we fuse a projection on top of it that drops every aggregate output (e.g.
		// `SELECT 1 AS one ... HAVING 1<2`), the merged SELECT would have neither an aggregate in its
		// select-list nor a GROUP BY, so it would no longer aggregate — emitting one row per input row.
		// In that case do NOT fuse: return nullptr so the aggregate stays its own CTE (which does collapse
		// to one row) and the projection reads from it. Fusion is still fine when the projection or HAVING
		// references an aggregate output (the merged SELECT then remains an aggregating query).
		if (agg_n && agg_n->group_by_columns.empty()) {
			auto token_appears = [](const string &hay, const string &needle) {
				if (needle.empty()) {
					return false;
				}
				for (size_t pos = hay.find(needle); pos != string::npos; pos = hay.find(needle, pos + 1)) {
					const bool left_ok =
					    pos == 0 || !(std::isalnum((unsigned char)hay[pos - 1]) || hay[pos - 1] == '_');
					const size_t end = pos + needle.size();
					const bool right_ok =
					    end >= hay.size() || !(std::isalnum((unsigned char)hay[end]) || hay[end] == '_');
					if (left_ok && right_ok) {
						return true;
					}
				}
				return false;
			};
			bool aggregate_output_used = false;
			// Ungrouped ⇒ every cte_column_name is an aggregate output.
			for (const string &agg_out : agg_n->cte_column_names) {
				if (proj_top) {
					for (const string &e : proj_top->expressions) {
						if (token_appears(e, agg_out)) {
							aggregate_output_used = true;
							break;
						}
					}
				}
				if (!aggregate_output_used && having_n) {
					for (const string &c : having_n->conditions) {
						if (token_appears(c, agg_out)) {
							aggregate_output_used = true;
							break;
						}
					}
				}
				if (aggregate_output_used) {
					break;
				}
			}
			if (proj_top && !aggregate_output_used) {
				return nullptr;
			}
		}

		// Materialize the FROM input: absorb the base-table scan, else recurse to a child CTE.
		unordered_map<string, string> col_map; // visible col name → folded expr over FROM source.
		vector<string> visible;                // visible column names, in order.
		string from_clause;
		if (absorb_get) {
			from_clause =
			    DialectQualifiedTableName(absorb_get->catalog, absorb_get->schema, absorb_get->table_name, dialect);
			for (size_t i = 0; i < absorb_get->cte_column_names.size(); i++) {
				// Struct field-extraction columns are raw SQL expressions, emitted verbatim; plain columns
				// are quoted identifiers.
				const bool is_expr = i < absorb_get->column_is_expression.size() && absorb_get->column_is_expression[i];
				col_map[absorb_get->cte_column_names[i]] =
				    is_expr ? absorb_get->column_names[i] : DialectQuoteIdent(absorb_get->column_names[i], dialect);
				visible.push_back(absorb_get->cte_column_names[i]);
			}
		} else {
			unique_ptr<CteNode> input_cte = FlattenNode(input);
			from_clause = input_cte->cte_name;
			visible = input_cte->cte_column_list;
			for (const auto &c : input_cte->cte_column_list) {
				col_map[c] = c;
			}
			cte_nodes.push_back(std::move(input_cte));
		}

		// 3. Fold operators inner → outer, accumulating clauses and updating col_map/visible.
		vector<string> where_conditions;
		vector<string> having_conditions;
		string group_by_text;
		vector<string> order_items;
		string limit_out;
		string offset_out;

		if (filter_n) {
			for (const auto &c : filter_n->conditions) {
				where_conditions.push_back(SubstituteColumnTokens(c, col_map));
			}
			// Honor COLUMN_LIFETIME pruning when the filter is the outermost op; an
			// overlying projection would override `visible` anyway.
			if (!filter_n->projection_map.empty()) {
				vector<string> pruned;
				pruned.reserve(filter_n->projection_map.size());
				for (auto idx : filter_n->projection_map) {
					pruned.push_back(visible[idx]);
				}
				visible = std::move(pruned);
			}
		}
		if (proj_bottom) {
			unordered_map<string, string> nm;
			vector<string> nv;
			for (size_t i = 0; i < proj_bottom->cte_column_names.size(); i++) {
				nm[proj_bottom->cte_column_names[i]] = SubstituteColumnTokens(proj_bottom->expressions[i], col_map);
				nv.push_back(proj_bottom->cte_column_names[i]);
			}
			col_map = std::move(nm);
			visible = std::move(nv);
		}
		if (agg_n) {
			group_by_text = SubstituteColumnTokens(agg_n->group_by_clause, col_map);
			unordered_map<string, string> nm;
			vector<string> nv;
			// Output columns are group keys first, then aggregate expressions
			// (mirrors AggregateNode::ToQuery / AstToInlineSQL ordering).
			for (size_t i = 0; i < agg_n->group_by_columns.size(); i++) {
				nm[agg_n->cte_column_names[i]] = SubstituteColumnTokens(agg_n->group_by_columns[i], col_map);
				nv.push_back(agg_n->cte_column_names[i]);
			}
			for (size_t j = 0; j < agg_n->aggregate_expressions.size(); j++) {
				const string &out = agg_n->cte_column_names[agg_n->group_by_columns.size() + j];
				nm[out] = SubstituteColumnTokens(agg_n->aggregate_expressions[j], col_map);
				nv.push_back(out);
			}
			col_map = std::move(nm);
			visible = std::move(nv);
		}
		if (having_n) {
			// HAVING sits directly above the aggregate, so its conditions reference the aggregate's
			// output columns — fold them with the post-aggregate map.
			for (const auto &c : having_n->conditions) {
				having_conditions.push_back(SubstituteColumnTokens(c, col_map));
			}
			if (!having_n->projection_map.empty()) {
				vector<string> pruned;
				pruned.reserve(having_n->projection_map.size());
				for (auto idx : having_n->projection_map) {
					pruned.push_back(visible[idx]);
				}
				visible = std::move(pruned);
			}
		}
		if (proj_top) {
			unordered_map<string, string> nm;
			vector<string> nv;
			for (size_t i = 0; i < proj_top->cte_column_names.size(); i++) {
				nm[proj_top->cte_column_names[i]] = SubstituteColumnTokens(proj_top->expressions[i], col_map);
				nv.push_back(proj_top->cte_column_names[i]);
			}
			col_map = std::move(nm);
			visible = std::move(nv);
		}
		if (order_n) {
			for (const auto &it : order_n->order_items) {
				order_items.push_back(SubstituteColumnTokens(it, col_map));
			}
		}
		if (limit_n) {
			limit_out = limit_n->limit_str;
			offset_out = limit_n->offset_str;
		}

		// 4. Build the final SELECT list and the merged node.
		vector<string> select_exprs;
		select_exprs.reserve(visible.size());
		for (const auto &col : visible) {
			auto it = col_map.find(col);
			// Raw expression; MergedSelectNode aliases it to the output name (cte_column_list = visible),
			// and a redundant "x AS x" is collapsed globally via SwallowSelfAlias.
			select_exprs.push_back((it != col_map.end()) ? it->second : col);
		}
		const size_t my_index = node_count++;
		LPTS_DEBUG_PRINT("[LPTS-CTE] MergedSelect: block_" + std::to_string(my_index) +
		                 " n_ops=" + std::to_string(n_ops) + " from='" + from_clause + "'");
		return make_uniq<MergedSelectNode>(my_index, std::move(visible), std::move(select_exprs),
		                                   std::move(from_clause), std::move(where_conditions),
		                                   std::move(having_conditions), std::move(group_by_text),
		                                   std::move(order_items), std::move(limit_out), std::move(offset_out));
	}

	/// Mark a join's right (inner) side — which is always materialized in its own CTE — by appending
	/// "_materialized_for_join" to that CTE's name. Returns the new name. The right input is referenced
	/// only by this join, so renaming the node here keeps every reference consistent. DuckDB output
	/// only; other dialects keep the plain CTE name.
	string MarkRightSideMaterialized(const string &right_cte_name) {
		if (dialect != SqlDialect::DUCKDB) {
			return right_cte_name;
		}
		static const string kSuffix = "_materialized_for_join";
		for (auto &node : cte_nodes) {
			if (node->cte_name == right_cte_name) {
				node->cte_name = right_cte_name + kSuffix;
				return node->cte_name;
			}
		}
		return right_cte_name; // not found (e.g. shared/registered CTE) — leave unchanged.
	}

	//--------------------------------------------------------------------------
	// FlattenNode: post-order walk → produce CteNode for each AstNode
	//--------------------------------------------------------------------------
	unique_ptr<CteNode> FlattenNode(const AstNode &ast_node) {
		const string &type = ast_node.NodeType();

		// MaterializedCte: must flatten body first, store body info, then flatten outer query.
		// This ordering ensures CteRef nodes in the outer query can resolve the body CTE name.
		if (type == "MaterializedCte") {
			const AstMaterializedCteNode &mat = static_cast<const AstMaterializedCteNode &>(ast_node);
			LPTS_DEBUG_PRINT("[LPTS-CTE] MaterializedCte: flattening body (cte_table_index=" +
			                 std::to_string(mat.cte_table_index) + ")");
			unique_ptr<CteNode> body_last = FlattenNode(*ast_node.children[0]);
			cte_index_to_body_info[mat.cte_table_index] = {body_last->cte_name, body_last->cte_column_list};
			cte_nodes.push_back(std::move(body_last));
			LPTS_DEBUG_PRINT("[LPTS-CTE] MaterializedCte: body stored as '" +
			                 cte_index_to_body_info[mat.cte_table_index].first + "', flattening outer query");
			return FlattenNode(*ast_node.children[1]);
		}

		// CteRef: leaf node — create a GetNode that reads from the materialized body CTE.
		// The body's LPTS column names become the SELECT list; this CTE's column names become the header.
		if (type == "CteRef") {
			const AstCteRefNode &cte_ref_node = static_cast<const AstCteRefNode &>(ast_node);
			auto it = cte_index_to_body_info.find(cte_ref_node.cte_table_index);
			if (it == cte_index_to_body_info.end()) {
				throw InternalException("LPTS: CteRef references unknown materialized CTE index %llu",
				                        (unsigned long long)cte_ref_node.cte_table_index);
			}
			const string &body_lpts_name = it->second.first;
			const vector<string> &body_lpts_cols = it->second.second;
			const size_t my_index = node_count++;
			LPTS_DEBUG_PRINT("[LPTS-CTE] CteRef: scan_" + std::to_string(my_index) + " -> SELECT FROM '" +
			                 body_lpts_name + "'");
			// scan_N(ref_col1, ...) AS (SELECT body_col1, ... FROM body_lpts_name)
			return make_uniq<GetNode>(my_index, cte_ref_node.cte_column_names, "", "", body_lpts_name, 0,
			                          vector<string>(), body_lpts_cols);
		}

		// DelimJoin: special ordering — flatten outer (left) child first, then register the
		// DELIM_GET source CTE name, then flatten inner (right) child, then create JOIN CTE.
		// This mirrors the MaterializedCte pattern of controlling child ordering.
		if (type == "DelimJoin") {
			const AstDelimJoinNode &dj = static_cast<const AstDelimJoinNode &>(ast_node);
			D_ASSERT(ast_node.children.size() == 2);

			// 1. Flatten outer (left) child.
			unique_ptr<CteNode> left_cte = FlattenNode(*ast_node.children[0]);
			string left_cte_name = left_cte->cte_name;
			LPTS_DEBUG_PRINT("[LPTS-CTE] DelimJoin: left_cte='" + left_cte_name +
			                 "' n_delim_tis=" + std::to_string(dj.delim_table_indices.size()));
			cte_nodes.push_back(std::move(left_cte));

			// 2. Register the outer CTE as the source for ALL DELIM_GETs in the inner subtree.
			for (const idx_t dti : dj.delim_table_indices) {
				LPTS_DEBUG_PRINT("[LPTS-CTE] DelimJoin: registering delim_ti=" + std::to_string(dti) + " -> '" +
				                 left_cte_name + "'");
				delim_get_source_cte[dti] = left_cte_name;
			}

			// 3. Flatten inner (right) child. AstDelimGetNode will pick up delim_get_source_cte.
			unique_ptr<CteNode> right_cte = FlattenNode(*ast_node.children[1]);
			string right_cte_name = right_cte->cte_name;
			cte_nodes.push_back(std::move(right_cte));

			// 4. Create the DELIM_JOIN as a regular JOIN CTE.
			const size_t my_index = node_count++;
			const string right_name = MarkRightSideMaterialized(right_cte_name);
			LPTS_DEBUG_PRINT("[LPTS-CTE] DelimJoin: join_" + std::to_string(my_index) + " LEFT='" + left_cte_name +
			                 "' RIGHT='" + right_name + "' mark_expr='" + dj.mark_expression + "'");
			auto join_node = make_uniq<JoinNode>(my_index, dj.cte_column_names, left_cte_name, right_name, dj.join_type,
			                                     dj.conditions, dj.mark_expression);
			join_node->mark_lhs_key = dj.mark_lhs_key;
			join_node->mark_rhs_key = dj.mark_rhs_key;
			join_node->mark_correlation_conditions = dj.mark_correlation_conditions;
			join_node->mark_membership_conditions = dj.mark_membership_conditions;
			join_node->mark_membership_comparisons = dj.mark_membership_comparisons;
			join_node->mark_membership_lhs = dj.mark_membership_lhs;
			join_node->mark_membership_rhs = dj.mark_membership_rhs;
			join_node->mark_join_has_equality = dj.mark_join_has_equality;
			return join_node;
		}

		// DelimGet: leaf node — creates a SELECT DISTINCT CTE from the outer left CTE.
		// The outer left CTE name was registered by the parent DelimJoin handler above.
		if (type == "DelimGet") {
			const AstDelimGetNode &dg = static_cast<const AstDelimGetNode &>(ast_node);
			auto it = delim_get_source_cte.find(dg.table_index);
			if (it == delim_get_source_cte.end()) {
				throw NotImplementedException("LPTS_UNSUPPORTED_DELIM_GET: nested table_index=%llu is not implemented "
				                              "without a parent DelimJoin source",
				                              (unsigned long long)dg.table_index);
			}
			const string &source_cte_name = it->second;
			const size_t my_index = node_count++;
			LPTS_DEBUG_PRINT("[LPTS-CTE] DelimGet: scan_" + std::to_string(my_index) + " SELECT DISTINCT FROM '" +
			                 source_cte_name + "'");
			return make_uniq<DelimGetNode>(my_index, dg.cte_column_names, source_cte_name, dg.source_col_names);
		}

		// RecursiveCte: flatten anchor first (as flat CTEs), then generate inline SQL for the
		// recursive step (to avoid forward references to the recursive CTE name), then create
		// the RecursiveCteNode. Returns a GetNode that maps the recursive CTE's exposed columns
		// to the LPTS-prefixed names expected by parent operators.
		if (type == "RecursiveCte") {
			const AstRecursiveCteNode &rec = static_cast<const AstRecursiveCteNode &>(ast_node);
			D_ASSERT(ast_node.children.size() == 2);

			// 1. Flatten anchor: push all anchor flat CTEs; anchor_tail is the last one.
			unique_ptr<CteNode> anchor_tail = FlattenNode(*ast_node.children[0]);
			const string anchor_cte_name = anchor_tail->cte_name;
			const vector<string> anchor_lpts_cols = anchor_tail->cte_column_list;
			cte_nodes.push_back(std::move(anchor_tail));

			// 2. Derive the stripped column names (user-visible, e.g. "n") for the CTE header. Dedup
			//    (case-insensitively, like DuckDB's resolution): two anchor outputs can strip to the same
			//    bare name (e.g. an anchor projecting one source column twice), and a CTE header must not
			//    declare duplicate columns.
			vector<string> stripped_cols;
			unordered_set<string> seen_stripped;
			for (const string &c : anchor_lpts_cols) {
				const size_t pos = c.find('_');
				string bare = (pos != string::npos && pos + 1 < c.size()) ? c.substr(pos + 1) : c;
				string candidate = bare;
				for (idx_t suffix = 1; seen_stripped.count(StringUtil::Lower(candidate)) > 0; suffix++) {
					candidate = bare + "_" + std::to_string(suffix);
				}
				seen_stripped.insert(StringUtil::Lower(candidate));
				stripped_cols.push_back(std::move(candidate));
			}

			// 3. Assign the recursive CTE's index and name; register in cte_index_to_body_info
			//    so self-referencing CteRef nodes inside the recursive step can resolve to it.
			const size_t rec_index = node_count++;
			const string rec_name = "t" + std::to_string(rec_index) + "_recursive_cte";
			cte_index_to_body_info[rec.cte_table_index] = {rec_name, stripped_cols};
			LPTS_DEBUG_PRINT("[LPTS-CTE] RecursiveCte: " + rec_name + " anchor='" + anchor_cte_name +
			                 "' stripped_cols=[" + VecToSeparatedList(stripped_cols) + "]");

			// 4. Generate inline SQL for the recursive step.
			const string recursive_step_sql = AstToInlineSQL(*ast_node.children[1]);
			LPTS_DEBUG_PRINT("[LPTS-CTE] RecursiveCte: recursive_step_sql='" + recursive_step_sql + "'");

			// 5. Push the RecursiveCteNode and mark the list as recursive.
			cte_nodes.push_back(make_uniq<RecursiveCteNode>(rec_index, stripped_cols, anchor_cte_name, anchor_lpts_cols,
			                                                recursive_step_sql, rec.union_all));
			has_recursive_cte = true;

			// 6. Return a GetNode that maps the recursive CTE's exposed column names (stripped)
			//    to the LPTS-prefixed names (output_col_names) that parent nodes expect.
			const size_t scan_index = node_count++;
			LPTS_DEBUG_PRINT("[LPTS-CTE] RecursiveCte: scan_" + std::to_string(scan_index) + " -> SELECT FROM '" +
			                 rec_name + "'");
			return make_uniq<GetNode>(scan_index, rec.output_col_names, "", "", rec_name, 0, vector<string>(),
			                          stripped_cols);
		}

		// Pipeline fusion: try to collapse a chain of single-child pipeline operators
		// into one flat SELECT. Runs after the special-ordering cases above (which must
		// keep their bespoke flattening) and before the generic per-operator path.
		if (merge_pipeline) {
			if (auto merged = TryFlattenMergedPipeline(ast_node)) {
				return merged;
			}
		}

		// 1. Recurse into children first (post-order), remembering each child's CTE
		//    name so the parent can reference it by name. We keep the name (not the
		//    child's idx) because UNION flattening may insert intermediate CTEs into
		//    cte_nodes; once that happens, a child's idx no longer equals its vector
		//    position, and `cte_nodes[idx]` silently picks up the wrong CTE.
		vector<string> children_names;
		vector<vector<string>> children_column_lists;
		for (const auto &child : ast_node.children) {
			unique_ptr<CteNode> child_cte = FlattenNode(*child);
			children_names.push_back(child_cte->cte_name);
			children_column_lists.push_back(child_cte->cte_column_list);
			cte_nodes.push_back(std::move(child_cte));
		}

		// 2. Assign an index to this node.
		const size_t my_index = node_count++;

		// 3. Create the CteNode matching this AstNode type.
		if (type == "Get") {
			const AstGetNode &get = static_cast<const AstGetNode &>(ast_node);
			// Dialect-specific table reference:
			//   DuckDB:   catalog.schema.table  (e.g. memory.main.users)
			//   Postgres: table                 (e.g. users)
			string catalog_out = DialectUsesUnqualifiedTableNames(dialect) ? "" : get.catalog;
			string schema_out = DialectUsesUnqualifiedTableNames(dialect) ? "" : get.schema;
			string input_cte_name = children_names.empty() ? string() : children_names[0];
			auto get_node = make_uniq<GetNode>(my_index, get.cte_column_names, catalog_out, schema_out, get.table_name,
			                                   get.table_index, get.table_filters, get.column_names, input_cte_name,
			                                   get.table_function_output_count);
			get_node->table_function_alias = get.table_function_alias;
			get_node->column_is_expression = get.column_is_expression;
			get_node->spark_broadcast_hint =
			    emit_spark_hints && dialect == SqlDialect::SPARK && IsOpenIvmDeltaTable(get.table_name);
			return unique_ptr<CteNode>(std::move(get_node));
		}

		if (type == "Filter") {
			const AstFilterNode &filter = static_cast<const AstFilterNode &>(ast_node);
			// Filters are pass-through operators: SELECT * keeps the child's column
			// layout. Preserve it so a filter-rooted subtree can produce a valid
			// FinalReadNode instead of an empty SELECT list.
			// COLUMN_LIFETIME may set a projection_map that prunes columns at the filter; honor it so the
			// emitted column list matches the filter's real output (otherwise pruned columns leak upward).
			vector<string> filter_columns = children_column_lists[0];
			if (!filter.projection_map.empty()) {
				filter_columns.clear();
				filter_columns.reserve(filter.projection_map.size());
				for (auto idx : filter.projection_map) {
					filter_columns.push_back(children_column_lists[0][idx]);
				}
			}
			auto node =
			    make_uniq<FilterNode>(my_index, std::move(filter_columns), children_names[0], filter.conditions);
			node->spark_broadcast_hint = cte_nodes.back()->spark_broadcast_hint;
			return unique_ptr<CteNode>(std::move(node));
		}

		if (type == "Project") {
			const AstProjectNode &proj = static_cast<const AstProjectNode &>(ast_node);
			auto node = make_uniq<ProjectNode>(my_index, proj.cte_column_names, children_names[0], proj.expressions,
			                                   proj.table_index);
			node->spark_broadcast_hint = cte_nodes.back()->spark_broadcast_hint;
			return unique_ptr<CteNode>(std::move(node));
		}

		if (type == "Aggregate") {
			const AstAggregateNode &agg = static_cast<const AstAggregateNode &>(ast_node);
			auto node = make_uniq<AggregateNode>(my_index, agg.cte_column_names, children_names[0],
			                                     agg.group_by_columns, agg.group_by_clause, agg.aggregate_expressions);
			node->spark_broadcast_hint = cte_nodes.back()->spark_broadcast_hint;
			return unique_ptr<CteNode>(std::move(node));
		}

		if (type == "Join") {
			const AstJoinNode &join = static_cast<const AstJoinNode &>(ast_node);
			const string right_name = MarkRightSideMaterialized(children_names[1]);
			auto join_node = make_uniq<JoinNode>(my_index, join.cte_column_names, children_names[0], right_name,
			                                     join.join_type, join.conditions, join.mark_expression, join.is_asof,
			                                     cte_nodes[cte_nodes.size() - 2]->spark_broadcast_hint,
			                                     cte_nodes[cte_nodes.size() - 1]->spark_broadcast_hint);
			join_node->mark_lhs_key = join.mark_lhs_key;
			join_node->mark_rhs_key = join.mark_rhs_key;
			join_node->mark_correlation_conditions = join.mark_correlation_conditions;
			join_node->mark_membership_conditions = join.mark_membership_conditions;
			join_node->mark_membership_comparisons = join.mark_membership_comparisons;
			join_node->mark_membership_lhs = join.mark_membership_lhs;
			join_node->mark_membership_rhs = join.mark_membership_rhs;
			join_node->mark_join_has_equality = join.mark_join_has_equality;
			return join_node;
		}

		if (type == "PositionalJoin") {
			const AstPositionalJoinNode &join = static_cast<const AstPositionalJoinNode &>(ast_node);
			const string right_name = MarkRightSideMaterialized(children_names[1]);
			return make_uniq<PositionalJoinNode>(my_index, join.cte_column_names, children_names[0], right_name);
		}

		if (type == "Sample") {
			const AstSampleNode &sample = static_cast<const AstSampleNode &>(ast_node);
			const auto &cols = sample.cte_column_names.empty() ? children_column_lists[0] : sample.cte_column_names;
			return make_uniq<SampleNode>(my_index, cols, children_names[0], sample.sample_clause);
		}

		if (type == "Union") {
			const AstUnionNode &u = static_cast<const AstUnionNode &>(ast_node);
			if (children_names.size() == 2) {
				auto union_node = make_uniq<UnionNode>(my_index, u.cte_column_names, children_names[0],
				                                       children_names[1], u.is_union_all);
				// Children can expose extra columns (e.g. an inner ORDER BY key) — the branch SELECTs
				// only the union's arity (see UnionNode::ToQuery).
				union_node->left_columns = children_column_lists[0];
				union_node->right_columns = children_column_lists[1];
				return union_node;
			}
			if (children_names.size() == 1) {
				return make_uniq<ProjectNode>(my_index, u.cte_column_names, children_names[0], children_column_lists[0],
				                              0);
			}
			// N-ary UNION: chain as left-deep binary UNIONs
			// (A UNION B UNION C) → UNION(UNION(A, B), C)
			string prev_cte_name = children_names[0];
			vector<string> prev_columns = children_column_lists[0];
			for (size_t ci = 1; ci < children_names.size(); ci++) {
				const string &right_cte_name = children_names[ci];
				if (ci < children_names.size() - 1) {
					// Intermediate union — create a CTE and add to cte_nodes
					size_t intermediate_index = node_count++;
					auto intermediate = make_uniq<UnionNode>(intermediate_index, u.cte_column_names, prev_cte_name,
					                                         right_cte_name, u.is_union_all);
					intermediate->left_columns = prev_columns;
					intermediate->right_columns = children_column_lists[ci];
					prev_cte_name = intermediate->cte_name;
					prev_columns = u.cte_column_names;
					cte_nodes.push_back(std::move(intermediate));
				} else {
					// Final union — use the current my_index
					auto final_union = make_uniq<UnionNode>(my_index, u.cte_column_names, prev_cte_name, right_cte_name,
					                                        u.is_union_all);
					final_union->left_columns = prev_columns;
					final_union->right_columns = children_column_lists[ci];
					return final_union;
				}
			}
			// Shouldn't reach here, but just in case
			throw InternalException("AstFlattener: empty UNION chain");
		}

		if (type == "SetOperation") {
			const AstSetOperationNode &s = static_cast<const AstSetOperationNode &>(ast_node);
			if (children_names.size() != 2) {
				throw InternalException("AstFlattener: EXCEPT/INTERSECT expected exactly two children");
			}
			if (children_column_lists[0].size() < s.cte_column_names.size() ||
			    children_column_lists[1].size() < s.cte_column_names.size()) {
				throw InternalException("AstFlattener: EXCEPT/INTERSECT child has fewer columns than output");
			}
			vector<string> left_select_columns(children_column_lists[0].begin(),
			                                   children_column_lists[0].begin() + s.cte_column_names.size());
			vector<string> right_select_columns(children_column_lists[1].begin(),
			                                    children_column_lists[1].begin() + s.cte_column_names.size());
			return make_uniq<CteSetOperationNode>(my_index, s.cte_column_names, children_names[0], children_names[1],
			                                      s.op_name, std::move(left_select_columns),
			                                      std::move(right_select_columns), s.is_all);
		}

		if (type == "Order") {
			const AstOrderNode &o = static_cast<const AstOrderNode &>(ast_node);
			const auto &cols = o.cte_column_names.empty() ? children_column_lists[0] : o.cte_column_names;
			return make_uniq<OrderNode>(my_index, cols, children_names[0], o.order_items);
		}

		if (type == "Limit") {
			const AstLimitNode &l = static_cast<const AstLimitNode &>(ast_node);
			const auto &cols = l.cte_column_names.empty() ? children_column_lists[0] : l.cte_column_names;
			return make_uniq<LimitNode>(my_index, cols, children_names[0], l.limit_str, l.offset_str,
			                            l.limit_needs_child_scalar, l.offset_needs_child_scalar);
		}

		if (type == "TopN") {
			const AstTopNNode &t = static_cast<const AstTopNNode &>(ast_node);
			return make_uniq<TopNNode>(my_index, t.cte_column_names, children_names[0], t.order_items, t.limit,
			                           t.offset);
		}

		if (type == "Distinct") {
			const AstDistinctNode &d = static_cast<const AstDistinctNode &>(ast_node);
			const auto &cols = d.cte_column_names.empty() ? children_column_lists[0] : d.cte_column_names;
			auto distinct_node = make_uniq<DistinctNode>(my_index, cols, children_names[0]);
			distinct_node->is_distinct_on = d.is_distinct_on;
			distinct_node->distinct_on_targets = d.distinct_on_targets;
			distinct_node->distinct_on_orders = d.distinct_on_orders;
			return distinct_node;
		}

		// Operators not yet implemented.
		throw NotImplementedException(
		    "LPTS_UNSUPPORTED_OPERATOR: AST node type '%s' is not yet implemented by the flattener", type);
	}

public:
	explicit AstFlattener(SqlDialect dialect = SqlDialect::DUCKDB, bool emit_spark_hints = false,
	                      bool merge_pipeline = true)
	    : dialect(dialect), emit_spark_hints(emit_spark_hints), merge_pipeline(merge_pipeline) {
	}

	/// Flatten the AST rooted at `root` into a CteList.
	/// The root node is handled specially (it produces the FinalReadNode).
	unique_ptr<CteList> Flatten(const AstNode &root) {
		const string &type = root.NodeType();

		// INSERT INTO: the root is an AstInsertNode wrapping a child plan.
		if (type == "Insert") {
			const AstInsertNode &ins = static_cast<const AstInsertNode &>(root);
			D_ASSERT(root.children.size() == 1);
			unique_ptr<CteNode> last_cte = FlattenNode(*root.children[0]);
			const size_t final_index = node_count++;
			auto insert_node =
			    make_uniq<InsertNode>(final_index, ins.target_table, last_cte->cte_name, ins.action_type);
			cte_nodes.push_back(std::move(last_cte));
			return make_uniq<CteList>(std::move(cte_nodes), std::move(insert_node), has_recursive_cte, dialect,
			                          emit_spark_hints);
		}

		// Regular SELECT: FlattenNode handles the entire subtree bottom-up.
		unique_ptr<CteNode> last_cte = FlattenNode(root);

		// Build the FinalReadNode: maps CTE column names back to original names.
		vector<string> final_column_list;
		const vector<string> &cte_cols = (type == "Project")
		                                     ? static_cast<const AstProjectNode &>(root).cte_column_names
		                                     : last_cte->cte_column_list;

		for (const string &cte_col : cte_cols) {
			// Strip exactly one leading "t<digits>_" prefix (a generated table prefix) to recover the
			// user-visible name (t1_name → name, t3_count_star → count_star). Bare names such as user
			// aliases (e.g. "total_rev") have no such prefix and are left untouched.
			size_t strip = 0;
			if (cte_col.size() > 2 && cte_col[0] == 't') {
				size_t d = 1;
				while (d < cte_col.size() && cte_col[d] >= '0' && cte_col[d] <= '9') {
					d++;
				}
				if (d > 1 && d < cte_col.size() && cte_col[d] == '_') {
					strip = d + 1;
				}
			}
			final_column_list.push_back(strip > 0 ? cte_col.substr(strip) : cte_col);
		}

		const size_t final_index = node_count++;
		auto final_node = make_uniq<FinalReadNode>(final_index, last_cte->cte_name, last_cte->cte_column_list,
		                                           std::move(final_column_list));
		cte_nodes.push_back(std::move(last_cte));
		return make_uniq<CteList>(std::move(cte_nodes), std::move(final_node), has_recursive_cte, dialect,
		                          emit_spark_hints);
	}
};

//==============================================================================
// Phase 2 entry point
//==============================================================================
unique_ptr<CteList> AstToCteList(const AstNode &root, SqlDialect dialect, bool emit_spark_hints, bool merge_pipeline) {
	AstFlattener flattener(dialect, emit_spark_hints, merge_pipeline);
	return flattener.Flatten(root);
}

} // namespace duckdb
