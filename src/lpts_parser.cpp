#include "lpts_parser.hpp"
#include "lpts_helpers.hpp"
#include "lpts_date_format.hpp"
#include "lpts_sql_scanner.hpp"

#include "duckdb/parser/parser.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace duckdb {

SqlDialect ReadInputDialect(ClientContext &context) {
	Value dialect_val;
	if (context.TryGetCurrentSetting("lpts_input_dialect", dialect_val)) {
		return ParseSqlDialectSetting(dialect_val.GetValue<string>(), "lpts_input_dialect");
	}
	return SqlDialect::DUCKDB;
}

static void ThrowUnsupportedInputDialectFeature(SqlDialect dialect, const string &feature_name, const string &reason) {
	throw NotImplementedException("LPTS_UNSUPPORTED_INPUT_DIALECT_FEATURE: dialect=%s feature=%s reason=%s",
	                              SqlDialectToString(dialect), feature_name, reason);
}

static string NormalizeBacktickIdentifiers(const string &sql, SqlDialect dialect) {
	if (dialect != SqlDialect::MYSQL_MARIADB && dialect != SqlDialect::HIVE && dialect != SqlDialect::BIGQUERY &&
	    dialect != SqlDialect::SPARK) {
		return sql;
	}

	string result;
	for (idx_t i = 0; i < sql.size(); i++) {
		char c = sql[i];
		if (TryAppendSkippableSqlSpan(sql, i, result)) {
			continue;
		}
		if (c == '`') {
			string identifier;
			i++;
			while (i < sql.size()) {
				if (sql[i] == '`') {
					if (i + 1 < sql.size() && sql[i + 1] == '`') {
						identifier += '`';
						i += 2;
						continue;
					}
					break;
				}
				if (sql[i] == '"') {
					identifier += "\"\"";
				} else {
					identifier += sql[i];
				}
				i++;
			}
			if (dialect == SqlDialect::BIGQUERY && StringUtil::Contains(identifier, ".")) {
				auto parts = StringUtil::Split(identifier, '.');
				for (idx_t part_idx = 0; part_idx < parts.size(); part_idx++) {
					if (part_idx > 0) {
						result += ".";
					}
					result += "\"" + parts[part_idx] + "\"";
				}
			} else {
				result += "\"" + identifier + "\"";
			}
			continue;
		}
		result += c;
	}
	return result;
}

static string NormalizeBracketIdentifiers(const string &sql, SqlDialect dialect) {
	if (dialect == SqlDialect::DUCKDB) {
		return sql;
	}

	string result;
	for (idx_t i = 0; i < sql.size(); i++) {
		char c = sql[i];
		if (TryAppendSkippableSqlSpan(sql, i, result)) {
			continue;
		}
		if (c != '[' || !CanStartBracketIdentifier(sql, i)) {
			result += c;
			continue;
		}

		idx_t close_pos = i + 1;
		while (close_pos < sql.size() && sql[close_pos] != ']') {
			close_pos++;
		}
		if (close_pos >= sql.size()) {
			result += c;
			continue;
		}

		string identifier = sql.substr(i + 1, close_pos - i - 1);
		if (!IsSafeIdentifierContent(identifier)) {
			result += sql.substr(i, close_pos - i + 1);
			i = close_pos;
			continue;
		}
		result += QuoteDuckDBIdentifier(identifier);
		i = close_pos;
	}
	return result;
}

/// Match a Spark/Delta temporal-clause keyword at `pos` and map it to the DuckDB `AT (...)` named
/// parameter that carries the same snapshot. `VERSION`/`SYSTEM_VERSION` pin a commit version,
/// `TIMESTAMP`/`SYSTEM_TIME` pin a point in time.
static bool TryReadTimeTravelKeyword(const string &sql, idx_t pos, idx_t &end, string &at_parameter) {
	struct TimeTravelKeyword {
		const char *keyword;
		const char *at_parameter;
	};
	static const TimeTravelKeyword KEYWORDS[] = {{"system_version", "VERSION"},
	                                             {"version", "VERSION"},
	                                             {"system_time", "TIMESTAMP"},
	                                             {"timestamp", "TIMESTAMP"}};
	for (const auto &candidate : KEYWORDS) {
		if (MatchesKeywordAt(sql, pos, candidate.keyword)) {
			end = pos + strlen(candidate.keyword);
			at_parameter = candidate.at_parameter;
			return true;
		}
	}
	return false;
}

