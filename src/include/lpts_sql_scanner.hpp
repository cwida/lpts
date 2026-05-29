#pragma once

#include "duckdb.hpp"

namespace duckdb {

bool IsIdentStart(char c);
bool IsIdentPart(char c);
bool EqualsLowercase(const string &value, const string &lowercase);
bool MatchesKeywordAt(const string &sql, idx_t pos, const string &keyword);
idx_t SkipWhitespace(const string &sql, idx_t pos);
string TrimCopy(string value);
string LowerCopy(const string &value);
string SingleQuotedSqlString(const string &value);
bool TryReadSingleQuotedLiteral(const string &sql, idx_t pos, idx_t &end, string &value);
bool TryReadSkippableSqlSpan(const string &sql, idx_t pos, idx_t &end);
bool TryAppendSkippableSqlSpan(const string &sql, idx_t &pos, string &result);
string QuoteDuckDBIdentifier(const string &identifier);
bool IsSafeIdentifierContent(const string &identifier);
bool CanStartBracketIdentifier(const string &sql, idx_t pos);
idx_t FindMatchingParen(const string &sql, idx_t open_pos);
vector<string> SplitTopLevelArgs(const string &args);
bool TryReadNumericToken(const string &sql, idx_t pos, idx_t &end, string &token);
bool TryReadIdentifierToken(const string &sql, idx_t pos, idx_t &end, string &token);
idx_t FindTopLevelAs(const string &sql);
idx_t FindTopLevelKeyword(const string &sql, const string &keyword, idx_t start = 0);

} // namespace duckdb
