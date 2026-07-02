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

// Render a scan's projected columns: plain identifiers are dialect-quoted; entries flagged in `is_expr`
// (struct field-extraction pushdowns like `struct_extract(c, 'a')`) are emitted verbatim. `is_expr` may be
// empty (or shorter than `column_names`), in which case the missing entries are treated as identifiers.
vector<string> RenderGetSelectColumns(const vector<string> &column_names, const vector<bool> &is_expr,
                                      SqlDialect dialect) {
	vector<string> out;
	out.reserve(column_names.size());
	for (size_t i = 0; i < column_names.size(); i++) {
		const bool raw = i < is_expr.size() && is_expr[i];
		out.push_back(raw ? column_names[i] : DialectQuoteIdent(column_names[i], dialect));
	}
	return out;
}

void RequireDuckDBDialect(SqlDialect dialect, const string &node_name, const string &feature) {
	if (dialect == SqlDialect::DUCKDB) {
		return;
	}
	throw NotImplementedException("LPTS_UNSUPPORTED_DIALECT_FEATURE: %s: %s is only implemented for the DuckDB dialect",
	                              node_name, feature);
}

//------------------------------------------------------------------------------
// Pretty-printer helpers.
//
// A SELECT block is laid out with each clause keyword left-aligned at column `pos` and the
// expression area starting at `pos + 8` (or `pos + 10` when GROUP BY / ORDER BY is present, so the
// 8-char keywords still fit). Comma/AND-separated lists wrap when they would exceed `MAX_WIDTH`,
// continuing aligned at the expression-area column. A single expression is never broken internally.
//------------------------------------------------------------------------------
constexpr size_t MAX_WIDTH = 100;
constexpr size_t INDENT_WIDTH = 4;

/// An ORDER BY item whose expression is a bare integer literal (e.g. a substituted constant projection,
/// `ORDER BY 10 ASC`) would be re-read as an ORDINAL position — and DuckDB constant-folds even `(10)`
/// back to an ordinal, while non-integer literals are rejected outright. Ordering by a constant is a
/// semantic no-op (every row compares equal), so such items are DROPPED.
bool IsConstantIntegerOrderItem(const string &item) {
	size_t p = 0;
	if (p < item.size() && item[p] == '-') {
		p++;
	}
	const size_t digits_start = p;
	while (p < item.size() && isdigit(static_cast<unsigned char>(item[p]))) {
		p++;
	}
	if (p == digits_start || (p != item.size() && item[p] != ' ')) {
		return false; // not a bare leading integer
	}
	// The remainder must be only order modifiers, not an operator continuing the expression.
	const string rest = item.substr(p);
	return rest.empty() || rest.rfind(" ASC", 0) == 0 || rest.rfind(" DESC", 0) == 0 || rest.rfind(" NULLS", 0) == 0;
}

vector<string> GuardOrdinalOrderItems(const vector<string> &items) {
	vector<string> out;
	out.reserve(items.size());
	for (const auto &it : items) {
		if (!IsConstantIntegerOrderItem(it)) {
			out.push_back(it);
		}
	}
	return out;
}

/// Single-line tail (" FROM ... [WHERE ...] [GROUP BY ...] [HAVING ...] [ORDER BY ...] [LIMIT/OFFSET]").
string RenderTail(const SelectParts &p) {
	std::ostringstream t;
	t << " FROM " << p.from;
	if (!p.where_conds.empty()) {
		t << " WHERE ";
		for (size_t i = 0; i < p.where_conds.size(); i++) {
			t << (i ? " AND " : "") << "(" << p.where_conds[i] << ")";
		}
	}
	if (!p.group_by.empty()) {
		t << " GROUP BY " << p.group_by;
	}
	if (!p.having_conds.empty()) {
		t << " HAVING ";
		for (size_t i = 0; i < p.having_conds.size(); i++) {
			t << (i ? " AND " : "") << "(" << p.having_conds[i] << ")";
		}
	}
	const vector<string> tail_order_items = GuardOrdinalOrderItems(p.order_items);
	if (!tail_order_items.empty()) {
		t << " ORDER BY " << VecToSeparatedList(tail_order_items);
	}
	if (!p.limit.empty()) {
		t << " LIMIT " << p.limit;
	}
	if (!p.offset.empty()) {
		t << " OFFSET " << p.offset;
	}
	return t.str();
}