/// True when `token` may name a relation on its own, without a leading `AS`. DuckDB's `alias_clause`
/// takes a `ColId`, which admits plain identifiers plus the unreserved and column-name keywords but
/// not the reserved or type/function ones -- so `WHERE`, `JOIN`, `NATURAL`, `TABLESAMPLE` and their
/// kin end the relation instead of naming it.
static bool CanBeBareRelationAlias(const string &token) {
	const KeywordCategory category = Parser::IsKeyword(LowerCopy(token));
	return category != KeywordCategory::KEYWORD_RESERVED && category != KeywordCategory::KEYWORD_TYPE_FUNC;
}

/// Read the optional `[AS] alias [(column, ...)]` that Spark allows *after* a temporal clause,
/// yielding the original text so quoting and case survive being moved. Returns false when the
/// relation is unaliased, i.e. when what follows continues the query (`WHERE`, `JOIN`, `,`, `)`).
static bool TryReadRelationAlias(const string &sql, idx_t pos, idx_t &end, string &alias_sql) {
	const idx_t start = SkipWhitespace(sql, pos);
	idx_t cursor = start;
	const bool explicit_as = MatchesKeywordAt(sql, cursor, "as");
	if (explicit_as) {
		cursor = SkipWhitespace(sql, cursor + 2);
	}

	idx_t alias_end;
	string token;
	if (cursor < sql.size() && sql[cursor] == '"') {
		if (!TryReadSkippableSqlSpan(sql, cursor, alias_end)) {
			return false;
		}
	} else if (TryReadIdentifierToken(sql, cursor, alias_end, token)) {
		// An explicit `AS` already committed to an alias; a bare token only aliases when the grammar
		// lets it, otherwise it is the next clause.
		if (!explicit_as && !CanBeBareRelationAlias(token)) {
			return false;
		}
	} else {
		return false;
	}

	const idx_t columns_start = SkipWhitespace(sql, alias_end);
	if (columns_start < sql.size() && sql[columns_start] == '(') {
		const idx_t columns_end = FindMatchingParen(sql, columns_start);
		if (columns_end == DConstants::INVALID_INDEX) {
			return false;
		}
		alias_end = columns_end + 1;
	}

	end = alias_end;
	alias_sql = sql.substr(start, alias_end - start);
	return true;
}

/// Rewrite the Spark/Delta time-travel clause into DuckDB's `AT (...)` snapshot clause:
///
///     FROM t [FOR] VERSION   AS OF 366        ->  FROM t AT (VERSION => 366)
///     FROM t [FOR] TIMESTAMP AS OF '2024-...' ->  FROM t AT (TIMESTAMP => '2024-...')
///     FROM t VERSION AS OF 366 AS v           ->  FROM t AS v AT (VERSION => 366)
///
/// Spark puts the clause between the relation and its alias; DuckDB puts it after the alias, so the
/// alias is carried across the rewrite rather than left stranded behind the qualifier.
///
/// The pinned snapshot is *represented*, never dropped: DuckDB's `AT` clause is the semantically
/// equivalent spelling, so the plan keeps reading the pinned snapshot and the generated SQL can
/// render the pin back out in the target dialect. Dropping the clause here would silently promote
/// every pinned scan to "read latest", changing the meaning of the query.
///
/// A match requires the full `<keyword> AS OF <literal>` sequence, so a column or alias merely named
/// `version` / `timestamp` is left alone.
static string RewriteTimeTravelClauses(const string &sql, SqlDialect dialect) {
	if (dialect != SqlDialect::SPARK) {
		return sql;
	}

	string result;
	for (idx_t i = 0; i < sql.size(); i++) {
		if (TryAppendSkippableSqlSpan(sql, i, result)) {
			continue;
		}

		idx_t keyword_start = i;
		if (MatchesKeywordAt(sql, i, "for")) {
			keyword_start = SkipWhitespace(sql, i + 3);
		}
		idx_t keyword_end = keyword_start;
		string at_parameter;
		if (!TryReadTimeTravelKeyword(sql, keyword_start, keyword_end, at_parameter)) {
			result += sql[i];
			continue;
		}
		idx_t as_pos = SkipWhitespace(sql, keyword_end);
		if (!MatchesKeywordAt(sql, as_pos, "as")) {
			result += sql[i];
			continue;
		}
		idx_t of_pos = SkipWhitespace(sql, as_pos + 2);
		if (!MatchesKeywordAt(sql, of_pos, "of")) {
			result += sql[i];
			continue;
		}

		idx_t value_start = SkipWhitespace(sql, of_pos + 2);
		idx_t value_end = value_start;
		string literal;
		string value_sql;
		if (TryReadSingleQuotedLiteral(sql, value_start, value_end, literal)) {
			value_sql = SingleQuotedSqlString(literal);
		} else if (TryReadNumericToken(sql, value_start, value_end, value_sql)) {
			if (at_parameter == "TIMESTAMP") {
				ThrowUnsupportedInputDialectFeature(
				    dialect, "time_travel", "timestamp time travel needs a string literal, got '" + value_sql + "'");
			}
		} else {
			result += sql[i];
			continue;
		}

		// Spark: `<relation> VERSION AS OF <v> [AS] alias`. DuckDB: `<relation> [AS] alias AT (...)`.
		// Emit the alias first so the qualifier still attaches to the relation it pins.
		idx_t clause_end = value_end;
		idx_t alias_end;
		string alias_sql;
		if (TryReadRelationAlias(sql, value_end, alias_end, alias_sql)) {
			result += alias_sql + " ";
			clause_end = alias_end;
		}
		result += "AT (" + at_parameter + " => " + value_sql + ")";
		i = clause_end - 1;
	}
	return result;
}

