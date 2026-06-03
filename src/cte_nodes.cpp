//==============================================================================
// cte_nodes.cpp
//
// CTE node class implementations: ToQuery() for each node type, and CteList
// serialization to a SQL string.
//==============================================================================

#include "cte_nodes.hpp"
#include "lpts_helpers.hpp"
#include "lpts_debug.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/planner/operator/logical_set_operation.hpp"
#include "duckdb/planner/expression/bound_lambda_expression.hpp"
#include "duckdb/planner/expression/bound_lambdaref_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/function/lambda_functions.hpp"

namespace duckdb {

namespace {

bool GetNodeColumnsAreExpressions(const string &table_name) {
	return table_name == "(SELECT 1)";
}

} // namespace

//------------------------------------------------------------------------------
// ToQuery() implementations for each IR node type.
// Each returns the SQL fragment for its CTE body (the part inside AS (...)).
//------------------------------------------------------------------------------

/// FinalReadNode: the closing SELECT that renames CTE columns back to their
/// original names (e.g. "SELECT t2_name AS name FROM projection_2").
string FinalReadNode::ToQuery(SqlDialect dialect) {
	const size_t col_count = final_column_list.size();
	if (child_cte_column_list.size() != col_count) {
		throw InternalException("LPTS: Size mismatch between column lists");
	}
	vector<string> merged_list;
	// Assign the final column names to the CTE column names. Format: "cte_col AS final_col".
	merged_list.reserve(col_count);
	for (size_t i = 0; i < final_column_list.size(); ++i) {
		string final_name = DialectQuoteIdent(final_column_list[i], dialect);
		merged_list.emplace_back(child_cte_column_list[i] + " AS " + final_name);
	}
	std::ostringstream sql_str;
	sql_str << "SELECT ";
	sql_str << VecToSeparatedList(std::move(merged_list));
	sql_str << " FROM ";
	sql_str << child_cte_name;
	return sql_str.str();
}

string InsertNode::ToQuery(SqlDialect dialect) {
	stringstream insert_str;
	insert_str << "INSERT ";
	switch (action_type) {
	case OnConflictAction::THROW:
		break;
	case OnConflictAction::REPLACE:
	case OnConflictAction::UPDATE:
		if (dialect == SqlDialect::POSTGRES) {
			// PostgreSQL ON CONFLICT DO UPDATE requires explicit conflict columns and SET
			// clauses — metadata not available at this stage. Surface as an error.
			throw NotImplementedException(
			    "LPTS POSTGRES dialect: OR REPLACE / OR UPDATE conflict action requires "
			    "ON CONFLICT (columns) DO UPDATE SET ... syntax. "
			    "Explicit conflict columns are not available in the logical plan. "
			    "Use ON CONFLICT DO NOTHING or handle conflict resolution at the application level.");
		}
		insert_str << "OR REPLACE ";
		break;
	case OnConflictAction::NOTHING:
		if (dialect != SqlDialect::POSTGRES) {
			insert_str << "OR IGNORE ";
		}
		break;
	default:
		ThrowLptsNotImplemented("LPTS_UNSUPPORTED_OPERATOR", dialect, "on_conflict_action",
		                        EnumUtil::ToString(action_type), "InsertNode",
		                        "ON CONFLICT action is not implemented by LPTS");
	}
	insert_str << "INTO ";
	insert_str << target_table;
	insert_str << " SELECT * FROM ";
	insert_str << child_cte_name;
	if (dialect == SqlDialect::POSTGRES && action_type == OnConflictAction::NOTHING) {
		insert_str << " ON CONFLICT DO NOTHING";
	}
	return insert_str.str();
}

string CteNode::ToCteQuery(SqlDialect dialect) {
	std::ostringstream cte_str;
	cte_str << cte_name;
	if (!cte_column_list.empty()) {
		cte_str << " (";
		cte_str << VecToSeparatedList(cte_column_list);
		cte_str << ")";
	}
	cte_str << " AS (";
	cte_str << this->ToQuery(dialect);
	cte_str << ")";
	return cte_str.str();
}

string GetNode::ToQuery(SqlDialect dialect) {
	std::ostringstream get_str;
	get_str << "SELECT ";
	if (column_names.empty()) {
		get_str << "*";
	} else if (GetNodeColumnsAreExpressions(table_name)) {
		get_str << VecToSeparatedList(column_names);
	} else {
		get_str << DialectVecToQuotedIdentifierList(column_names, dialect);
	}
	get_str << " FROM ";
	if (!catalog.empty()) {
		// Fully-qualified: catalog.schema.table (DuckDB / Spark dialect)
		get_str << DialectQualifiedTableName(catalog, schema, table_name, dialect);
	} else {
		if (!input_cte_name.empty()) {
			get_str << input_cte_name << ", ";
		}
		// Unqualified: table function or simple table name
		get_str << table_name;
		// For table functions, add column aliases so renamed columns resolve correctly.
		// Skip for DuckLake functions — the _tf alias mismatches when virtual columns
		// (snapshot_id, rowid) are in the SELECT but not in the function's output schema.
		if (table_name.find('(') != string::npos && table_name != "(SELECT 1)" && !column_names.empty() &&
		    table_name.find("ducklake_table_") == string::npos) {
			idx_t alias_count = table_function_output_count == DConstants::INVALID_INDEX ? column_names.size()
			                                                                             : table_function_output_count;
			vector<string> table_function_columns;
			for (idx_t i = 0; i < alias_count && i < column_names.size(); i++) {
				table_function_columns.push_back(DialectQuoteIdent(column_names[i], dialect));
			}
			get_str << " _tf(" << VecToSeparatedList(table_function_columns) << ")";
		}
	}
	if (!table_filters.empty()) {
		get_str << " WHERE ";
		get_str << VecToSeparatedList(table_filters, " AND ");
	}
	return get_str.str();
}

string FilterNode::ToQuery(SqlDialect dialect) {
	std::ostringstream get_str;
	// Use explicit column list so COLUMN_LIFETIME projection_map pruning is
	// respected: SELECT * would expose more columns than the CTE header declares.
	get_str << "SELECT " << VecToSeparatedList(cte_column_list) << " FROM ";
	get_str << child_cte_name;
	if (!conditions.empty()) {
		get_str << " WHERE ";
		// Wrap each condition in parentheses to preserve precedence
		// when conditions containing OR are ANDed together.
		for (size_t i = 0; i < conditions.size(); i++) {
			if (i > 0) {
				get_str << " AND ";
			}
			get_str << "(" << conditions[i] << ")";
		}
	}
	return get_str.str();
}

string ProjectNode::ToQuery(SqlDialect dialect) {
	std::ostringstream project_str;
	project_str << "SELECT ";
	if (column_names.empty()) {
		project_str << "*";
	} else {
		project_str << VecToSeparatedList(column_names);
	}
	project_str << " FROM ";
	project_str << child_cte_name;
	return project_str.str();
}

string AggregateNode::ToQuery(SqlDialect dialect) {
	std::ostringstream aggregate_str;
	aggregate_str << "SELECT ";
	if (!group_by_columns.empty()) {
		aggregate_str << VecToSeparatedList(group_by_columns);
		aggregate_str << ", ";
	}
	aggregate_str << VecToSeparatedList(aggregate_expressions);
	aggregate_str << " FROM ";
	aggregate_str << child_cte_name;
	if (!group_by_clause.empty()) {
		aggregate_str << " GROUP BY ";
		aggregate_str << group_by_clause;
	}
	return aggregate_str.str();
}

string JoinNode::ToQuery(SqlDialect dialect) {
	std::ostringstream join_str;
	// Use explicit column list instead of SELECT * to avoid including
	// duplicate join key columns from both sides of the join.
	// For MARK→LEFT joins, the last column is a computed mark expression.
	if (!mark_expression.empty() && !cte_column_list.empty()) {
		vector<string> select_cols(cte_column_list.begin(), cte_column_list.end() - 1);
		select_cols.push_back(mark_expression);
		join_str << "SELECT " << VecToSeparatedList(select_cols) << " FROM ";
	} else {
		join_str << "SELECT " << VecToSeparatedList(cte_column_list) << " FROM ";
	}
	// RIGHT_SEMI / RIGHT_ANTI: the preserved (output) side is the RIGHT CTE.
	// Emit as "right SEMI/ANTI JOIN left" so the preserved side is on the left in SQL.
	// cte_column_list already contains only the preserved side's columns (from GetColumnBindings).
	if (join_type == JoinType::RIGHT_SEMI || join_type == JoinType::RIGHT_ANTI) {
		join_str << right_cte_name << " ";
		join_str << (join_type == JoinType::RIGHT_SEMI ? "SEMI" : "ANTI");
		join_str << " JOIN " << left_cte_name;
		join_str << " ON " << JoinConditionsToSQL(join_conditions);
		return join_str.str();
	}

	join_str << left_cte_name;
	join_str << " ";
	switch (join_type) {
	case JoinType::INNER:
	case JoinType::LEFT:
	case JoinType::RIGHT:
	case JoinType::OUTER:
	case JoinType::SEMI:
	case JoinType::ANTI:
		join_str << EnumUtil::ToString(join_type);
		break;
	case JoinType::SINGLE:
		// DuckDB uses SINGLE joins for scalar subqueries. They behave like a
		// LEFT join when the RHS has at most one matching row per LHS row.
		join_str << "LEFT";
		break;
	default:
		ThrowLptsNotImplemented("LPTS_UNSUPPORTED_JOIN_TYPE", dialect, "join_type", EnumUtil::ToString(join_type),
		                        "JoinNode", "join type is not implemented by LPTS");
	}
	join_str << " JOIN ";
	// MARK→LEFT joins: deduplicate the right side to prevent left-row multiplication
	// when the RHS has duplicate matching values. IN subquery semantics treat the RHS
	// as a set, so (SELECT DISTINCT * FROM rhs) preserves correctness.
	if (!mark_expression.empty()) {
		LPTS_DEBUG_PRINT("[LPTS-CTE] MARK join: wrapping right CTE '" + right_cte_name +
		                 "' in SELECT DISTINCT to prevent duplicate rows");
		join_str << "(SELECT DISTINCT * FROM " << right_cte_name << ") AS _rhs_dedup";
	} else {
		join_str << right_cte_name;
	}
	join_str << " ON ";
	join_str << JoinConditionsToSQL(join_conditions);
	return join_str.str();
}

string UnionNode::ToQuery(SqlDialect dialect) {
	std::ostringstream union_str;
	union_str << "SELECT * FROM ";
	union_str << left_cte_name;
	if (is_union_all) {
		union_str << " UNION ALL ";
	} else {
		union_str << " UNION ";
	}
	union_str << "SELECT * FROM ";
	union_str << right_cte_name;
	return union_str.str();
}

string ExceptNode::ToQuery(SqlDialect dialect) {
	std::ostringstream except_str;
	except_str << "SELECT * FROM ";
	except_str << left_cte_name;
	if (is_except_all) {
		except_str << " EXCEPT ALL ";
	} else {
		except_str << " EXCEPT ";
	}
	except_str << "SELECT * FROM ";
	except_str << right_cte_name;
	return except_str.str();
}

string CteSetOperationNode::ToQuery(SqlDialect dialect) {
	std::ostringstream setop_str;
	setop_str << "SELECT * FROM ";
	setop_str << left_cte_name;
	setop_str << " " << op_name;
	if (is_all) {
		setop_str << " ALL";
	}
	setop_str << " SELECT * FROM ";
	setop_str << right_cte_name;
	return setop_str.str();
}

string OrderNode::ToQuery(SqlDialect dialect) {
	std::ostringstream order_str;
	// Use explicit column list so COLUMN_LIFETIME projection_map pruning is
	// respected: SELECT * would expose more columns than the CTE header declares.
	order_str << "SELECT " << VecToSeparatedList(cte_column_list) << " FROM " << child_cte_name;
	if (!order_items.empty()) {
		order_str << " ORDER BY " << VecToSeparatedList(order_items);
	}
	return order_str.str();
}

string LimitNode::ToQuery(SqlDialect dialect) {
	std::ostringstream limit_str_stream;
	limit_str_stream << "SELECT " << VecToSeparatedList(cte_column_list) << " FROM " << child_cte_name;
	if (!limit_str.empty()) {
		limit_str_stream << " LIMIT ";
		if (limit_needs_child_scalar) {
			limit_str_stream << "(SELECT first(" << limit_str << ") FROM " << child_cte_name << ")";
		} else {
			limit_str_stream << limit_str;
		}
	}
	if (!offset_str.empty()) {
		limit_str_stream << " OFFSET ";
		if (offset_needs_child_scalar) {
			limit_str_stream << "(SELECT first(" << offset_str << ") FROM " << child_cte_name << ")";
		} else {
			limit_str_stream << offset_str;
		}
	}
	return limit_str_stream.str();
}

string TopNNode::ToQuery(SqlDialect dialect) {
	std::ostringstream ss;
	ss << "SELECT " << VecToSeparatedList(cte_column_list) << " FROM " << child_cte_name;
	if (!order_items.empty()) {
		ss << " ORDER BY " << VecToSeparatedList(order_items);
	}
	if (limit > 0) {
		ss << " LIMIT " << limit;
	}
	if (offset > 0) {
		ss << " OFFSET " << offset;
	}
	return ss.str();
}

string DistinctNode::ToQuery(SqlDialect dialect) {
	std::ostringstream distinct_str;
	distinct_str << "SELECT DISTINCT " << VecToSeparatedList(cte_column_list) << " FROM " << child_cte_name;
	return distinct_str.str();
}

string DelimGetNode::ToQuery(SqlDialect dialect) {
	std::ostringstream s;
	s << "SELECT DISTINCT ";
	if (source_cols.empty()) {
		s << "*";
	} else {
		s << VecToSeparatedList(source_cols);
	}
	s << " FROM " << source_cte_name;
	return s.str();
}

string RecursiveCteNode::ToQuery(SqlDialect dialect) {
	string union_kw = union_all ? "\nUNION ALL\n" : "\nUNION\n";
	return "SELECT " + VecToSeparatedList(anchor_cols) + " FROM " + anchor_cte_name + union_kw + recursive_step_sql;
}

/// Serialize the entire CTE list into a SQL string.
/// Output format: WITH cte_0(...) AS (...), cte_1(...) AS (...), ... SELECT ...;
string CteList::ToQuery(const bool use_newlines, const vector<string> &output_names) {
	// Override final column aliases if output_names are provided
	if (!output_names.empty()) {
		auto *fr = dynamic_cast<FinalReadNode *>(final_node.get());
		if (fr) {
			for (size_t i = 0; i < output_names.size() && i < fr->final_column_list.size(); i++) {
				if (!output_names[i].empty()) {
					fr->final_column_list[i] = output_names[i];
				}
			}
		}
	}

	std::ostringstream sql_str;
	if (!nodes.empty()) {
		sql_str << (has_recursive_cte ? "WITH RECURSIVE " : "WITH ");
		for (size_t i = 0; i < nodes.size(); ++i) {
			sql_str << nodes[i]->ToCteQuery(dialect);
			if (i != nodes.size() - 1) {
				sql_str << ", ";
			} else if (!use_newlines) {
				sql_str << " ";
			}
			if (use_newlines) {
				sql_str << "\n";
			}
		}
	}
	sql_str << final_node->ToQuery(dialect);
	sql_str << ";";
	return sql_str.str();
}

} // namespace duckdb
