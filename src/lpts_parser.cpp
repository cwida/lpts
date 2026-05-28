#include "lpts_parser.hpp"
#include "lpts_helpers.hpp"

#include <cctype>

namespace duckdb {

SqlDialect ReadInputDialect(ClientContext &context) {
	Value dialect_val;
	if (context.TryGetCurrentSetting("lpts_input_dialect", dialect_val)) {
		return ParseSqlDialectSetting(dialect_val.GetValue<string>(), "lpts_input_dialect");
	}
	return SqlDialect::DUCKDB;
}

static bool IsIdentStart(char c) {
	return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

static bool IsIdentPart(char c) {
	return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

static bool EqualsLowercase(const string &value, const string &lowercase) {
	if (value.size() != lowercase.size()) {
		return false;
	}
	for (idx_t i = 0; i < value.size(); i++) {
		if (std::tolower(static_cast<unsigned char>(value[i])) != lowercase[i]) {
			return false;
		}
	}
	return true;
}

static bool MatchesKeywordAt(const string &sql, idx_t pos, const string &keyword) {
	if (pos > 0 && IsIdentPart(sql[pos - 1])) {
		return false;
	}
	if (pos + keyword.size() > sql.size()) {
		return false;
	}
	for (idx_t i = 0; i < keyword.size(); i++) {
		if (std::tolower(static_cast<unsigned char>(sql[pos + i])) != keyword[i]) {
			return false;
		}
	}
	return pos + keyword.size() >= sql.size() || !IsIdentPart(sql[pos + keyword.size()]);
}

static idx_t SkipWhitespace(const string &sql, idx_t pos) {
	while (pos < sql.size() && std::isspace(static_cast<unsigned char>(sql[pos]))) {
		pos++;
	}
	return pos;
}

static string TrimCopy(string value) {
	StringUtil::Trim(value);
	return value;
}

static string SingleQuotedSqlString(const string &value) {
	string result = "'";
	for (char c : value) {
		if (c == '\'') {
			result += "''";
		} else {
			result += c;
		}
	}
	result += "'";
	return result;
}

static bool TryReadSingleQuotedLiteral(const string &sql, idx_t pos, idx_t &end, string &value) {
	if (pos >= sql.size() || sql[pos] != '\'') {
		return false;
	}
	value.clear();
	pos++;
	while (pos < sql.size()) {
		char c = sql[pos++];
		if (c == '\'') {
			if (pos < sql.size() && sql[pos] == '\'') {
				value += '\'';
				pos++;
				continue;
			}
			end = pos;
			return true;
		}
		value += c;
	}
	return false;
}

static bool TryReadDoubleQuotedIdentifier(const string &sql, idx_t pos, idx_t &end) {
	if (pos >= sql.size() || sql[pos] != '"') {
		return false;
	}
	pos++;
	while (pos < sql.size()) {
		if (sql[pos] == '"') {
			if (pos + 1 < sql.size() && sql[pos + 1] == '"') {
				pos += 2;
				continue;
			}
			end = pos + 1;
			return true;
		}
		pos++;
	}
	return false;
}

static bool TryReadLineComment(const string &sql, idx_t pos, idx_t &end) {
	if (pos + 1 >= sql.size() || sql[pos] != '-' || sql[pos + 1] != '-') {
		return false;
	}
	end = pos + 2;
	while (end < sql.size() && sql[end] != '\n' && sql[end] != '\r') {
		end++;
	}
	return true;
}

static bool TryReadBlockComment(const string &sql, idx_t pos, idx_t &end) {
	if (pos + 1 >= sql.size() || sql[pos] != '/' || sql[pos + 1] != '*') {
		return false;
	}
	end = pos + 2;
	while (end + 1 < sql.size() && !(sql[end] == '*' && sql[end + 1] == '/')) {
		end++;
	}
	end = MinValue<idx_t>(end + 2, sql.size());
	return true;
}

static bool TryReadSkippableSqlSpan(const string &sql, idx_t pos, idx_t &end) {
	string literal;
	return TryReadSingleQuotedLiteral(sql, pos, end, literal) || TryReadDoubleQuotedIdentifier(sql, pos, end) ||
	       TryReadLineComment(sql, pos, end) || TryReadBlockComment(sql, pos, end);
}

static bool TryAppendSkippableSqlSpan(const string &sql, idx_t &pos, string &result) {
	idx_t end;
	if (!TryReadSkippableSqlSpan(sql, pos, end)) {
		return false;
	}
	result += sql.substr(pos, end - pos);
	pos = end - 1;
	return true;
}

static void ThrowUnsupportedInputDialectFeature(SqlDialect dialect, const string &feature_name, const string &reason) {
	throw NotImplementedException("LPTS_UNSUPPORTED_INPUT_DIALECT_FEATURE: dialect=%s feature=%s reason=%s",
	                              SqlDialectToString(dialect), feature_name, reason);
}

enum class InputDateFormatStyle : uint8_t { MYSQL_PERCENT, BIGQUERY_PERCENT, JAVA, POSTGRES, SNOWFLAKE };

static string ConvertPercentDateFormatToDuckDB(const string &format, SqlDialect dialect) {
	string result;
	for (idx_t i = 0; i < format.size(); i++) {
		if (format[i] != '%') {
			result += format[i];
			continue;
		}
		if (i + 1 >= format.size()) {
			ThrowUnsupportedInputDialectFeature(dialect, "date_format_token", "trailing '%' has no DuckDB equivalent");
		}
		char specifier = format[++i];
		switch (specifier) {
		case 'Y':
		case 'y':
		case 'm':
		case 'd':
		case 'H':
		case 'I':
		case 'f':
		case 'j':
		case 'p':
		case 'U':
		case 'w':
		case '%':
			result += "%";
			result += specifier;
			break;
		case 'i':
			result += "%M";
			break;
		case 's':
		case 'S':
			result += "%S";
			break;
		case 'b':
			result += "%b";
			break;
		case 'M':
			result += "%B";
			break;
		default:
			ThrowUnsupportedInputDialectFeature(dialect, "%" + string(1, specifier),
			                                    "no verified DuckDB date format equivalent");
		}
	}
	return result;
}

static string ConvertBigQueryPercentDateFormatToDuckDB(const string &format, SqlDialect dialect) {
	string result;
	for (idx_t i = 0; i < format.size(); i++) {
		if (format[i] != '%') {
			result += format[i];
			continue;
		}
		if (i + 1 >= format.size()) {
			ThrowUnsupportedInputDialectFeature(dialect, "date_format_token", "trailing '%' has no DuckDB equivalent");
		}
		char specifier = format[++i];
		switch (specifier) {
		case 'Y':
		case 'y':
		case 'm':
		case 'd':
		case 'H':
		case 'I':
		case 'M':
		case 'S':
		case 'f':
		case 'j':
		case 'p':
		case 'U':
		case 'w':
		case '%':
			result += "%";
			result += specifier;
			break;
		case 'b':
			result += "%b";
			break;
		case 'B':
			result += "%B";
			break;
		default:
			ThrowUnsupportedInputDialectFeature(dialect, "%" + string(1, specifier),
			                                    "no verified DuckDB date format equivalent");
		}
	}
	return result;
}

static bool MatchesCaseInsensitiveToken(const string &format, idx_t pos, const string &token) {
	if (pos + token.size() > format.size()) {
		return false;
	}
	for (idx_t i = 0; i < token.size(); i++) {
		if (std::tolower(static_cast<unsigned char>(format[pos + i])) !=
		    std::tolower(static_cast<unsigned char>(token[i]))) {
			return false;
		}
	}
	return true;
}

static string ConvertTokenDateFormatToDuckDB(const string &format, SqlDialect dialect,
                                             const vector<pair<string, string>> &tokens, bool case_sensitive) {
	string result;
	for (idx_t i = 0; i < format.size();) {
		bool matched = false;
		for (const auto &token : tokens) {
			bool token_matches = case_sensitive ? format.substr(i, token.first.size()) == token.first
			                                    : MatchesCaseInsensitiveToken(format, i, token.first);
			if (token_matches) {
				result += token.second;
				i += token.first.size();
				matched = true;
				break;
			}
		}
		if (matched) {
			continue;
		}
		if (std::isalpha(static_cast<unsigned char>(format[i]))) {
			ThrowUnsupportedInputDialectFeature(dialect, "date_format_token",
			                                    "no verified DuckDB equivalent for token near '" +
			                                        format.substr(i, MinValue<idx_t>(8, format.size() - i)) + "'");
		}
		result += format[i++];
	}
	return result;
}

static string ConvertJavaDateFormatToDuckDB(const string &format, SqlDialect dialect) {
	static const vector<pair<string, string>> tokens = {
	    {"SSSSSS", "%f"}, {"yyyy", "%Y"}, {"YYYY", "%Y"}, {"MMMM", "%B"}, {"MMM", "%b"}, {"EEE", "%a"},
	    {"SSS", "%f"},    {"yy", "%y"},   {"YY", "%y"},   {"MM", "%m"},   {"dd", "%d"},  {"HH", "%H"},
	    {"hh", "%I"},     {"mm", "%M"},   {"ss", "%S"},   {"a", "%p"}};
	return ConvertTokenDateFormatToDuckDB(format, dialect, tokens, true);
}

static string ConvertPostgresDateFormatToDuckDB(const string &format, SqlDialect dialect) {
	static const vector<pair<string, string>> tokens = {{"YYYY", "%Y"}, {"HH24", "%H"}, {"HH12", "%I"}, {"MONTH", "%B"},
	                                                    {"MON", "%b"},  {"MI", "%M"},   {"SS", "%S"},   {"US", "%f"},
	                                                    {"YY", "%y"},   {"MM", "%m"},   {"DD", "%d"}};
	return ConvertTokenDateFormatToDuckDB(format, dialect, tokens, false);
}

static string ConvertSnowflakeDateFormatToDuckDB(const string &format, SqlDialect dialect) {
	static const vector<pair<string, string>> tokens = {{"YYYY", "%Y"}, {"HH24", "%H"}, {"HH12", "%I"}, {"MONTH", "%B"},
	                                                    {"MON", "%b"},  {"FF6", "%f"},  {"FF", "%f"},   {"MI", "%M"},
	                                                    {"SS", "%S"},   {"YY", "%y"},   {"MM", "%m"},   {"DD", "%d"}};
	return ConvertTokenDateFormatToDuckDB(format, dialect, tokens, false);
}

static string ConvertInputDateFormatToDuckDB(const string &format, SqlDialect dialect, InputDateFormatStyle style) {
	switch (style) {
	case InputDateFormatStyle::MYSQL_PERCENT:
		return ConvertPercentDateFormatToDuckDB(format, dialect);
	case InputDateFormatStyle::BIGQUERY_PERCENT:
		return ConvertBigQueryPercentDateFormatToDuckDB(format, dialect);
	case InputDateFormatStyle::JAVA:
		return ConvertJavaDateFormatToDuckDB(format, dialect);
	case InputDateFormatStyle::POSTGRES:
		return ConvertPostgresDateFormatToDuckDB(format, dialect);
	case InputDateFormatStyle::SNOWFLAKE:
		return ConvertSnowflakeDateFormatToDuckDB(format, dialect);
	default:
		throw InternalException("unknown input date format style");
	}
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

static idx_t FindMatchingParen(const string &sql, idx_t open_pos) {
	idx_t depth = 0;
	for (idx_t i = open_pos; i < sql.size(); i++) {
		char c = sql[i];
		idx_t end;
		if (TryReadSkippableSqlSpan(sql, i, end)) {
			i = end - 1;
			continue;
		}
		if (c == '\'' || c == '"') {
			return DConstants::INVALID_INDEX;
		}
		if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
			return DConstants::INVALID_INDEX;
		}
		if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
			return DConstants::INVALID_INDEX;
		}
		if (c == '`') {
			continue;
		}
		if (c == '(') {
			depth++;
		} else if (c == ')') {
			if (depth == 0) {
				return DConstants::INVALID_INDEX;
			}
			depth--;
			if (depth == 0) {
				return i;
			}
		}
	}
	return DConstants::INVALID_INDEX;
}

static vector<string> SplitTopLevelArgs(const string &args) {
	vector<string> result;
	idx_t depth = 0;
	idx_t start = 0;
	for (idx_t i = 0; i < args.size(); i++) {
		char c = args[i];
		idx_t end;
		if (TryReadSkippableSqlSpan(args, i, end)) {
			i = end - 1;
			continue;
		}
		if (c == '\'' || c == '"') {
			throw ParserException("Unterminated quoted span in dialect-normalized function call");
		}
		if (c == '(') {
			depth++;
		} else if (c == ')') {
			if (depth > 0) {
				depth--;
			}
		} else if (c == ',' && depth == 0) {
			result.push_back(TrimCopy(args.substr(start, i - start)));
			start = i + 1;
		}
	}
	result.push_back(TrimCopy(args.substr(start)));
	return result;
}

static string RewriteFunctionCalls(const string &sql, SqlDialect dialect, const string &source_name,
                                   const string &target_name, bool convert_format_arg,
                                   InputDateFormatStyle format_style = InputDateFormatStyle::MYSQL_PERCENT,
                                   idx_t format_arg_index = 1, bool swap_two_args = false,
                                   bool cast_result_to_date = false) {
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
		if (!EqualsLowercase(ident, source_name) || open_pos >= sql.size() || sql[open_pos] != '(') {
			result += ident;
			i = ident_end - 1;
			continue;
		}

		idx_t close_pos = FindMatchingParen(sql, open_pos);
		if (close_pos == DConstants::INVALID_INDEX) {
			ThrowUnsupportedInputDialectFeature(dialect, source_name, "could not find matching ')'");
		}
		auto args = SplitTopLevelArgs(sql.substr(open_pos + 1, close_pos - open_pos - 1));
		if (convert_format_arg) {
			if (args.size() != 2) {
				ThrowUnsupportedInputDialectFeature(dialect, source_name,
				                                    "date format function rewrite expects exactly two arguments");
			}
			if (format_arg_index >= args.size()) {
				throw InternalException("date format argument index out of range");
			}
			idx_t literal_end;
			string format;
			if (!TryReadSingleQuotedLiteral(args[format_arg_index], 0, literal_end, format) ||
			    SkipWhitespace(args[format_arg_index], literal_end) != args[format_arg_index].size()) {
				ThrowUnsupportedInputDialectFeature(dialect, source_name,
				                                    "date format argument must be a string literal");
			}
			args[format_arg_index] =
			    SingleQuotedSqlString(ConvertInputDateFormatToDuckDB(format, dialect, format_style));
		}
		string rewritten_call;
		if (swap_two_args) {
			if (args.size() != 2) {
				ThrowUnsupportedInputDialectFeature(dialect, source_name,
				                                    "argument reordering expects exactly two arguments");
			}
			rewritten_call = target_name + "(" + args[1] + ", " + args[0] + ")";
		} else {
			rewritten_call = target_name + "(" + VecToSeparatedList(args, ", ") + ")";
		}
		if (cast_result_to_date) {
			rewritten_call = "CAST(" + rewritten_call + " AS DATE)";
		}
		result += rewritten_call;
		i = close_pos;
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

string NormalizeInputSqlToDuckDB(const string &query, SqlDialect dialect) {
	if (dialect == SqlDialect::DUCKDB) {
		return query;
	}

	string result = NormalizeBacktickIdentifiers(query, dialect);
	result = RewriteFunctionCalls(result, dialect, "ifnull", "coalesce", false);
	result = RewriteFunctionCalls(result, dialect, "nvl", "coalesce", false);
	if (dialect == SqlDialect::MYSQL_MARIADB) {
		result = RewriteFunctionCalls(result, dialect, "date_format", "strftime", true);
		result = RewriteFunctionCalls(result, dialect, "str_to_date", "strptime", true);
		result = RewriteMysqlLimitComma(result, dialect);
		return result;
	}
	if (dialect == SqlDialect::TRINO_PRESTO) {
		result = RewriteFunctionCalls(result, dialect, "date_format", "strftime", true);
		result = RewriteFunctionCalls(result, dialect, "date_parse", "strptime", true);
		return result;
	}
	if (dialect == SqlDialect::SPARK || dialect == SqlDialect::HIVE) {
		result = RewriteFunctionCalls(result, dialect, "date_format", "strftime", true, InputDateFormatStyle::JAVA);
		result = RewriteFunctionCalls(result, dialect, "to_timestamp", "strptime", true, InputDateFormatStyle::JAVA);
		result = RewriteFunctionCalls(result, dialect, "to_date", "strptime", true, InputDateFormatStyle::JAVA, 1,
		                              false, true);
		return result;
	}
	if (dialect == SqlDialect::POSTGRES || dialect == SqlDialect::REDSHIFT) {
		result = RewriteFunctionCalls(result, dialect, "to_char", "strftime", true, InputDateFormatStyle::POSTGRES);
		result =
		    RewriteFunctionCalls(result, dialect, "to_timestamp", "strptime", true, InputDateFormatStyle::POSTGRES);
		result = RewriteFunctionCalls(result, dialect, "to_date", "strptime", true, InputDateFormatStyle::POSTGRES, 1,
		                              false, true);
		return result;
	}
	if (dialect == SqlDialect::SNOWFLAKE) {
		result = RewriteFunctionCalls(result, dialect, "to_char", "strftime", true, InputDateFormatStyle::SNOWFLAKE);
		result =
		    RewriteFunctionCalls(result, dialect, "to_timestamp", "strptime", true, InputDateFormatStyle::SNOWFLAKE);
		result = RewriteFunctionCalls(result, dialect, "to_date", "strptime", true, InputDateFormatStyle::SNOWFLAKE, 1,
		                              false, true);
		return result;
	}
	if (dialect == SqlDialect::BIGQUERY) {
		result = RewriteFunctionCalls(result, dialect, "format_timestamp", "strftime", true,
		                              InputDateFormatStyle::BIGQUERY_PERCENT, 0, true);
		result = RewriteFunctionCalls(result, dialect, "format_date", "strftime", true,
		                              InputDateFormatStyle::BIGQUERY_PERCENT, 0, true);
		result = RewriteFunctionCalls(result, dialect, "parse_timestamp", "strptime", true,
		                              InputDateFormatStyle::BIGQUERY_PERCENT, 0, true);
		result = RewriteFunctionCalls(result, dialect, "parse_date", "strptime", true,
		                              InputDateFormatStyle::BIGQUERY_PERCENT, 0, true, true);
		return result;
	}
	return result;
}

} // namespace duckdb