enum class InputFunctionRewriteKind : uint8_t {
	RENAME,
	DATE_ADD_DAYS,
	DATE_SUB_DAYS,
	DATE_ADD_INTERVAL,
	DATE_SUB_INTERVAL,
	DATE_DIFF_DAYS,
	DATE_DIFF_UNIT_LAST,
	DATE_TRUNC_UNIT_SECOND,
	CURRENT_DATE,
	CURRENT_TIMESTAMP
};

struct InputFunctionRewrite {
	const char *source_name;
	const char *target_name;
	InputFunctionRewriteKind kind;
	bool convert_format_arg;
	InputDateFormatStyle format_style;
	idx_t format_arg_index;
	bool swap_two_args;
	bool cast_result_to_date;
	uint32_t dialect_mask;
};

static constexpr uint32_t DialectBit(SqlDialect dialect) {
	return 1U << static_cast<uint8_t>(dialect);
}

static constexpr uint32_t ALL_INPUT_DIALECTS = DialectBit(SqlDialect::POSTGRES) | DialectBit(SqlDialect::SPARK) |
                                               DialectBit(SqlDialect::HIVE) | DialectBit(SqlDialect::TRINO_PRESTO) |
                                               DialectBit(SqlDialect::SNOWFLAKE) | DialectBit(SqlDialect::BIGQUERY) |
                                               DialectBit(SqlDialect::REDSHIFT) | DialectBit(SqlDialect::MYSQL_MARIADB);

static bool AppliesToDialect(const InputFunctionRewrite &rewrite, SqlDialect dialect) {
	return (rewrite.dialect_mask & DialectBit(dialect)) != 0;
}

static string NormalizeDatePartArg(const string &arg, SqlDialect dialect, const string &function_name) {
	string trimmed = TrimCopy(arg);
	idx_t end;
	string literal;
	if (TryReadSingleQuotedLiteral(trimmed, 0, end, literal) && SkipWhitespace(trimmed, end) == trimmed.size()) {
		return SingleQuotedSqlString(LowerCopy(literal));
	}
	for (char c : trimmed) {
		if (!std::isalpha(static_cast<unsigned char>(c)) && c != '_') {
			ThrowUnsupportedInputDialectFeature(dialect, function_name,
			                                    "date part must be an identifier or string literal");
		}
	}
	return SingleQuotedSqlString(LowerCopy(trimmed));
}