/// Append `items` joined by `sep`, breaking to a new line indented to `indent` columns whenever the
/// running line would exceed MAX_WIDTH (but always after at least one item). `col` tracks/returns the
/// current column.
void AppendWrapped(std::ostringstream &s, size_t &col, const vector<string> &items, const string &sep, size_t indent) {
	for (size_t i = 0; i < items.size(); i++) {
		if (i == 0) {
			s << items[i];
			col += items[i].size();
			continue;
		}
		// Reserve one char for the trailing separator that lands on this line if the next item wraps,
		// so a "..., item," line still fits within MAX_WIDTH.
		const size_t reserve = (i + 1 < items.size()) ? 1 : 0;
		if (col + sep.size() + items[i].size() + reserve <= MAX_WIDTH) {
			s << sep << items[i];
			col += sep.size() + items[i].size();
		} else {
			string trimmed = sep;
			while (!trimmed.empty() && trimmed.back() == ' ') {
				trimmed.pop_back();
			}
			s << trimmed << "\n" << string(indent, ' ') << items[i];
			col = indent + items[i].size();
		}
	}
}

/// Emit one clause line: "<pos spaces><keyword><pad><prefix><wrapped items>".
void EmitClauseList(std::ostringstream &s, size_t pos, size_t expr_col, const string &keyword, const string &prefix,
                    const vector<string> &items, const string &sep) {
	s << string(pos, ' ') << keyword;
	const size_t kwend = pos + keyword.size();
	size_t col = expr_col;
	if (kwend < expr_col) {
		s << string(expr_col - kwend, ' ');
	} else {
		s << ' ';
		col = kwend + 1;
	}
	if (!prefix.empty()) {
		s << prefix;
		col += prefix.size();
	}
	AppendWrapped(s, col, items, sep, expr_col);
}

/// Emit one clause line carrying a single (non-wrapped) value.
void EmitClauseValue(std::ostringstream &s, size_t pos, size_t expr_col, const string &keyword, const string &value) {
	s << string(pos, ' ') << keyword;
	const size_t kwend = pos + keyword.size();
	s << (kwend < expr_col ? string(expr_col - kwend, ' ') : string(" "));
	s << value;
}

/// Pretty-print a SELECT block at indent `pos`. When `out_names` is empty the select expressions are
/// printed as-is (the enclosing CTE header names the columns); otherwise each is aliased to its output
/// name (a redundant "x AS x" is dropped).
string RenderSelectPretty(const SelectParts &p, const vector<string> &out_names, size_t pos, SqlDialect dialect) {
	const size_t expr_col = pos + ((!p.group_by.empty() || !p.order_items.empty()) ? 10 : 8);
	vector<string> select_items;
	select_items.reserve(p.select_exprs.size());
	for (size_t i = 0; i < p.select_exprs.size(); i++) {
		if (out_names.empty()) {
			select_items.push_back(p.select_exprs[i]);
		} else {
			const string nm = DialectQuoteIdent(out_names[i], dialect);
			select_items.push_back(p.select_exprs[i] == nm ? nm : (p.select_exprs[i] + " AS " + nm));
		}
	}
	std::ostringstream s;
	EmitClauseList(s, pos, expr_col, "SELECT", p.distinct ? "DISTINCT " : "", select_items, ", ");
	s << "\n";
	EmitClauseValue(s, pos, expr_col, "FROM", p.from);
	if (!p.where_conds.empty()) {
		vector<string> w;
		for (const auto &c : p.where_conds) {
			w.push_back("(" + c + ")");
		}
		s << "\n";
		EmitClauseList(s, pos, expr_col, "WHERE", "", w, " AND ");
	}
	if (!p.group_by.empty()) {
		s << "\n";
		EmitClauseValue(s, pos, expr_col, "GROUP BY", p.group_by);
	}
	if (!p.having_conds.empty()) {
		vector<string> h;
		for (const auto &c : p.having_conds) {
			h.push_back("(" + c + ")");
		}
		s << "\n";
		EmitClauseList(s, pos, expr_col, "HAVING", "", h, " AND ");
	}
	const vector<string> pretty_order_items = GuardOrdinalOrderItems(p.order_items);
	if (!pretty_order_items.empty()) {
		s << "\n";
		EmitClauseList(s, pos, expr_col, "ORDER BY", "", pretty_order_items, ", ");
	}
	if (!p.limit.empty()) {
		s << "\n";
		EmitClauseValue(s, pos, expr_col, "LIMIT", p.limit);
	}
	if (!p.offset.empty()) {
		s << "\n";
		EmitClauseValue(s, pos, expr_col, "OFFSET", p.offset);
	}
	return s.str();
}

