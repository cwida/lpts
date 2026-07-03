#pragma once

#include "duckdb.hpp"
#include "sql_dialect.hpp"

namespace duckdb {

//==============================================================================
// CTE List Node Hierarchy
//==============================================================================
//
// The conversion pipeline is: Logical Plan → CTE List → SQL String.
//
// There is no AST involved. The CTE list is a flat, ordered list of CTEs
// (Common Table Expressions). Each logical operator from DuckDB's plan becomes
// one CTE. Dependencies between CTEs are expressed through name references
// (e.g. a filter CTE reads FROM its child scan CTE by name), not through
// parent-child pointers.
//
// The bottom-up traversal of the logical plan guarantees that each CTE only
// references CTEs defined before it.
//
// Example: "SELECT name FROM users WHERE age > 25" produces (CTEs named t{index}_{operator}):
//
//   WITH t0_scan(t0_name, t0_age) AS (SELECT name, age FROM memory.main.users),
//        t1_filter AS (SELECT * FROM t0_scan WHERE (t0_age) > (25)),
//        t2_projection(t2_name) AS (SELECT t0_name FROM t1_filter)
//   SELECT t2_name AS name FROM t2_projection;
//
// Class hierarchy:
//   CteBaseNode (base)
//   ├── RootNode (virtual) — terminal nodes that produce the final query
//   │   ├── FinalReadNode — the closing SELECT that renames CTE columns back
//   │   └── InsertNode — INSERT INTO ... SELECT * FROM <cte>
//   └── CteNode (virtual) — intermediate nodes, each becomes a WITH clause
//       ├── GetNode        — table scan
//       ├── FilterNode     — WHERE clause
//       ├── ProjectNode    — column selection / expressions
//       ├── AggregateNode  — GROUP BY + aggregate functions
//       ├── JoinNode       — INNER/LEFT/RIGHT/OUTER JOIN
//       ├── UnionNode      — UNION / UNION ALL
//       └── ExceptNode     — EXCEPT / EXCEPT ALL
//==============================================================================

/// Structured SELECT-block clauses for a node that renders as a plain SELECT. Used by both the
/// single-line and the pretty-printed serializers so there is one source of truth per node.
/// `select_exprs` is positionally aligned with the node's `cte_column_list` (its output names).
struct SelectParts {
	bool distinct = false;
	vector<string> select_exprs; ///< Raw SELECT expressions (no "AS"), aligned with cte_column_list.
	string from;                 ///< FROM source (table ref, CTE name, "a JOIN b ON ...", "x USING SAMPLE ...").
	vector<string> where_conds;  ///< Raw WHERE conditions (parens added by the renderer).
	string group_by;             ///< GROUP BY body, or empty.
	vector<string> having_conds; ///< Raw HAVING conditions (parens added by the renderer).
	vector<string> order_items;  ///< ORDER BY items, or empty.
	string limit;                ///< Rendered LIMIT expression, or empty.
	string offset;               ///< Rendered OFFSET expression, or empty.
};

/// Base node for all nodes in the CTE list. Virtual class.
class CteBaseNode {
public:
	virtual ~CteBaseNode() = default;
	/// Produce the SQL fragment for this node (the body inside a CTE's AS (...)).
	/// `dialect` controls identifier quoting and other dialect-specific syntax;
	/// it is owned by the enclosing `CteList` and threaded in by it.
	virtual string ToQuery(SqlDialect dialect) = 0;
	// Constructor.
	explicit CteBaseNode(const size_t index) : idx(index) {
	}
	const size_t idx; // Unique index used for naming (e.g. t0_scan, t1_filter).
};

/// Virtual class for terminal/root nodes (SELECT result or INSERT).
/// These are NOT wrapped in a CTE — they appear as the final statement.
class RootNode : public CteBaseNode {
public:
	explicit RootNode(const size_t index) : CteBaseNode(index) {
	}
};

/// Final node specifically for SELECT queries.
/// Renames CTE column names back to the original column names the user expects.
class FinalReadNode : public RootNode {
	// Attributes.
	string child_cte_name;
	vector<string> child_cte_column_list;

public:
	vector<string> final_column_list;
	const string &ChildCteName() const {
		return child_cte_name;
	}
	const vector<string> &ChildCteColumnList() const {
		return child_cte_column_list;
	}
	~FinalReadNode() override = default;
	// Constructor. Creates the root representation of a SELECT node.
	FinalReadNode(const size_t index, string _child_cte_name, vector<string> _child_cte_column_list,
	              vector<string> _final_column_list)
	    : RootNode(index), child_cte_name(std::move(_child_cte_name)),
	      child_cte_column_list(std::move(_child_cte_column_list)), final_column_list(std::move(_final_column_list)) {
	}
	// Functions
	string ToQuery(SqlDialect dialect) override;
};

/// Node for insertion queries. Cannot be a CTE.
class InsertNode : public RootNode {
	// Attributes.
	string target_table;
	string child_cte_name; // If "insert into t_name values (...)", not defined.
	OnConflictAction action_type;

public:
	~InsertNode() override = default;
	// Constructor.
	InsertNode(const size_t index, string _target_table, string _child_cte_name,
	           const OnConflictAction conflict_action_type)
	    : RootNode(index), target_table(std::move(_target_table)), child_cte_name(std::move(_child_cte_name)),
	      action_type(conflict_action_type) {
	}
	// Functions.
	string ToQuery(SqlDialect dialect) override;
};

/// Node for update queries. Cannot be a CTE. (Not yet implemented.)
class UpdateNode : public RootNode {
public:
	~UpdateNode() override = default;
};

/// Node for deletion queries. Cannot be a CTE. (Not yet implemented.)
class DeleteNode : public RootNode {
public:
	~DeleteNode() override = default;
};

/// Virtual class for intermediate CTE nodes. Each becomes a WITH clause.
class CteNode : public CteBaseNode {
public:
	~CteNode() override = default;
	// Explicitly delete copy constructor to avoid issues.
	CteNode(const CteNode &) = delete;
	CteNode &operator=(const CteNode &) = delete;
	// Constructor.
	explicit CteNode(const size_t index, string name, vector<string> col_list)
	    : CteBaseNode(index), cte_name(std::move(name)), cte_column_list(std::move(col_list)) {
	}
	// Requires ToQuery() to be implemented by derived classes.
	/// Create a CTE-like string for the Node (excluding the WITH keyword).
	/// Example output: "t0_scan(t0_name, t0_age) AS (SELECT name, age FROM ...)"
	string ToCteQuery(SqlDialect dialect);