static string BuildRewrittenFunctionCall(const InputFunctionRewrite &rewrite, SqlDialect dialect, vector<string> args) {
	const string source_name = rewrite.source_name;
	if (rewrite.convert_format_arg) {
		if (args.size() != 2) {
			ThrowUnsupportedInputDialectFeature(dialect, source_name,
			                                    "date format function rewrite expects exactly two arguments");
		}
		if (rewrite.format_arg_index >= args.size()) {
			throw InternalException("date format argument index out of range");
		}
		idx_t literal_end;
		string format;
		if (!TryReadSingleQuotedLiteral(args[rewrite.format_arg_index], 0, literal_end, format) ||
		    SkipWhitespace(args[rewrite.format_arg_index], literal_end) != args[rewrite.format_arg_index].size()) {
			ThrowUnsupportedInputDialectFeature(dialect, source_name, "date format argument must be a string literal");
		}
		args[rewrite.format_arg_index] =
		    SingleQuotedSqlString(ConvertInputDateFormatToDuckDB(format, dialect, rewrite.format_style));
	}

	string rewritten_call;
	switch (rewrite.kind) {
	case InputFunctionRewriteKind::RENAME:
		if (rewrite.swap_two_args) {
			if (args.size() != 2) {
				ThrowUnsupportedInputDialectFeature(dialect, source_name,
				                                    "argument reordering expects exactly two arguments");
			}
			rewritten_call = string(rewrite.target_name) + "(" + args[1] + ", " + args[0] + ")";
		} else {
			rewritten_call = string(rewrite.target_name) + "(" + VecToSeparatedList(args, ", ") + ")";
		}
		break;
	case InputFunctionRewriteKind::DATE_ADD_DAYS:
		if (args.size() != 2) {
			ThrowUnsupportedInputDialectFeature(dialect, source_name, "date_add expects exactly two arguments");
		}
		rewritten_call = "(" + args[0] + " + (" + args[1] + ") * INTERVAL '1' DAY)";
		break;
	case InputFunctionRewriteKind::DATE_SUB_DAYS:
		if (args.size() != 2) {
			ThrowUnsupportedInputDialectFeature(dialect, source_name, "date_sub expects exactly two arguments");
		}
		rewritten_call = "(" + args[0] + " - (" + args[1] + ") * INTERVAL '1' DAY)";
		break;
	case InputFunctionRewriteKind::DATE_ADD_INTERVAL:
		if (args.size() != 2) {
			ThrowUnsupportedInputDialectFeature(dialect, source_name, "date_add expects exactly two arguments");
		}
		rewritten_call = "(" + args[0] + " + " + args[1] + ")";
		break;
	case InputFunctionRewriteKind::DATE_SUB_INTERVAL:
		if (args.size() != 2) {
			ThrowUnsupportedInputDialectFeature(dialect, source_name, "date_sub expects exactly two arguments");
		}
		rewritten_call = "(" + args[0] + " - " + args[1] + ")";
		break;
	case InputFunctionRewriteKind::DATE_DIFF_DAYS:
		if (args.size() != 2) {
			ThrowUnsupportedInputDialectFeature(dialect, source_name, "datediff expects exactly two arguments");
		}
		rewritten_call = "date_diff('day', " + args[1] + ", " + args[0] + ")";
		break;
	case InputFunctionRewriteKind::DATE_DIFF_UNIT_LAST:
		if (args.size() != 3) {
			ThrowUnsupportedInputDialectFeature(dialect, source_name, "date_diff expects exactly three arguments");
		}
		rewritten_call =
		    "date_diff(" + NormalizeDatePartArg(args[2], dialect, source_name) + ", " + args[1] + ", " + args[0] + ")";
		break;
	case InputFunctionRewriteKind::DATE_TRUNC_UNIT_SECOND:
		if (args.size() != 2) {
			ThrowUnsupportedInputDialectFeature(dialect, source_name, "date_trunc expects exactly two arguments");
		}
		rewritten_call = "date_trunc(" + NormalizeDatePartArg(args[1], dialect, source_name) + ", " + args[0] + ")";
		break;
	case InputFunctionRewriteKind::CURRENT_DATE:
		if (!args.empty()) {
			ThrowUnsupportedInputDialectFeature(dialect, source_name, "current_date expects no arguments");
		}
		rewritten_call = "CAST(now() AS DATE)";
		break;
	case InputFunctionRewriteKind::CURRENT_TIMESTAMP:
		if (!args.empty()) {
			ThrowUnsupportedInputDialectFeature(dialect, source_name, "current_timestamp expects no arguments");
		}
		rewritten_call = "now()";
		break;
	default:
		throw InternalException("unknown input function rewrite kind");
	}

	if (rewrite.cast_result_to_date) {
		rewritten_call = "CAST(" + rewritten_call + " AS DATE)";
	}
	return rewritten_call;
}

static string RewriteFunctionCalls(const string &sql, SqlDialect dialect, const InputFunctionRewrite &rewrite) {
	string result;
	for (idx_t i = 0; i < sql.size(); i++) {
		char c = sql[i];
		if (TryAppendSkippableSqlSpan(sql, i, result)) {
			continue;
		}
		if (!IsIdentStart(c)) {
			result += c;
			continue;
		}

		idx_t ident_start = i;
		idx_t ident_end = i + 1;
		while (ident_end < sql.size() && IsIdentPart(sql[ident_end])) {
			ident_end++;
		}
		string ident = sql.substr(ident_start, ident_end - ident_start);
		idx_t open_pos = SkipWhitespace(sql, ident_end);
		if (!EqualsLowercase(ident, rewrite.source_name) || open_pos >= sql.size() || sql[open_pos] != '(') {
			result += ident;
			i = ident_end - 1;
			continue;
		}

		idx_t close_pos = FindMatchingParen(sql, open_pos);
		if (close_pos == DConstants::INVALID_INDEX) {
			ThrowUnsupportedInputDialectFeature(dialect, rewrite.source_name, "could not find matching ')'");
		}
		auto args = SplitTopLevelArgs(sql.substr(open_pos + 1, close_pos - open_pos - 1));
		result += BuildRewrittenFunctionCall(rewrite, dialect, std::move(args));
		i = close_pos;
	}
	return result;
}

