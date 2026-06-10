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
// using the same monotonically increasing counter as the original pipeline,
// generating scan_N, filter_N, projection_N, etc.
//==============================================================================
class AstFlattener {
private:
	size_t node_count = 0;
	vector<unique_ptr<CteNode>> cte_nodes;
	SqlDialect dialect;             // Controls dialect-specific SQL rendering.
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
			throw NotImplementedException("AstToInlineSQL: %s type %s not supported in recursive CTE step", context,
			                              EnumUtil::ToString(join_type));
		}
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
					string source_col = get.table_name == "(SELECT 1)"
					                        ? get.column_names[i]
					                        : DialectQuoteIdent(get.column_names[i], dialect);
					sql += source_col + " AS " + get.cte_column_names[i];
				}
			}
			sql += " FROM ";
			if (!get.catalog.empty()) {
				sql += DialectQualifiedTableName(get.catalog, get.schema, get.table_name, dialect);
			} else {
				sql += get.table_name;
				if (get.table_name.find('(') != string::npos && get.table_name != "(SELECT 1)" &&
				    !get.column_names.empty() && get.table_name.find("ducklake_table_") == string::npos) {
					idx_t alias_count = get.table_function_output_count == DConstants::INVALID_INDEX
					                        ? get.column_names.size()
					                        : get.table_function_output_count;
					vector<string> table_function_columns;
					for (idx_t i = 0; i < alias_count && i < get.column_names.size(); i++) {
						table_function_columns.push_back(DialectQuoteIdent(get.column_names[i], dialect));
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
				sql += src_cols[i] + " AS " + cte_ref.cte_column_names[i];
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
					throw NotImplementedException("AstToInlineSQL: ASOF JOIN type %s is not supported",
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
				    "AstToInlineSQL: DelimGet table_index=%llu has no inline DELIM_JOIN source",
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
			return "SELECT DISTINCT " + VecToSeparatedList(cols) + " FROM (" +
			       AstToInlineSQL(*ast_node.children[0], inline_delim_sources) + ")";
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
		    "AstToInlineSQL: node type '%s' is not supported in a recursive CTE step — cannot serialize as inline SQL",
		    type);
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
			LPTS_DEBUG_PRINT("[LPTS-CTE] DelimJoin: join_" + std::to_string(my_index) + " LEFT='" + left_cte_name +
			                 "' RIGHT='" + right_cte_name + "' mark_expr='" + dj.mark_expression + "'");
			return make_uniq<JoinNode>(my_index, dj.cte_column_names, left_cte_name, right_cte_name, dj.join_type,
			                           dj.conditions, dj.mark_expression);
		}

		// DelimGet: leaf node — creates a SELECT DISTINCT CTE from the outer left CTE.
		// The outer left CTE name was registered by the parent DelimJoin handler above.
		if (type == "DelimGet") {
			const AstDelimGetNode &dg = static_cast<const AstDelimGetNode &>(ast_node);
			auto it = delim_get_source_cte.find(dg.table_index);
			if (it == delim_get_source_cte.end()) {
				throw NotImplementedException(
				    "LPTS: nested DelimGet table_index=%llu is not implemented without a parent DelimJoin source",
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

			// 2. Derive the stripped column names (user-visible, e.g. "n") for the CTE header.
			vector<string> stripped_cols;
			for (const string &c : anchor_lpts_cols) {
				const size_t pos = c.find('_');
				stripped_cols.push_back((pos != string::npos && pos + 1 < c.size()) ? c.substr(pos + 1) : c);
			}

			// 3. Assign the recursive CTE's index and name; register in cte_index_to_body_info
			//    so self-referencing CteRef nodes inside the recursive step can resolve to it.
			const size_t rec_index = node_count++;
			const string rec_name = "recursive_cte_" + std::to_string(rec_index);
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
			return make_uniq<GetNode>(my_index, get.cte_column_names, catalog_out, schema_out, get.table_name,
			                          get.table_index, get.table_filters, get.column_names, input_cte_name,
			                          get.table_function_output_count);
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
			return make_uniq<FilterNode>(my_index, std::move(filter_columns), children_names[0], filter.conditions);
		}

		if (type == "Project") {
			const AstProjectNode &proj = static_cast<const AstProjectNode &>(ast_node);
			return make_uniq<ProjectNode>(my_index, proj.cte_column_names, children_names[0], proj.expressions,
			                              proj.table_index);
		}

		if (type == "Aggregate") {
			const AstAggregateNode &agg = static_cast<const AstAggregateNode &>(ast_node);
			return make_uniq<AggregateNode>(my_index, agg.cte_column_names, children_names[0], agg.group_by_columns,
			                                agg.group_by_clause, agg.aggregate_expressions);
		}

		if (type == "Join") {
			const AstJoinNode &join = static_cast<const AstJoinNode &>(ast_node);
			return make_uniq<JoinNode>(my_index, join.cte_column_names, children_names[0], children_names[1],
			                           join.join_type, join.conditions, join.mark_expression, join.is_asof);
		}

		if (type == "PositionalJoin") {
			const AstPositionalJoinNode &join = static_cast<const AstPositionalJoinNode &>(ast_node);
			return make_uniq<PositionalJoinNode>(my_index, join.cte_column_names, children_names[0], children_names[1]);
		}

		if (type == "Sample") {
			const AstSampleNode &sample = static_cast<const AstSampleNode &>(ast_node);
			const auto &cols = sample.cte_column_names.empty() ? children_column_lists[0] : sample.cte_column_names;
			return make_uniq<SampleNode>(my_index, cols, children_names[0], sample.sample_clause);
		}

		if (type == "Union") {
			const AstUnionNode &u = static_cast<const AstUnionNode &>(ast_node);
			if (children_names.size() == 2) {
				return make_uniq<UnionNode>(my_index, u.cte_column_names, children_names[0], children_names[1],
				                            u.is_union_all);
			}
			if (children_names.size() == 1) {
				return make_uniq<ProjectNode>(my_index, u.cte_column_names, children_names[0], children_column_lists[0],
				                              0);
			}
			// N-ary UNION: chain as left-deep binary UNIONs
			// (A UNION B UNION C) → UNION(UNION(A, B), C)
			string prev_cte_name = children_names[0];
			for (size_t ci = 1; ci < children_names.size(); ci++) {
				const string &right_cte_name = children_names[ci];
				if (ci < children_names.size() - 1) {
					// Intermediate union — create a CTE and add to cte_nodes
					size_t intermediate_index = node_count++;
					auto intermediate = make_uniq<UnionNode>(intermediate_index, u.cte_column_names, prev_cte_name,
					                                         right_cte_name, u.is_union_all);
					prev_cte_name = intermediate->cte_name;
					cte_nodes.push_back(std::move(intermediate));
				} else {
					// Final union — use the current my_index
					return make_uniq<UnionNode>(my_index, u.cte_column_names, prev_cte_name, right_cte_name,
					                            u.is_union_all);
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
			return make_uniq<DistinctNode>(my_index, cols, children_names[0]);
		}

		// Operators not yet implemented.
		throw NotImplementedException("AstFlattener: node type '%s' is not yet implemented", type);
	}

public:
	explicit AstFlattener(SqlDialect dialect = SqlDialect::DUCKDB) : dialect(dialect) {
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
			return make_uniq<CteList>(std::move(cte_nodes), std::move(insert_node), has_recursive_cte, dialect);
		}

		// Regular SELECT: FlattenNode handles the entire subtree bottom-up.
		unique_ptr<CteNode> last_cte = FlattenNode(root);

		// Build the FinalReadNode: maps CTE column names back to original names.
		vector<string> final_column_list;
		const vector<string> &cte_cols = (type == "Project")
		                                     ? static_cast<const AstProjectNode &>(root).cte_column_names
		                                     : last_cte->cte_column_list;

		for (const string &cte_col : cte_cols) {
			// cte_col = "t1_name"  →  strip "tN_" prefix to get user-visible name.
			const size_t underscore_pos = cte_col.find('_');
			if (underscore_pos != string::npos && underscore_pos + 1 < cte_col.size()) {
				final_column_list.push_back(cte_col.substr(underscore_pos + 1));
			} else {
				final_column_list.push_back(cte_col);
			}
		}

		const size_t final_index = node_count++;
		auto final_node = make_uniq<FinalReadNode>(final_index, last_cte->cte_name, last_cte->cte_column_list,
		                                           std::move(final_column_list));
		cte_nodes.push_back(std::move(last_cte));
		return make_uniq<CteList>(std::move(cte_nodes), std::move(final_node), has_recursive_cte, dialect);
	}
};

//==============================================================================
// Phase 2 entry point
//==============================================================================
unique_ptr<CteList> AstToCteList(const AstNode &root, SqlDialect dialect) {
	AstFlattener flattener(dialect);
	return flattener.Flatten(root);
}

} // namespace duckdb