	/// Structured SELECT clauses for this node when it renders as a plain SELECT block. Returns false
	/// for nodes that don't (set operations, recursive CTEs, mark joins, table functions, SELECT *).
	/// Used both to inline this node as the final SELECT and to pretty-print it.
	virtual bool BuildSelectParts(SqlDialect dialect, SelectParts &out) const {
		(void)dialect;
		(void)out;
		return false;
	}

	/// Single-line "SELECT-list items + tail" view, derived from BuildSelectParts (tail begins with
	/// " FROM ..."). Returns false when BuildSelectParts does. `items` aligns with cte_column_list.
	bool SelectListAndTail(SqlDialect dialect, vector<string> &items, string &tail, bool &distinct) const;

	// Attributes.
	/// The name of the CTE (e.g. "t0_scan", "t1_filter").
	string cte_name;
	/// The "external" names of the CTE columns (the names ancestors use to reference them).
	vector<string> cte_column_list;
	/// True when this CTE is small enough to broadcast in Spark joins.
	bool spark_broadcast_hint = false;
};

class GetNode : public CteNode {
	// Attributes.
	string catalog;
	string schema;
	string table_name;
	size_t table_index;
	vector<string> table_filters;
	vector<string> column_names;
	string input_cte_name;
	idx_t table_function_output_count;

public:
	/// `_tf(...)` alias list in the table function's output order (empty = use projected column order).
	vector<string> table_function_alias;
	/// Parallel to `column_names`: true where the entry is a raw SQL expression (struct field-extraction
	/// pushdown) emitted verbatim rather than quoted. Empty means "all plain identifiers".
	vector<bool> column_is_expression;
	~GetNode() override = default;
	bool BuildSelectParts(SqlDialect dialect, SelectParts &out) const override;
	// Constructor.
	explicit GetNode(const size_t index, vector<string> cte_column_names, string _catalog, string _schema,
	                 string _table_name, const size_t _table_index, vector<string> _table_filters,
	                 vector<string> _column_names, string _input_cte_name = string(),
	                 idx_t _table_function_output_count = DConstants::INVALID_INDEX)
	    : CteNode(index, "t" + std::to_string(index) + "_scan", std::move(cte_column_names)),
	      catalog(std::move(_catalog)), schema(std::move(_schema)), table_name(std::move(_table_name)),
	      table_index(_table_index), table_filters(std::move(_table_filters)), column_names(std::move(_column_names)),
	      input_cte_name(std::move(_input_cte_name)), table_function_output_count(_table_function_output_count) {
	}
	// Functions.
	string ToQuery(SqlDialect dialect) override;
};

class FilterNode : public CteNode {
	// Attributes.
	string child_cte_name;
	vector<string> conditions;

public:
	~FilterNode() override = default;
	bool BuildSelectParts(SqlDialect dialect, SelectParts &out) const override;
	// Constructor.
	FilterNode(const size_t index, vector<string> cte_column_names, string _child_cte_name, vector<string> _conditions)
	    : CteNode(index, "t" + std::to_string(index) + "_filter", std::move(cte_column_names)),
	      child_cte_name(std::move(_child_cte_name)), conditions(std::move(_conditions)) {
	}
	// Functions.
	string ToQuery(SqlDialect dialect) override;
};

class ProjectNode : public CteNode {
	// Attributes.
	string child_cte_name;
	vector<string> column_names;
	size_t table_index;

public:
	~ProjectNode() override = default;
	bool BuildSelectParts(SqlDialect dialect, SelectParts &out) const override;
	// Constructor.
	ProjectNode(const size_t index, vector<string> cte_column_names, string _child_cte_name,
	            vector<string> _column_names, const size_t _table_index)
	    : CteNode(index, "t" + std::to_string(index) + "_projection", std::move(cte_column_names)),
	      child_cte_name(std::move(_child_cte_name)), column_names(std::move(_column_names)),
	      table_index(_table_index) {
	}
	// Functions.
	string ToQuery(SqlDialect dialect) override;
};

class AggregateNode : public CteNode {
	// Attributes.
	string child_cte_name;
	vector<string> group_by_columns; // If empty, is scalar aggregate (e.g. count(*) with no GROUP BY).
	string group_by_clause;
	vector<string> aggregate_expressions;

public:
	~AggregateNode() override = default;
	bool BuildSelectParts(SqlDialect dialect, SelectParts &out) const override;
	// Constructor.
	AggregateNode(const size_t index, vector<string> cte_column_names, string _child_cte_name,
	              vector<string> _group_names, string _group_by_clause, vector<string> _aggregate_names)
	    : CteNode(index, "t" + std::to_string(index) + "_aggregate", std::move(cte_column_names)),
	      child_cte_name(std::move(_child_cte_name)), group_by_columns(std::move(_group_names)),
	      group_by_clause(std::move(_group_by_clause)), aggregate_expressions(std::move(_aggregate_names)) {
	}
	// Functions.
	string ToQuery(SqlDialect dialect) override;
};

class JoinNode : public CteNode {
	// Attributes.
	string left_cte_name, right_cte_name;
	JoinType join_type;
	vector<string> join_conditions;
	string mark_expression; ///< For MARK→LEFT conversion: computed boolean column expression.
	bool is_asof;

public:
	/// MARK join membership-comparison key expressions + null-safe correlation conditions, used to build
	/// the 3-valued mark (see ToQuery).
	string mark_lhs_key;
	string mark_rhs_key;
	vector<string> mark_correlation_conditions;
	/// One indeterminate clause per NULL-propagating membership comparison (see ToQuery / the AST field).
	vector<string> mark_membership_conditions;
	/// Per-membership pieces + equality flag for the AND-vs-OR mark rendering choice (see AstJoinNode).
	vector<string> mark_membership_comparisons;
	vector<string> mark_membership_lhs;
	vector<string> mark_membership_rhs;
	bool mark_join_has_equality = false;
	~JoinNode() override = default;
	bool BuildSelectParts(SqlDialect dialect, SelectParts &out) const override;
	// Constructor.
	JoinNode(const size_t index, vector<string> cte_column_names, string _left_cte_name, string _right_cte_name,
	         JoinType _join_type, vector<string> _join_conditions, string _mark_expression = "", bool _is_asof = false,
	         bool _broadcast_left = false, bool _broadcast_right = false)
	    : CteNode(index, "t" + std::to_string(index) + "_join", std::move(cte_column_names)),
	      left_cte_name(std::move(_left_cte_name)), right_cte_name(std::move(_right_cte_name)), join_type(_join_type),
	      join_conditions(std::move(_join_conditions)), mark_expression(std::move(_mark_expression)), is_asof(_is_asof),
	      broadcast_left(_broadcast_left), broadcast_right(_broadcast_right) {
	}
	// Functions.
	string ToQuery(SqlDialect dialect) override;

private:
	bool broadcast_left;
	bool broadcast_right;
};

class PositionalJoinNode : public CteNode {
	string left_cte_name, right_cte_name;

public:
	~PositionalJoinNode() override = default;
	PositionalJoinNode(const size_t index, vector<string> cte_column_names, string _left_cte_name,
	                   string _right_cte_name)
	    : CteNode(index, "t" + std::to_string(index) + "_positional_join", std::move(cte_column_names)),
	      left_cte_name(std::move(_left_cte_name)), right_cte_name(std::move(_right_cte_name)) {
	}
	string ToQuery(SqlDialect dialect) override;
};

class SampleNode : public CteNode {
	string child_cte_name;
	string sample_clause;

public:
	~SampleNode() override = default;
	bool BuildSelectParts(SqlDialect dialect, SelectParts &out) const override;
	SampleNode(const size_t index, vector<string> cte_column_names, string _child_cte_name, string _sample_clause)
	    : CteNode(index, "t" + std::to_string(index) + "_sample", std::move(cte_column_names)),
	      child_cte_name(std::move(_child_cte_name)), sample_clause(std::move(_sample_clause)) {
	}
	string ToQuery(SqlDialect dialect) override;
};

class UnionNode : public CteNode {
	// Attributes.
	string left_cte_name;
	string right_cte_name;
	const bool is_union_all; // Whether to use "UNION ALL" or just "UNION".
public:
	/// The children's own column lists. A union input can expose MORE columns than the union's arity
	/// (e.g. an inner `ORDER BY a+1` keeps its order key as an extra projected column) — the branch then
	/// selects only the first arity-many columns instead of `*`. Empty ⇒ render `SELECT *`.
	vector<string> left_columns;
	vector<string> right_columns;
	~UnionNode() override = default;
	// Constructor.
	UnionNode(const size_t index, vector<string> cte_column_names, string _left_cte_name, string _right_cte_name,
	          const bool union_all)
	    : CteNode(index, "t" + std::to_string(index) + "_union", std::move(cte_column_names)),
	      left_cte_name(std::move(_left_cte_name)), right_cte_name(std::move(_right_cte_name)),
	      is_union_all(union_all) {
	}
	// Functions.
	string ToQuery(SqlDialect dialect) override;
};

class ExceptNode : public CteNode {
	// Attributes.
	string left_cte_name;
	string right_cte_name;
	const bool is_except_all; // Whether to use "EXCEPT ALL" or just "EXCEPT".
public:
	~ExceptNode() override = default;
	// Constructor.
	ExceptNode(const size_t index, vector<string> cte_column_names, string _left_cte_name, string _right_cte_name,
	           const bool except_all)
	    : CteNode(index, "t" + std::to_string(index) + "_except", std::move(cte_column_names)),
	      left_cte_name(std::move(_left_cte_name)), right_cte_name(std::move(_right_cte_name)),
	      is_except_all(except_all) {
	}
	// Functions.
	string ToQuery(SqlDialect dialect) override;
};

class CteSetOperationNode : public CteNode {
	string left_cte_name;
	string right_cte_name;
	string op_name;
	vector<string> left_select_columns;
	vector<string> right_select_columns;
	const bool is_all;

public:
	~CteSetOperationNode() override = default;
	CteSetOperationNode(const size_t index, vector<string> cte_column_names, string _left_cte_name,
	                    string _right_cte_name, string _op_name, vector<string> _left_select_columns,
	                    vector<string> _right_select_columns, const bool all)
	    : CteNode(index, "t" + std::to_string(index) + "_setop", std::move(cte_column_names)),
	      left_cte_name(std::move(_left_cte_name)), right_cte_name(std::move(_right_cte_name)),
	      op_name(std::move(_op_name)), left_select_columns(std::move(_left_select_columns)),
	      right_select_columns(std::move(_right_select_columns)), is_all(all) {
	}
	string ToQuery(SqlDialect dialect) override;
};

/// ORDER BY node — wraps the child CTE with an ORDER BY clause.
class OrderNode : public CteNode {
	string child_cte_name;
	vector<string> order_items; ///< e.g. "t1_age DESC", "t0_name ASC"
public:
	~OrderNode() override = default;
	bool BuildSelectParts(SqlDialect dialect, SelectParts &out) const override;
	OrderNode(const size_t index, vector<string> cte_column_names, string _child_cte_name, vector<string> _order_items)
	    : CteNode(index, "t" + std::to_string(index) + "_order", std::move(cte_column_names)),
	      child_cte_name(std::move(_child_cte_name)), order_items(std::move(_order_items)) {
	}
	string ToQuery(SqlDialect dialect) override;
};

/// LIMIT / OFFSET node — wraps the child CTE with LIMIT and optional OFFSET.
class LimitNode : public CteNode {
	string child_cte_name;
	string limit_str;  ///< e.g. "10" or "" if no LIMIT
	string offset_str; ///< e.g. "5"  or "" if no OFFSET
	bool limit_needs_child_scalar;
	bool offset_needs_child_scalar;

public:
	~LimitNode() override = default;
	bool BuildSelectParts(SqlDialect dialect, SelectParts &out) const override;
	LimitNode(const size_t index, vector<string> cte_column_names, string _child_cte_name, string _limit_str,
	          string _offset_str, bool _limit_needs_child_scalar, bool _offset_needs_child_scalar)
	    : CteNode(index, "t" + std::to_string(index) + "_limit", std::move(cte_column_names)),
	      child_cte_name(std::move(_child_cte_name)), limit_str(std::move(_limit_str)),
	      offset_str(std::move(_offset_str)), limit_needs_child_scalar(_limit_needs_child_scalar),
	      offset_needs_child_scalar(_offset_needs_child_scalar) {
	}
	string ToQuery(SqlDialect dialect) override;
};

/// TOP_N node — ORDER BY + LIMIT/OFFSET fused into one CTE by the TOP_N optimizer.
class TopNNode : public CteNode {
	string child_cte_name;
	vector<string> order_items;
	idx_t limit;
	idx_t offset;

public:
	~TopNNode() override = default;
	bool BuildSelectParts(SqlDialect dialect, SelectParts &out) const override;
	TopNNode(const size_t index, vector<string> cte_column_names, string _child_cte_name, vector<string> _order_items,
	         idx_t _limit, idx_t _offset)
	    : CteNode(index, "t" + std::to_string(index) + "_topn", std::move(cte_column_names)),
	      child_cte_name(std::move(_child_cte_name)), order_items(std::move(_order_items)), limit(_limit),
	      offset(_offset) {
	}
	string ToQuery(SqlDialect dialect) override;
};

/// DISTINCT node — wraps the child CTE with SELECT DISTINCT, or, for DISTINCT ON, a row_number() filter.
class DistinctNode : public CteNode {
	string child_cte_name;

public:
	/// DISTINCT ON (distinct_on_targets) keeping one row per group, ordered by distinct_on_orders.
	bool is_distinct_on = false;
	vector<string> distinct_on_targets;
	vector<string> distinct_on_orders;