static string RewriteFunctionCalls(const string &sql, SqlDialect dialect) {
	static const vector<InputFunctionRewrite> rewrites = {
	    {"ifnull", "coalesce", InputFunctionRewriteKind::RENAME, false, InputDateFormatStyle::MYSQL_PERCENT, 1, false,
	     false, ALL_INPUT_DIALECTS},
	    {"nvl", "coalesce", InputFunctionRewriteKind::RENAME, false, InputDateFormatStyle::MYSQL_PERCENT, 1, false,
	     false, ALL_INPUT_DIALECTS},
	    {"date_format", "strftime", InputFunctionRewriteKind::RENAME, true, InputDateFormatStyle::MYSQL_PERCENT, 1,
	     false, false, DialectBit(SqlDialect::MYSQL_MARIADB) | DialectBit(SqlDialect::TRINO_PRESTO)},
	    {"str_to_date", "strptime", InputFunctionRewriteKind::RENAME, true, InputDateFormatStyle::MYSQL_PERCENT, 1,
	     false, false, DialectBit(SqlDialect::MYSQL_MARIADB)},
	    {"date_parse", "strptime", InputFunctionRewriteKind::RENAME, true, InputDateFormatStyle::MYSQL_PERCENT, 1,
	     false, false, DialectBit(SqlDialect::TRINO_PRESTO)},
	    {"date_format", "strftime", InputFunctionRewriteKind::RENAME, true, InputDateFormatStyle::JAVA, 1, false, false,
	     DialectBit(SqlDialect::SPARK) | DialectBit(SqlDialect::HIVE)},
	    {"to_timestamp", "strptime", InputFunctionRewriteKind::RENAME, true, InputDateFormatStyle::JAVA, 1, false,
	     false, DialectBit(SqlDialect::SPARK) | DialectBit(SqlDialect::HIVE)},
	    {"to_date", "strptime", InputFunctionRewriteKind::RENAME, true, InputDateFormatStyle::JAVA, 1, false, true,
	     DialectBit(SqlDialect::SPARK) | DialectBit(SqlDialect::HIVE)},
	    {"to_char", "strftime", InputFunctionRewriteKind::RENAME, true, InputDateFormatStyle::POSTGRES, 1, false, false,
	     DialectBit(SqlDialect::POSTGRES) | DialectBit(SqlDialect::REDSHIFT)},
	    {"to_timestamp", "strptime", InputFunctionRewriteKind::RENAME, true, InputDateFormatStyle::POSTGRES, 1, false,
	     false, DialectBit(SqlDialect::POSTGRES) | DialectBit(SqlDialect::REDSHIFT)},
	    {"to_date", "strptime", InputFunctionRewriteKind::RENAME, true, InputDateFormatStyle::POSTGRES, 1, false, true,
	     DialectBit(SqlDialect::POSTGRES) | DialectBit(SqlDialect::REDSHIFT)},
	    {"to_char", "strftime", InputFunctionRewriteKind::RENAME, true, InputDateFormatStyle::SNOWFLAKE, 1, false,
	     false, DialectBit(SqlDialect::SNOWFLAKE)},
	    {"to_timestamp", "strptime", InputFunctionRewriteKind::RENAME, true, InputDateFormatStyle::SNOWFLAKE, 1, false,
	     false, DialectBit(SqlDialect::SNOWFLAKE)},
	    {"to_date", "strptime", InputFunctionRewriteKind::RENAME, true, InputDateFormatStyle::SNOWFLAKE, 1, false, true,
	     DialectBit(SqlDialect::SNOWFLAKE)},
	    {"format_timestamp", "strftime", InputFunctionRewriteKind::RENAME, true, InputDateFormatStyle::BIGQUERY_PERCENT,
	     0, true, false, DialectBit(SqlDialect::BIGQUERY)},
	    {"format_date", "strftime", InputFunctionRewriteKind::RENAME, true, InputDateFormatStyle::BIGQUERY_PERCENT, 0,
	     true, false, DialectBit(SqlDialect::BIGQUERY)},
	    {"parse_timestamp", "strptime", InputFunctionRewriteKind::RENAME, true, InputDateFormatStyle::BIGQUERY_PERCENT,
	     0, true, false, DialectBit(SqlDialect::BIGQUERY)},
	    {"parse_date", "strptime", InputFunctionRewriteKind::RENAME, true, InputDateFormatStyle::BIGQUERY_PERCENT, 0,
	     true, true, DialectBit(SqlDialect::BIGQUERY)},
	    {"date_add", nullptr, InputFunctionRewriteKind::DATE_ADD_DAYS, false, InputDateFormatStyle::MYSQL_PERCENT, 1,
	     false, false, DialectBit(SqlDialect::SPARK) | DialectBit(SqlDialect::HIVE)},
	    {"date_sub", nullptr, InputFunctionRewriteKind::DATE_SUB_DAYS, false, InputDateFormatStyle::MYSQL_PERCENT, 1,
	     false, false, DialectBit(SqlDialect::SPARK) | DialectBit(SqlDialect::HIVE)},
	    {"date_add", nullptr, InputFunctionRewriteKind::DATE_ADD_INTERVAL, false, InputDateFormatStyle::MYSQL_PERCENT,
	     1, false, false, DialectBit(SqlDialect::MYSQL_MARIADB) | DialectBit(SqlDialect::BIGQUERY)},
	    {"date_sub", nullptr, InputFunctionRewriteKind::DATE_SUB_INTERVAL, false, InputDateFormatStyle::MYSQL_PERCENT,
	     1, false, false, DialectBit(SqlDialect::MYSQL_MARIADB) | DialectBit(SqlDialect::BIGQUERY)},
	    {"datediff", nullptr, InputFunctionRewriteKind::DATE_DIFF_DAYS, false, InputDateFormatStyle::MYSQL_PERCENT, 1,
	     false, false,
	     DialectBit(SqlDialect::SPARK) | DialectBit(SqlDialect::HIVE) | DialectBit(SqlDialect::MYSQL_MARIADB)},
	    {"date_diff", nullptr, InputFunctionRewriteKind::DATE_DIFF_UNIT_LAST, false,
	     InputDateFormatStyle::MYSQL_PERCENT, 1, false, false, DialectBit(SqlDialect::BIGQUERY)},
	    {"timestamp_diff", nullptr, InputFunctionRewriteKind::DATE_DIFF_UNIT_LAST, false,
	     InputDateFormatStyle::MYSQL_PERCENT, 1, false, false, DialectBit(SqlDialect::BIGQUERY)},
	    {"date_trunc", nullptr, InputFunctionRewriteKind::DATE_TRUNC_UNIT_SECOND, false,
	     InputDateFormatStyle::MYSQL_PERCENT, 1, false, false, DialectBit(SqlDialect::BIGQUERY)},
	    {"current_date", nullptr, InputFunctionRewriteKind::CURRENT_DATE, false, InputDateFormatStyle::MYSQL_PERCENT, 1,
	     false, false, ALL_INPUT_DIALECTS},
	    {"current_timestamp", nullptr, InputFunctionRewriteKind::CURRENT_TIMESTAMP, false,
	     InputDateFormatStyle::MYSQL_PERCENT, 1, false, false, ALL_INPUT_DIALECTS},
	};

	string result = sql;
	for (const auto &rewrite : rewrites) {
		if (AppliesToDialect(rewrite, dialect)) {
			result = RewriteFunctionCalls(result, dialect, rewrite);
		}
	}
	return result;
}

