#include "lpts_sql_scanner.hpp"
#include "lpts_helpers.hpp"

#include <cctype>

namespace duckdb {

bool IsIdentStart(char c) {
	return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool IsIdentPart(char c) {
	return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool EqualsLowercase(const string &value, const string &lowercase) {
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

bool MatchesKeywordAt(const string &sql, idx_t pos, const string &keyword) {
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

idx_t SkipWhitespace(const string &sql, idx_t pos) {
	while (pos < sql.size() && std::isspace(static_cast<unsigned char>(sql[pos]))) {
		pos++;
	}
	return pos;
}

string TrimCopy(string value) {
	StringUtil::Trim(value);
	return value;
}

string LowerCopy(const string &value) {
	return SQLToLowercase(value);
}

string SingleQuotedSqlString(const string &value) {
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

bool TryReadSingleQuotedLiteral(const string &sql, idx_t pos, idx_t &end, string &value) {
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

bool TryReadSkippableSqlSpan(const string &sql, idx_t pos, idx_t &end) {
	string literal;
	return TryReadSingleQuotedLiteral(sql, pos, end, literal) || TryReadDoubleQuotedIdentifier(sql, pos, end) ||
	       TryReadLineComment(sql, pos, end) || TryReadBlockComment(sql, pos, end);
}

bool TryAppendSkippableSqlSpan(const string &sql, idx_t &pos, string &result) {
	idx_t end;
	if (!TryReadSkippableSqlSpan(sql, pos, end)) {
		return false;
	}
	result += sql.substr(pos, end - pos);
	pos = end - 1;
	return true;
}

string QuoteDuckDBIdentifier(const string &identifier) {
	string result = "\"";
	for (char c : identifier) {
		if (c == '"') {
			result += "\"\"";
		} else {
			result += c;
		}
	}
	result += "\"";
	return result;
}

bool IsSafeIdentifierContent(const string &identifier) {
	if (identifier.empty() || !IsIdentStart(identifier[0])) {
		return false;
	}
	for (idx_t i = 1; i < identifier.size(); i++) {
		if (!IsIdentPart(identifier[i])) {
			return false;
		}
	}
	return true;
}

static idx_t PreviousNonWhitespace(const string &sql, idx_t pos) {
	while (pos > 0) {
		pos--;
		if (!std::isspace(static_cast<unsigned char>(sql[pos]))) {
			return pos;
		}
	}
	return DConstants::INVALID_INDEX;
}

bool CanStartBracketIdentifier(const string &sql, idx_t pos) {
	idx_t prev = PreviousNonWhitespace(sql, pos);
	if (prev == DConstants::INVALID_INDEX) {
		return true;
	}
	char c = sql[prev];
	if (IsIdentPart(c)) {
		idx_t start = prev;
		while (start > 0 && IsIdentPart(sql[start - 1])) {
			start--;
		}
		string token = LowerCopy(sql.substr(start, prev - start + 1));
		return token == "select" || token == "from" || token == "where" || token == "by" || token == "group" ||
		       token == "order" || token == "having" || token == "qualify" || token == "on" || token == "join" ||
		       token == "as" || token == "and" || token == "or";
	}
	if (c == ')' || c == ']' || c == '"' || c == '\'' || c == '`') {
		return false;
	}
	return true;
}

idx_t FindMatchingParen(const string &sql, idx_t open_pos) {
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

vector<string> SplitTopLevelArgs(const string &args) {
	if (TrimCopy(args).empty()) {
		return {};
	}
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

bool TryReadNumericToken(const string &sql, idx_t pos, idx_t &end, string &token) {
	idx_t start = pos;
	end = pos;
	while (end < sql.size() && (std::isdigit(static_cast<unsigned char>(sql[end])) || sql[end] == '.')) {
		end++;
	}
	if (end == start) {
		return false;
	}
	token = sql.substr(start, end - start);
	return true;
}

bool TryReadIdentifierToken(const string &sql, idx_t pos, idx_t &end, string &token) {
	if (pos >= sql.size() || !IsIdentStart(sql[pos])) {
		return false;
	}
	end = pos + 1;
	while (end < sql.size() && IsIdentPart(sql[end])) {
		end++;
	}
	token = sql.substr(pos, end - pos);
	return true;
}

idx_t FindTopLevelAs(const string &sql) {
	idx_t depth = 0;
	for (idx_t i = 0; i < sql.size(); i++) {
		idx_t end;
		if (TryReadSkippableSqlSpan(sql, i, end)) {
			i = end - 1;
			continue;
		}
		char c = sql[i];
		if (c == '(') {
			depth++;
		} else if (c == ')') {
			if (depth > 0) {
				depth--;
			}
		} else if (depth == 0 && MatchesKeywordAt(sql, i, "as")) {
			return i;
		}
	}
	return DConstants::INVALID_INDEX;
}

idx_t FindTopLevelKeyword(const string &sql, const string &keyword, idx_t start) {
	idx_t depth = 0;
	for (idx_t i = start; i < sql.size(); i++) {
		idx_t end;
		if (TryReadSkippableSqlSpan(sql, i, end)) {
			i = end - 1;
			continue;
		}
		char c = sql[i];
		if (c == '(') {
			depth++;
		} else if (c == ')') {
			if (depth > 0) {
				depth--;
			}
		} else if (depth == 0 && MatchesKeywordAt(sql, i, keyword)) {
			return i;
		}
	}
	return DConstants::INVALID_INDEX;
}

} // namespace duckdb