/// Pretty-print a CTE header: "<name> (<wrapped column list>) AS (".
string RenderCteHeader(const string &name, const vector<string> &cols) {
	std::ostringstream s;
	s << name;
	if (!cols.empty()) {
		// Quote where needed: generated t{N}_ names pass through unchanged, but a recursive CTE header
		// carries user-facing (stripped) names that can be reserved words ("begin", "end").
		vector<string> quoted;
		quoted.reserve(cols.size());
		for (const auto &c : cols) {
			quoted.push_back(QuoteIdentifier(c));
		}
		s << " (";
		size_t col = name.size() + 2; // after "<name> ("
		AppendWrapped(s, col, quoted, ", ", name.size() + 2);
		s << ")";
	}
	s << " AS (";
	return s.str();
}

/// Apply a column-token rename map to every string in a SelectParts (in place).
void RenameParts(SelectParts &p, const unordered_map<string, string> &m) {
	if (m.empty()) {
		return;
	}
	for (auto &e : p.select_exprs) {
		e = SubstituteColumnTokens(e, m);
	}
	p.from = SubstituteColumnTokens(p.from, m);
	for (auto &c : p.where_conds) {
		c = SubstituteColumnTokens(c, m);
	}
	p.group_by = SubstituteColumnTokens(p.group_by, m);
	for (auto &c : p.having_conds) {
		c = SubstituteColumnTokens(c, m);
	}
	for (auto &o : p.order_items) {
		o = SubstituteColumnTokens(o, m);
	}
	p.limit = SubstituteColumnTokens(p.limit, m);
	p.offset = SubstituteColumnTokens(p.offset, m);
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
		// A redundant "x AS x" (when the column already matches the final name) is collapsed
		// globally in CteList::ToQuery via SwallowSelfAlias.
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
			    "LPTS_UNSUPPORTED_DIALECT_FEATURE: POSTGRES OR REPLACE / OR UPDATE conflict action requires "
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

bool CteNode::SelectListAndTail(SqlDialect dialect, vector<string> &items, string &tail, bool &distinct) const {
	SelectParts p;
	if (!BuildSelectParts(dialect, p)) {
		return false;
	}
	items = p.select_exprs;
	distinct = p.distinct;
	tail = RenderTail(p);
	return true;
}

string GetNode::ToQuery(SqlDialect dialect) {
	std::ostringstream get_str;
	get_str << "SELECT ";
	if (column_names.empty()) {
		get_str << "*";
	} else if (GetNodeColumnsAreExpressions(table_name)) {
		get_str << VecToSeparatedList(column_names);
	} else {
		get_str << VecToSeparatedList(RenderGetSelectColumns(column_names, column_is_expression, dialect));
	}
	get_str << " FROM ";
	if (!catalog.empty()) {
		// Fully-qualified: catalog.schema.table (DuckDB / Spark dialect)
		get_str << DialectQualifiedTableName(catalog, schema, table_name, dialect);
	} else {
		// A TABLE-argument function: the child CTE is the function's argument, not a lateral input.
		const size_t table_arg_pos = table_name.find("%LPTS_TABLE_ARG%");
		if (table_arg_pos != string::npos && !input_cte_name.empty()) {
			string fn = table_name;
			fn.replace(table_arg_pos, string("%LPTS_TABLE_ARG%").size(), "(SELECT * FROM " + input_cte_name + ")");
			get_str << fn;
		} else {
			if (!input_cte_name.empty()) {
				get_str << input_cte_name << ", ";
			}
			// Unqualified: table function or simple table name
			get_str << table_name;
		}
		// For table functions, add column aliases so renamed columns resolve correctly.
		// Skip for DuckLake functions — the _tf alias mismatches when virtual columns
		// (snapshot_id, rowid) are in the SELECT but not in the function's output schema.
		if (table_name.find('(') != string::npos && table_name != "(SELECT 1)" && !column_names.empty() &&
		    table_name.find("ducklake_table_") == string::npos) {
			vector<string> table_function_columns;
			if (!table_function_alias.empty()) {
				// Output-ordered alias (read_csv/read_parquet/range/...): names every output column at its
				// true position so the SELECT (which references the names) resolves correctly.
				for (const string &name : table_function_alias) {
					table_function_columns.push_back(DialectQuoteIdent(name, dialect));
				}
			} else {
				idx_t alias_count = table_function_output_count == DConstants::INVALID_INDEX
				                        ? column_names.size()
				                        : table_function_output_count;
				for (idx_t i = 0; i < alias_count && i < column_names.size(); i++) {
					table_function_columns.push_back(DialectQuoteIdent(column_names[i], dialect));
				}
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

bool GetNode::BuildSelectParts(SqlDialect dialect, SelectParts &out) const {
	// Only the simple physical-table case (explicit columns, no table function / "(SELECT 1)" /
	// table-function input CTE). Everything else keeps GetNode::ToQuery's single-line rendering.
	if (column_names.empty() || GetNodeColumnsAreExpressions(table_name) || !input_cte_name.empty() ||
	    table_name.find('(') != string::npos) {
		return false;
	}
	out.select_exprs = RenderGetSelectColumns(column_names, column_is_expression, dialect);
	out.from = catalog.empty() ? table_name : DialectQualifiedTableName(catalog, schema, table_name, dialect);
	out.where_conds = table_filters; // already complete conditions; the renderer wraps each in parens
	return true;
}

bool FilterNode::BuildSelectParts(SqlDialect dialect, SelectParts &out) const {
	(void)dialect;
	// Use explicit column list so COLUMN_LIFETIME projection_map pruning is
	// respected: SELECT * would expose more columns than the CTE header declares.
	out.select_exprs = cte_column_list;
	out.from = child_cte_name;
	out.where_conds = conditions;
	return true;
}

string FilterNode::ToQuery(SqlDialect dialect) {
	vector<string> items;
	string tail;
	bool distinct;
	SelectListAndTail(dialect, items, tail, distinct);
	return "SELECT " + VecToSeparatedList(items) + tail;
}

bool ProjectNode::BuildSelectParts(SqlDialect dialect, SelectParts &out) const {
	(void)dialect;
	if (column_names.empty()) {
		return false; // SELECT * — cannot positionally alias.
	}
	out.select_exprs = column_names;
	out.from = child_cte_name;
	return true;
}

string ProjectNode::ToQuery(SqlDialect dialect) {
	vector<string> items;
	string tail;
	bool distinct;
	if (SelectListAndTail(dialect, items, tail, distinct)) {
		return "SELECT " + VecToSeparatedList(items) + tail;
	}
	return "SELECT * FROM " + child_cte_name;
}

bool AggregateNode::BuildSelectParts(SqlDialect dialect, SelectParts &out) const {
	(void)dialect;
	out.select_exprs.reserve(group_by_columns.size() + aggregate_expressions.size());
	for (const auto &g : group_by_columns) {
		out.select_exprs.push_back(g);
	}
	for (const auto &a : aggregate_expressions) {
		out.select_exprs.push_back(a);
	}
	out.from = child_cte_name;
	out.group_by = group_by_clause;
	return true;
}

string AggregateNode::ToQuery(SqlDialect dialect) {
	vector<string> items;
	string tail;
	bool distinct;
	SelectListAndTail(dialect, items, tail, distinct);
	return "SELECT " + VecToSeparatedList(items) + tail;
}

string JoinNode::ToQuery(SqlDialect dialect) {
	if (is_asof) {
		RequireDuckDBDialect(dialect, "JoinNode", "ASOF JOIN");
		if (join_type != JoinType::INNER && join_type != JoinType::LEFT) {
			throw NotImplementedException("LPTS_UNSUPPORTED_JOIN_TYPE: ASOF join type %s is not implemented",
			                              EnumUtil::ToString(join_type));
		}
	}
	// IN/ANY/ALL mark join, single NULL-propagating comparison (keys captured): render as a correlated
	// EXISTS in the SELECT. This produces exactly one row per LHS row (a LEFT join on an inequality could
	// match several RHS rows and multiply the LHS row, which is wrong for the IN/ANY directions that keep
	// the mark), and builds a 3-valued mark: TRUE if a row matches; else NULL if the comparison is
	// indeterminate (the RHS key has a NULL, or the LHS key is NULL and the RHS is non-empty); else FALSE.
	// Multi-condition mark join with NO equality condition: executed as a nested-loop join whose
	// conditions match INDEPENDENTLY — found iff ANY condition matches ANY rhs row (e.g. the decomposed
	// row `!=` of `x != ANY(...)` / `= ALL(...)`, where per-row OR distributes over the existential).
	// NULL semantics mirror PhysicalJoin::ConstructMarkJoinResult: a NULL in any LHS key → NULL (this
	// overrides a match); else found → TRUE; else a NULL in any RHS key → NULL; else FALSE.
	if (!mark_expression.empty() && mark_membership_comparisons.size() > 1 && !mark_join_has_equality) {
		auto or_list = [](const vector<string> &items) {
			string out;
			for (size_t i = 0; i < items.size(); i++) {
				out += (i ? " OR " : "") + items[i];
			}
			return out;
		};
		vector<string> lhs_nulls, rhs_nulls;
		for (const auto &l : mark_membership_lhs) {
			lhs_nulls.push_back("(" + l + ") IS NULL");
		}
		for (const auto &r : mark_membership_rhs) {
			rhs_nulls.push_back("(" + r + ") IS NULL");
		}
		const string matched =
		    "EXISTS (SELECT 1 FROM " + right_cte_name + " WHERE " + or_list(mark_membership_comparisons) + ")";
		const string rhs_has_null = "EXISTS (SELECT 1 FROM " + right_cte_name + " WHERE " + or_list(rhs_nulls) + ")";
		const string mark = "CASE WHEN " + or_list(lhs_nulls) + " THEN NULL WHEN " + matched + " THEN TRUE WHEN " +
		                    rhs_has_null + " THEN NULL ELSE FALSE END";
		vector<string> select_cols(cte_column_list.begin(), cte_column_list.end() - 1);
		select_cols.push_back(mark);
		return "SELECT " + VecToSeparatedList(select_cols) + " FROM " + left_cte_name;
	}
	if (!mark_expression.empty() && !mark_membership_conditions.empty()) {
		// matched: a correlated row satisfies every join condition (all membership comparisons AND any
		// correlation keys).
		const string matched =
		    "EXISTS (SELECT 1 FROM " + right_cte_name + " WHERE " + JoinConditionsToSQL(join_conditions) + ")";
		// indeterminate: no TRUE match, but a *correlated* row exists whose per-row membership predicate is
		// NULL — i.e. every membership comparison is not-definitely-false (holds or has a NULL operand). The
		// null-safe correlation keys still gate which rows count. AND-ing one clause per comparison handles
		// single- and multi-column IN/ANY/ALL alike (matched rows are caught by the TRUE branch first). This
		// unifies the uncorrelated case (no correlation conditions) and the correlated case.
		vector<string> indeterminate_conds = mark_correlation_conditions;
		for (const auto &mc : mark_membership_conditions) {
			indeterminate_conds.push_back(mc);
		}
		const string indeterminate =
		    "EXISTS (SELECT 1 FROM " + right_cte_name + " WHERE " + JoinConditionsToSQL(indeterminate_conds) + ")";
		const string mark = "CASE WHEN " + matched + " THEN TRUE WHEN " + indeterminate + " THEN NULL ELSE FALSE END";
		vector<string> select_cols(cte_column_list.begin(), cte_column_list.end() - 1);
		select_cols.push_back(mark);
		return "SELECT " + VecToSeparatedList(select_cols) + " FROM " + left_cte_name;
	}
	std::ostringstream join_str;
	// Other MARK joins (EXISTS, correlated/DELIM, multi-condition) render as LEFT JOIN + a 2-valued mark.
	// The mark must express "did the RHS have a matching row?". A key-based `(rhs_key IS NOT NULL)` is WRONG
	// when the correlation key can itself be NULL and legitimately matches (`NULL IS NOT DISTINCT FROM NULL`):
	// the matched rhs_key is NULL, so `IS NOT NULL` reads false even though a row matched. Instead attach a
	// non-null sentinel (`TRUE AS _lpts_matched`) to the deduped RHS and test THAT — it is NULL only when the
	// LEFT JOIN found no partner, regardless of the join key's nullability. (The `true` mark — an EXISTS with
	// no join condition — needs no sentinel.)
	const bool use_match_sentinel = !mark_expression.empty() && mark_expression != "true";
	if (!mark_expression.empty() && !cte_column_list.empty()) {
		vector<string> select_cols(cte_column_list.begin(), cte_column_list.end() - 1);
		select_cols.push_back(use_match_sentinel ? "(_rhs_dedup._lpts_matched IS NOT NULL)" : mark_expression);
		join_str << "SELECT " << VecToSeparatedList(select_cols) << " FROM ";
	} else {
		join_str << "SELECT " << VecToSeparatedList(cte_column_list) << " FROM ";
	}
	// RIGHT_SEMI / RIGHT_ANTI: the preserved (output) side is the RIGHT CTE.
	// Emit as "right SEMI/ANTI JOIN left" so the preserved side is on the left in SQL.
	// cte_column_list already contains only the preserved side's columns (from GetColumnBindings).
	if (join_type == JoinType::RIGHT_SEMI || join_type == JoinType::RIGHT_ANTI) {
		if (is_asof) {
			throw NotImplementedException("LPTS_UNSUPPORTED_JOIN_TYPE: ASOF RIGHT_SEMI/RIGHT_ANTI are not implemented");
		}
		join_str << right_cte_name << " ";
		join_str << (join_type == JoinType::RIGHT_SEMI ? "SEMI" : "ANTI");
		join_str << " JOIN " << left_cte_name;
		join_str << " ON " << JoinConditionsToSQL(join_conditions);
		return join_str.str();
	}

	join_str << left_cte_name;
	join_str << " ";
	if (is_asof) {
		join_str << "ASOF ";
	}
	switch (join_type) {
	case JoinType::INNER:
		if (!is_asof) {
			join_str << EnumUtil::ToString(join_type);
		}
		break;
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
	// Deduplicate the right side so a left row isn't multiplied by duplicate RHS rows. MARK→LEFT joins need
	// this because IN/EXISTS treat the RHS as a set; SINGLE joins (scalar subqueries) need it because the
	// subquery must yield at most one row per outer row, but a decorrelated non-aggregated subquery can
	// carry duplicate rows per correlation key.
	if (use_match_sentinel) {
		// Non-null match sentinel (see above): NULL after the LEFT JOIN iff no RHS partner.
		join_str << "(SELECT DISTINCT *, TRUE AS _lpts_matched FROM " << right_cte_name << ") AS _rhs_dedup";
	} else if (!mark_expression.empty() || join_type == JoinType::SINGLE) {
		join_str << "(SELECT DISTINCT * FROM " << right_cte_name << ") AS _rhs_dedup";
	} else {
		join_str << right_cte_name;
	}
	join_str << " ON ";
	join_str << JoinConditionsToSQL(join_conditions);
	return join_str.str();
}

bool JoinNode::BuildSelectParts(SqlDialect dialect, SelectParts &out) const {
	(void)dialect;
	// MARK→LEFT, RIGHT_SEMI/ANTI, ASOF and SINGLE joins have bespoke rendering (SINGLE deduplicates its
	// RHS in ToQuery) — keep those.
	if (!mark_expression.empty() || is_asof || join_type == JoinType::RIGHT_SEMI || join_type == JoinType::RIGHT_ANTI ||
	    join_type == JoinType::SINGLE) {
		return false;
	}
	std::ostringstream f;
	f << left_cte_name << " ";
	switch (join_type) {
	case JoinType::INNER:
	case JoinType::LEFT:
	case JoinType::RIGHT:
	case JoinType::OUTER:
	case JoinType::SEMI:
	case JoinType::ANTI:
		f << EnumUtil::ToString(join_type);
		break;
	case JoinType::SINGLE:
		f << "LEFT";
		break;
	default:
		return false;
	}
	f << " JOIN " << right_cte_name << " ON " << JoinConditionsToSQL(join_conditions);
	out.select_exprs = cte_column_list;
	out.from = f.str();
	return true;
}

string PositionalJoinNode::ToQuery(SqlDialect dialect) {
	RequireDuckDBDialect(dialect, "PositionalJoinNode", "POSITIONAL JOIN");
	std::ostringstream join_str;
	join_str << "SELECT " << VecToSeparatedList(cte_column_list) << " FROM ";
	join_str << left_cte_name << " POSITIONAL JOIN " << right_cte_name;
	return join_str.str();
}

bool SampleNode::BuildSelectParts(SqlDialect dialect, SelectParts &out) const {
	(void)dialect;
	out.select_exprs = cte_column_list;
	out.from = child_cte_name + " USING SAMPLE " + sample_clause;
	return true;
}

string SampleNode::ToQuery(SqlDialect dialect) {
	RequireDuckDBDialect(dialect, "SampleNode", "USING SAMPLE");
	vector<string> items;
	string tail;
	bool distinct;
	SelectListAndTail(dialect, items, tail, distinct);
	return "SELECT " + VecToSeparatedList(items) + tail;
}

string UnionNode::ToQuery(SqlDialect dialect) {
	// A branch that exposes MORE columns than the union's arity (e.g. an inner ORDER BY key kept as an
	// extra projected column) must select only the first arity-many columns — `SELECT *` would make the
	// two sides' column counts differ.
	const size_t arity = cte_column_list.size();
	auto branch_select = [&](const vector<string> &branch_cols) {
		if (arity > 0 && branch_cols.size() > arity) {
			vector<string> first_cols(branch_cols.begin(), branch_cols.begin() + arity);
			return "SELECT " + VecToSeparatedList(first_cols) + " FROM ";
		}
		return string("SELECT * FROM ");
	};
	std::ostringstream union_str;
	union_str << branch_select(left_columns) << left_cte_name;
	if (is_union_all) {
		union_str << " UNION ALL ";
	} else {
		union_str << " UNION ";
	}
	union_str << branch_select(right_columns) << right_cte_name;
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
	setop_str << "SELECT " << VecToSeparatedList(left_select_columns) << " FROM ";
	setop_str << left_cte_name;
	setop_str << " " << op_name;
	if (is_all) {
		setop_str << " ALL";
	}
	setop_str << " SELECT " << VecToSeparatedList(right_select_columns) << " FROM ";
	setop_str << right_cte_name;
	return setop_str.str();
}

bool OrderNode::BuildSelectParts(SqlDialect dialect, SelectParts &out) const {
	(void)dialect;
	// Use explicit column list so COLUMN_LIFETIME projection_map pruning is
	// respected: SELECT * would expose more columns than the CTE header declares.
	out.select_exprs = cte_column_list;
	out.from = child_cte_name;
	out.order_items = order_items;
	return true;
}

string OrderNode::ToQuery(SqlDialect dialect) {
	vector<string> items;
	string tail;
	bool distinct;
	SelectListAndTail(dialect, items, tail, distinct);
	return "SELECT " + VecToSeparatedList(items) + tail;
}

bool LimitNode::BuildSelectParts(SqlDialect dialect, SelectParts &out) const {
	(void)dialect;
	out.select_exprs = cte_column_list;
	out.from = child_cte_name;
	if (!limit_str.empty()) {
		out.limit =
		    limit_needs_child_scalar ? ("(SELECT first(" + limit_str + ") FROM " + child_cte_name + ")") : limit_str;
	}
	if (!offset_str.empty()) {
		out.offset =
		    offset_needs_child_scalar ? ("(SELECT first(" + offset_str + ") FROM " + child_cte_name + ")") : offset_str;
	}
	return true;
}

string LimitNode::ToQuery(SqlDialect dialect) {
	vector<string> items;
	string tail;
	bool distinct;
	SelectListAndTail(dialect, items, tail, distinct);
	return "SELECT " + VecToSeparatedList(items) + tail;
}

bool TopNNode::BuildSelectParts(SqlDialect dialect, SelectParts &out) const {
	(void)dialect;
	out.select_exprs = cte_column_list;
	out.from = child_cte_name;
	out.order_items = order_items;
	if (limit > 0) {
		out.limit = std::to_string(limit);
	}
	if (offset > 0) {
		out.offset = std::to_string(offset);
	}
	return true;
}

string TopNNode::ToQuery(SqlDialect dialect) {
	vector<string> items;
	string tail;
	bool distinct;
	SelectListAndTail(dialect, items, tail, distinct);
	return "SELECT " + VecToSeparatedList(items) + tail;
}

bool DistinctNode::BuildSelectParts(SqlDialect dialect, SelectParts &out) const {
	(void)dialect;
	// DISTINCT ON renders as a row_number() subquery + filter (see ToQuery), which is not a plain SELECT
	// block, so it cannot be fused into a parent SELECT.
	if (is_distinct_on) {
		return false;
	}
	out.distinct = true;
	out.select_exprs = cte_column_list;
	out.from = child_cte_name;
	return true;
}

string DistinctNode::ToQuery(SqlDialect dialect) {
	if (is_distinct_on && !distinct_on_targets.empty()) {
		return BuildDistinctOnQuery(cte_column_list, distinct_on_targets, distinct_on_orders, child_cte_name);
	}
	vector<string> items;
	string tail;
	bool distinct;
	SelectListAndTail(dialect, items, tail, distinct);
	return "SELECT DISTINCT " + VecToSeparatedList(items) + tail;
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

bool MergedSelectNode::BuildSelectParts(SqlDialect dialect, SelectParts &out) const {
	(void)dialect;
	out.select_exprs = select_exprs; // raw expressions; the output names live in cte_column_list
	out.from = from_clause;
	out.where_conds = where_conditions;
	out.having_conds = having_conditions;
	out.group_by = group_by_text;
	out.order_items = order_items;
	out.limit = limit_str;
	out.offset = offset_str;
	return true;
}

string MergedSelectNode::ToQuery(SqlDialect dialect) {
	vector<string> items;
	string tail;
	bool distinct;
	SelectListAndTail(dialect, items, tail, distinct);
	vector<string> aliased;
	aliased.reserve(items.size());
	for (size_t i = 0; i < items.size(); i++) {
		aliased.push_back(items[i] + " AS " + cte_column_list[i]);
	}
	return "SELECT " + VecToSeparatedList(aliased) + tail;
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

	(void)use_newlines; // output is always pretty-printed.

	// Inline the last CTE as the final SELECT when possible: instead of emitting it as a CTE and
	// then a redundant closing "SELECT ... FROM <last_cte>", turn its body into the result (with its
	// output columns aliased to the user-facing names). Only when the final node is a FinalReadNode
	// whose child is the last CTE and that CTE renders as a plain SELECT (BuildSelectParts succeeds).
	auto *fr = dynamic_cast<FinalReadNode *>(final_node.get());
	SelectParts last_parts;
	const bool inline_last = fr && !nodes.empty() && nodes.back()->cte_name == fr->ChildCteName() &&
	                         nodes.back()->BuildSelectParts(dialect, last_parts);
	const size_t emit_count = inline_last ? nodes.size() - 1 : nodes.size();

	// Build a "de-prefix" rename map for column identifiers: a generated t{N}_{bare} column name
	// collapses to its bare {bare} form when that bare is globally unique across all introduced
	// column names and is a safe unquoted identifier. This drops the t{N}_ prefix wherever it isn't
	// needed to disambiguate. CTE names are NOT collapsed; a column whose full name happens to equal
	// a CTE name is left alone so the shared token isn't rewritten. Only the emitted CTEs count
	// (an inlined last CTE contributes no column definitions of its own).
	unordered_set<string> cte_names;
	for (size_t i = 0; i < emit_count; i++) {
		cte_names.insert(nodes[i]->cte_name);
	}
	// Uniqueness is judged CASE-INSENSITIVELY: DuckDB resolves identifiers ignoring case, so de-prefixing
	// both t0_hello and t1_HeLlO (distinct as strings) would make every later `hello` reference ambiguous.
	unordered_map<string, int> bare_count;
	unordered_set<string> seen_full;
	for (size_t i = 0; i < emit_count; i++) {
		for (const auto &col : nodes[i]->cte_column_list) {
			if (seen_full.insert(col).second) {
				bare_count[StringUtil::Lower(StripGeneratedTablePrefix(col))]++;
			}
		}
	}
	unordered_map<string, string> rename;
	for (const auto &full : seen_full) {
		if (!HasGeneratedTablePrefix(full) || cte_names.count(full)) {
			continue;
		}
		const string bare = StripGeneratedTablePrefix(full);
		// Unsafe to expose bare: a reserved keyword / non-identifier (QuoteIdentifier would quote it)
		// or a name that itself looks like a generated prefix. Keep those prefixed.
		if (bare_count[StringUtil::Lower(bare)] == 1 && QuoteIdentifier(bare) == bare &&
		    !HasGeneratedTablePrefix(bare)) {
			rename[full] = bare;
		}
	}

	std::ostringstream out;
	// WITH on its own line, then each CTE definition at column 0 (body indented INDENT_WIDTH).
	if (emit_count > 0) {
		out << (has_recursive_cte ? "WITH RECURSIVE" : "WITH") << "\n";
		for (size_t i = 0; i < emit_count; ++i) {
			vector<string> cols;
			cols.reserve(nodes[i]->cte_column_list.size());
			for (const auto &c : nodes[i]->cte_column_list) {
				cols.push_back(SubstituteColumnTokens(c, rename));
			}
			out << RenderCteHeader(nodes[i]->cte_name, cols) << "\n";
			SelectParts p;
			if (nodes[i]->BuildSelectParts(dialect, p)) {
				RenameParts(p, rename);
				out << RenderSelectPretty(p, /*out_names=*/vector<string>(), INDENT_WIDTH, dialect);
			} else {
				// Fallback (set ops, recursive CTE, table functions): the node's single-line body.
				out << string(INDENT_WIDTH, ' ') << SubstituteColumnTokens(nodes[i]->ToQuery(dialect), rename);
			}
			out << "\n)" << (i + 1 < emit_count ? ",\n" : "\n");
		}
	}

	// Final statement at column 0.
	if (inline_last) {
		RenameParts(last_parts, rename);
		out << RenderSelectPretty(last_parts, fr->final_column_list, 0, dialect);
	} else if (fr) {
		// Closing "SELECT <child col> AS <user name> FROM <last cte>".
		SelectParts p;
		for (const auto &c : fr->ChildCteColumnList()) {
			p.select_exprs.push_back(SubstituteColumnTokens(c, rename));
		}
		p.from = fr->ChildCteName();
		out << RenderSelectPretty(p, fr->final_column_list, 0, dialect);
	} else {
		// Non-SELECT terminal (e.g. INSERT): keep its own rendering, but put the trailing
		// "SELECT * FROM <cte>" source on its own line so it reads consistently below the CTEs.
		string stmt = SubstituteColumnTokens(final_node->ToQuery(dialect), rename);
		const auto src = stmt.find(" SELECT * FROM ");
		if (src != string::npos) {
			stmt.replace(src, 1, "\n");
		}
		out << stmt;
	}
	out << ";";

	// Drop any "x AS x" that the de-prefixing (or pass-through naming) produced.
	return SwallowSelfAlias(out.str());
}

} // namespace duckdb