static string RewriteMysqlLimitComma(const string &sql, SqlDialect dialect) {
	string result;
	for (idx_t i = 0; i < sql.size(); i++) {
		char c = sql[i];
		if (TryAppendSkippableSqlSpan(sql, i, result)) {
			continue;
		}
		if (!MatchesKeywordAt(sql, i, "limit")) {
			result += c;
			continue;
		}

		idx_t offset_start = SkipWhitespace(sql, i + 5);
		idx_t offset_end = offset_start;
		while (offset_end < sql.size() && std::isdigit(static_cast<unsigned char>(sql[offset_end]))) {
			offset_end++;
		}
		idx_t comma_pos = SkipWhitespace(sql, offset_end);
		if (offset_start == offset_end || comma_pos >= sql.size() || sql[comma_pos] != ',') {
			result += sql.substr(i, 5);
			i += 4;
			continue;
		}
		idx_t count_start = SkipWhitespace(sql, comma_pos + 1);
		idx_t count_end = count_start;
		while (count_end < sql.size() && std::isdigit(static_cast<unsigned char>(sql[count_end]))) {
			count_end++;
		}
		if (count_start == count_end) {
			ThrowUnsupportedInputDialectFeature(dialect, "LIMIT offset,count",
			                                    "only numeric LIMIT offset,count is supported");
		}
		result += "LIMIT " + sql.substr(count_start, count_end - count_start) + " OFFSET " +
		          sql.substr(offset_start, offset_end - offset_start);
		i = count_end - 1;
	}
	return result;
}

static bool IsIntervalUnit(const string &unit) {
	string normalized = LowerCopy(unit);
	return normalized == "year" || normalized == "years" || normalized == "month" || normalized == "months" ||
	       normalized == "day" || normalized == "days" || normalized == "hour" || normalized == "hours" ||
	       normalized == "minute" || normalized == "minutes" || normalized == "second" || normalized == "seconds";
}

static string SingularIntervalUnit(const string &unit) {
	string normalized = LowerCopy(unit);
	if (!normalized.empty() && normalized.back() == 's') {
		normalized.pop_back();
	}
	return normalized;
}