	~DistinctNode() override = default;
	bool BuildSelectParts(SqlDialect dialect, SelectParts &out) const override;
	DistinctNode(const size_t index, vector<string> cte_column_names, string _child_cte_name)
	    : CteNode(index, "t" + std::to_string(index) + "_distinct", std::move(cte_column_names)),
	      child_cte_name(std::move(_child_cte_name)) {
	}
	string ToQuery(SqlDialect dialect) override;
};

/// DELIM_GET node — SELECT DISTINCT of correlated columns from the outer query CTE.
/// Generated as part of DELIM_JOIN decorrelation. Produces distinct correlation key values
/// that are fed into the inner subquery, enabling a non-correlated join.
class DelimGetNode : public CteNode {
	string source_cte_name;     ///< The left-side (outer) CTE to SELECT DISTINCT from.
	vector<string> source_cols; ///< Column names to project from source_cte (e.g. "t3_p_partkey").

public:
	~DelimGetNode() override = default;
	DelimGetNode(const size_t index, vector<string> cte_column_names, string _source_cte_name,
	             vector<string> _source_cols)
	    : CteNode(index, "t" + std::to_string(index) + "_scan", std::move(cte_column_names)),
	      source_cte_name(std::move(_source_cte_name)), source_cols(std::move(_source_cols)) {
	}
	string ToQuery(SqlDialect dialect) override;
};

/// Recursive CTE node — WITH RECURSIVE body definition.
/// The body combines an anchor SELECT (reading from anchor_cte_name) with the
/// recursive step (expressed as inline SQL) using UNION [ALL].
/// cte_column_list holds the user-visible column names for the CTE header, e.g. (n).
class RecursiveCteNode : public CteNode {
	string anchor_cte_name;     ///< The flat CTE that holds the anchor result.
	vector<string> anchor_cols; ///< LPTS column names to SELECT from the anchor CTE.
	string recursive_step_sql;  ///< Inline SQL for the recursive step.
	bool union_all;

public:
	~RecursiveCteNode() override = default;
	RecursiveCteNode(const size_t index, vector<string> stripped_cols, string _anchor_cte_name,
	                 vector<string> _anchor_cols, string _recursive_step_sql, bool _union_all)
	    : CteNode(index, "t" + std::to_string(index) + "_recursive_cte", std::move(stripped_cols)),
	      anchor_cte_name(std::move(_anchor_cte_name)), anchor_cols(std::move(_anchor_cols)),
	      recursive_step_sql(std::move(_recursive_step_sql)), union_all(_union_all) {
	}
	string ToQuery(SqlDialect dialect) override;
};

/// Merged pipeline block — a single flat SELECT that fuses a chain of single-child
/// pipeline operators (Limit / OrderBy / Project / Aggregate / Project / Filter,
/// and optionally an absorbed pushdown-free base-table scan) into one statement:
///
///   SELECT <select_exprs> FROM <from_clause>
///     [WHERE (c1) AND (c2) ...] [GROUP BY <group_by_text>] [HAVING (h1) AND (h2) ...]
///     [ORDER BY <order_items>] [LIMIT <limit>] [OFFSET <offset>]
///
/// All clause strings are already fully rendered (column references folded into
/// their defining expressions and dialect-qualified) when the node is built.
///
/// Only built when ≥2 components are actually fused (pipeline operators, plus an
/// absorbed base-table scan); a single unfused operator goes through its own CteNode
/// instead. Always named "block_N".
class MergedSelectNode : public CteNode {
	vector<string> select_exprs;      ///< Raw SELECT expressions; output names are in cte_column_list.
	string from_clause;               ///< Table reference or child CTE name.
	vector<string> where_conditions;  ///< Folded WHERE conditions (ANDed, each parenthesized).
	vector<string> having_conditions; ///< Folded HAVING conditions (ANDed, each parenthesized).
	string group_by_text;             ///< Folded GROUP BY body, or empty.
	vector<string> order_items;       ///< Folded ORDER BY items, or empty.
	string limit_str;                 ///< e.g. "10", or empty.
	string offset_str;                ///< e.g. "5", or empty.

public:
	~MergedSelectNode() override = default;
	bool BuildSelectParts(SqlDialect dialect, SelectParts &out) const override;
	MergedSelectNode(const size_t index, vector<string> cte_column_names, vector<string> _select_exprs,
	                 string _from_clause, vector<string> _where_conditions, vector<string> _having_conditions,
	                 string _group_by_text, vector<string> _order_items, string _limit_str, string _offset_str)
	    : CteNode(index, "t" + std::to_string(index) + "_block", std::move(cte_column_names)),
	      select_exprs(std::move(_select_exprs)), from_clause(std::move(_from_clause)),
	      where_conditions(std::move(_where_conditions)), having_conditions(std::move(_having_conditions)),
	      group_by_text(std::move(_group_by_text)), order_items(std::move(_order_items)),
	      limit_str(std::move(_limit_str)), offset_str(std::move(_offset_str)) {
	}
	string ToQuery(SqlDialect dialect) override;
};

/// The complete CTE list: an ordered list of CTE nodes + one final (root) node.
/// Calling ToQuery() serializes the whole thing into a single SQL string.
class CteList {
	// Attributes.
	vector<unique_ptr<CteNode>> nodes; ///< Ordered list of CTEs (leaf-to-root).
	unique_ptr<RootNode> final_node;   ///< The closing statement (SELECT or INSERT).
	bool has_recursive_cte;            ///< True if any node is a RecursiveCteNode.
	SqlDialect dialect;                ///< Dialect propagated into every node's ToQuery().
	bool emit_spark_hints;             ///< Emit Spark optimizer hints when supported.

public:
	// Constructor.
	CteList(vector<unique_ptr<CteNode>> _nodes, unique_ptr<RootNode> _final_node, bool _has_recursive_cte = false,
	        SqlDialect _dialect = SqlDialect::DUCKDB, bool _emit_spark_hints = false)
	    : nodes(std::move(_nodes)), final_node(std::move(_final_node)), has_recursive_cte(_has_recursive_cte),
	      dialect(_dialect), emit_spark_hints(_emit_spark_hints) {
	}
	/// Serialize the CTE list into a SQL query string.
	/// If `use_newlines` is true, the string uses newlines between CTEs for readability.
	/// If `output_names` is provided, use them for the final SELECT column aliases
	/// (overriding the default CTE-derived names).
	string ToQuery(bool use_newlines, const vector<string> &output_names = {});
};

} // namespace duckdb