static string RewriteIntervals(const string &sql, SqlDialect dialect) {
	string result;
	for (idx_t i = 0; i < sql.size(); i++) {
		if (TryAppendSkippableSqlSpan(sql, i, result)) {
			continue;
		}
		if (!MatchesKeywordAt(sql, i, "interval")) {
			result += sql[i];
			continue;
		}

		idx_t value_start = SkipWhitespace(sql, i + 8);
		idx_t value_end = value_start;
		string value;
		string literal;
		if (TryReadSingleQuotedLiteral(sql, value_start, value_end, literal)) {
			value = literal;
		} else if (!TryReadNumericToken(sql, value_start, value_end, value)) {
			result += sql.substr(i, 8);
			i += 7;
			continue;
		}

		idx_t unit_start = SkipWhitespace(sql, value_end);
		idx_t unit_end = unit_start;
		string unit;
		if (!TryReadIdentifierToken(sql, unit_start, unit_end, unit)) {
			result += sql.substr(i, value_end - i);
			i = value_end - 1;
			continue;
		}
		if (!IsIntervalUnit(unit)) {
			ThrowUnsupportedInputDialectFeature(dialect, "interval", "unsupported interval unit '" + unit + "'");
		}

		idx_t next = SkipWhitespace(sql, unit_end);
		if (next < sql.size() && (IsIdentStart(sql[next]) || std::isdigit(static_cast<unsigned char>(sql[next])))) {
			ThrowUnsupportedInputDialectFeature(dialect, "interval", "compound intervals are not supported");
		}
		result += "INTERVAL " + SingleQuotedSqlString(value) + " " + SingularIntervalUnit(unit);
		i = unit_end - 1;
	}
	return result;
}

static string NormalizeCastType(const string &type_name, SqlDialect dialect) {
	string trimmed = TrimCopy(type_name);
	string normalized = LowerCopy(trimmed);
	if (normalized == "string" || normalized == "nvarchar" || normalized == "nchar") {
		return "VARCHAR";
	}
	if (normalized == "int64") {
		return "BIGINT";
	}
	if (normalized == "float64") {
		return "DOUBLE";
	}
	if (normalized == "bool") {
		return "BOOLEAN";
	}
	if (normalized == "datetime") {
		return "TIMESTAMP";
	}
	if (normalized == "numeric") {
		return "DECIMAL";
	}
	if (StringUtil::StartsWith(normalized, "number(")) {
		return "DECIMAL" + trimmed.substr(6);
	}
	if (StringUtil::StartsWith(normalized, "datetime64") || StringUtil::Contains(normalized, " unsigned")) {
		ThrowUnsupportedInputDialectFeature(dialect, "cast_type", "unsupported input type '" + trimmed + "'");
	}
	return trimmed;
}

static string RewriteCastTypes(const string &sql, SqlDialect dialect) {
	string result;
	for (idx_t i = 0; i < sql.size(); i++) {
		char c = sql[i];
		if (TryAppendSkippableSqlSpan(sql, i, result)) {
			continue;
		}
		if (!IsIdentStart(c)) {
			result += c;
			continue;
		}
		idx_t ident_end = i + 1;
		while (ident_end < sql.size() && IsIdentPart(sql[ident_end])) {
			ident_end++;
		}
		string ident = sql.substr(i, ident_end - i);
		bool is_cast = EqualsLowercase(ident, "cast");
		bool is_try_cast = EqualsLowercase(ident, "try_cast");
		idx_t open_pos = SkipWhitespace(sql, ident_end);
		if ((!is_cast && !is_try_cast) || open_pos >= sql.size() || sql[open_pos] != '(') {
			result += ident;
			i = ident_end - 1;
			continue;
		}
		idx_t close_pos = FindMatchingParen(sql, open_pos);
		if (close_pos == DConstants::INVALID_INDEX) {
			ThrowUnsupportedInputDialectFeature(dialect, ident, "could not find matching ')'");
		}
		string body = sql.substr(open_pos + 1, close_pos - open_pos - 1);
		idx_t as_pos = FindTopLevelAs(body);
		if (as_pos == DConstants::INVALID_INDEX) {
			result += sql.substr(i, close_pos - i + 1);
			i = close_pos;
			continue;
		}
		string expression = TrimCopy(body.substr(0, as_pos));
		string type_name = TrimCopy(body.substr(as_pos + 2));
		result += ident + "(" + expression + " AS " + NormalizeCastType(type_name, dialect) + ")";
		i = close_pos;
	}
	return result;
}

static idx_t FindTopLevelGroupBy(const string &sql, idx_t start = 0) {
	idx_t group_pos = FindTopLevelKeyword(sql, "group", start);
	if (group_pos == DConstants::INVALID_INDEX) {
		return DConstants::INVALID_INDEX;
	}
	idx_t by_pos = SkipWhitespace(sql, group_pos + 5);
	if (MatchesKeywordAt(sql, by_pos, "by")) {
		return group_pos;
	}
	return DConstants::INVALID_INDEX;
}

static idx_t NextClauseStart(const string &sql, idx_t start) {
	vector<idx_t> candidates;
	for (auto keyword : {"where", "group", "having", "qualify", "order", "limit", "union", "except", "intersect"}) {
		idx_t pos = FindTopLevelKeyword(sql, keyword, start);
		if (pos != DConstants::INVALID_INDEX) {
			candidates.push_back(pos);
		}
	}
	if (candidates.empty()) {
		return sql.size();
	}
	return *std::min_element(candidates.begin(), candidates.end());
}

static bool ContainsIdentifierReference(const string &sql, const string &identifier) {
	for (idx_t i = 0; i < sql.size(); i++) {
		idx_t end;
		if (TryReadSkippableSqlSpan(sql, i, end)) {
			i = end - 1;
			continue;
		}
		if (!IsIdentStart(sql[i])) {
			continue;
		}
		idx_t ident_end = i + 1;
		while (ident_end < sql.size() && IsIdentPart(sql[ident_end])) {
			ident_end++;
		}
		if (EqualsLowercase(sql.substr(i, ident_end - i), LowerCopy(identifier))) {
			return true;
		}
		i = ident_end - 1;
	}
	return false;
}

static bool TryExtractSelectAlias(const string &select_item, string &alias) {
	idx_t as_pos = FindTopLevelAs(select_item);
	if (as_pos == DConstants::INVALID_INDEX) {
		return false;
	}
	string candidate = TrimCopy(select_item.substr(as_pos + 2));
	if (!IsSafeIdentifierContent(candidate)) {
		return false;
	}
	alias = candidate;
	return true;
}

static void RejectRiskyAliasReferences(const string &sql, SqlDialect dialect) {
	if (!MatchesKeywordAt(sql, SkipWhitespace(sql, 0), "select")) {
		return;
	}
	idx_t from_pos = FindTopLevelKeyword(sql, "from");
	if (from_pos == DConstants::INVALID_INDEX) {
		return;
	}
	vector<string> aliases;
	for (auto &item :
	     SplitTopLevelArgs(sql.substr(SkipWhitespace(sql, 0) + 6, from_pos - (SkipWhitespace(sql, 0) + 6)))) {
		string alias;
		if (TryExtractSelectAlias(item, alias)) {
			aliases.push_back(alias);
		}
	}
	if (aliases.empty()) {
		return;
	}
	for (auto clause : {"where", "having", "qualify"}) {
		idx_t clause_pos = FindTopLevelKeyword(sql, clause, from_pos);
		if (clause_pos == DConstants::INVALID_INDEX) {
			continue;
		}
		idx_t clause_start = SkipWhitespace(sql, clause_pos + strlen(clause));
		string clause_body = sql.substr(clause_start, NextClauseStart(sql, clause_start) - clause_start);
		for (const auto &alias : aliases) {
			if (ContainsIdentifierReference(clause_body, alias)) {
				ThrowUnsupportedInputDialectFeature(dialect, "alias_semantics",
				                                    "select alias '" + alias + "' referenced from " + clause);
			}
		}
	}
	idx_t group_pos = FindTopLevelGroupBy(sql, from_pos);
	if (group_pos != DConstants::INVALID_INDEX) {
		idx_t clause_start = SkipWhitespace(sql, SkipWhitespace(sql, group_pos + 5) + 2);
		string clause_body = sql.substr(clause_start, NextClauseStart(sql, clause_start) - clause_start);
		for (const auto &alias : aliases) {
			if (ContainsIdentifierReference(clause_body, alias)) {
				ThrowUnsupportedInputDialectFeature(dialect, "alias_semantics",
				                                    "select alias '" + alias + "' referenced from group by");
			}
		}
	}
}

string NormalizeInputSqlToDuckDB(const string &query, SqlDialect dialect) {
	if (dialect == SqlDialect::DUCKDB) {
		return query;
	}

	string result = NormalizeBacktickIdentifiers(query, dialect);
	result = NormalizeBracketIdentifiers(result, dialect);
	result = RewriteTimeTravelClauses(result, dialect);
	result = RewriteIntervals(result, dialect);
	result = RewriteCastTypes(result, dialect);
	RejectRiskyAliasReferences(result, dialect);
	result = RewriteFunctionCalls(result, dialect);
	if (dialect == SqlDialect::MYSQL_MARIADB) {
		result = RewriteMysqlLimitComma(result, dialect);
		return result;
	}
	return result;
}

} // namespace duckdb
