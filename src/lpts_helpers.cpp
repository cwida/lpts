#include "lpts_helpers.hpp"

#include "duckdb/parser/keyword_helper.hpp"

#include <algorithm>
#include <cctype>
#include <regex>

namespace duckdb {

string VecToSeparatedList(const vector<string> &input_list, const string &separator) {
	std::ostringstream ret_str;
	for (size_t i = 0; i < input_list.size(); ++i) {
		ret_str << input_list[i];
		if (i != input_list.size() - 1) {
			ret_str << separator;
		}
	}
	return ret_str.str();
}

string JoinConditionsToSQL(const vector<string> &conditions) {
	if (conditions.empty()) {
		return "TRUE";
	}
	return VecToSeparatedList(conditions, " AND ");
}

string QuoteIdentifier(const string &identifier) {
	return KeywordHelper::WriteOptionallyQuoted(identifier);
}

string VecToQuotedIdentifierList(const vector<string> &input_list, const string &separator) {
	std::ostringstream ret_str;
	for (size_t i = 0; i < input_list.size(); ++i) {
		ret_str << QuoteIdentifier(input_list[i]);
		if (i != input_list.size() - 1) {
			ret_str << separator;
		}
	}
	return ret_str.str();
}

string QuoteTableWithOptionalSuffix(const string &table_name) {
	static const string at_suffix = " AT (";
	auto suffix_pos = table_name.find(at_suffix);
	if (suffix_pos == string::npos) {
		return QuoteIdentifier(table_name);
	}
	return QuoteIdentifier(table_name.substr(0, suffix_pos)) + table_name.substr(suffix_pos);
}

string QualifiedTableName(const string &catalog, const string &schema, const string &table_name) {
	return QuoteIdentifier(catalog) + "." + QuoteIdentifier(schema) + "." + QuoteTableWithOptionalSuffix(table_name);
}

string DialectVecToQuotedIdentifierList(const vector<string> &input_list, SqlDialect dialect, const string &separator) {
	std::ostringstream ret_str;
	for (size_t i = 0; i < input_list.size(); ++i) {
		ret_str << DialectQuoteIdent(input_list[i], dialect);
		if (i != input_list.size() - 1) {
			ret_str << separator;
		}
	}
	return ret_str.str();
}

string DialectQuoteTableWithOptionalSuffix(const string &table_name, SqlDialect dialect) {
	static const string at_suffix = " AT (";
	auto suffix_pos = table_name.find(at_suffix);
	if (suffix_pos == string::npos) {
		return DialectQuoteIdent(table_name, dialect);
	}
	return DialectQuoteIdent(table_name.substr(0, suffix_pos), dialect) + table_name.substr(suffix_pos);
}

string DialectQualifiedTableName(const string &catalog, const string &schema, const string &table_name,
                                 SqlDialect dialect) {
	if (DialectUsesSingleQuotedTablePath(dialect)) {
		string table_path;
		if (!catalog.empty()) {
			table_path += catalog;
		}
		if (!schema.empty()) {
			if (!table_path.empty()) {
				table_path += ".";
			}
			table_path += schema;
		}
		if (!table_path.empty()) {
			table_path += ".";
		}
		table_path += table_name;
		return DialectQuoteTableWithOptionalSuffix(table_path, dialect);
	}
	if (DialectUsesSchemaQualifiedTableNames(dialect)) {
		if (schema.empty()) {
			return DialectQuoteTableWithOptionalSuffix(table_name, dialect);
		}
		return DialectQuoteIdent(schema, dialect) + "." + DialectQuoteTableWithOptionalSuffix(table_name, dialect);
	}
	return DialectQuoteIdent(catalog, dialect) + "." + DialectQuoteIdent(schema, dialect) + "." +
	       DialectQuoteTableWithOptionalSuffix(table_name, dialect);
}

[[noreturn]] void ThrowLptsNotImplemented(const string &code, SqlDialect dialect, const string &feature_kind,
                                          const string &feature_name, const string &context, const string &reason) {
	throw NotImplementedException("%s: dialect=%s feature=%s name=%s context=%s reason=%s", code,
	                              SqlDialectToString(dialect), feature_kind, feature_name, context, reason);
}

string EscapeSingleQuotes(const string &input) {
	std::stringstream escaped_stream;
	for (char c : input) {
		if (c == '\'') {
			escaped_stream << "''";
		} else {
			escaped_stream << c;
		}
	}
	return escaped_stream.str();
}

string SQLToLowercase(const string &sql) {
	std::stringstream lowercase_stream;
	bool in_string = false;
	for (char c : sql) {
		if (c == '\'') {
			in_string = !in_string;
		}
		if (!in_string) {
			lowercase_stream << (char)tolower(c);
		} else {
			lowercase_stream << c;
		}
	}
	return lowercase_stream.str();
}

void RemoveRedundantWhitespaces(string &query) {
	query = std::regex_replace(query, std::regex("\\s+"), " ");
}

namespace {
bool IsIdentifierChar(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}
bool IsIdentifierStart(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
} // namespace

bool HasGeneratedTablePrefix(const string &name) {
	if (name.size() < 3 || name[0] != 't') {
		return false;
	}
	size_t i = 1;
	while (i < name.size() && name[i] >= '0' && name[i] <= '9') {
		i++;
	}
	return i > 1 && i < name.size() && name[i] == '_';
}

string StripGeneratedTablePrefix(const string &name) {
	if (!HasGeneratedTablePrefix(name)) {
		return name;
	}
	return name.substr(name.find('_') + 1);
}

string SwallowSelfAlias(const string &sql) {
	// Tokenize: single-quoted strings ('o', skipped as operands), double-quoted identifiers ('i'),
	// bare identifiers ('i'), whitespace ('s'), everything else ('o').
	struct Tok {
		char kind;
		string text;
	};
	vector<Tok> toks;
	const size_t n = sql.size();
	size_t i = 0;
	while (i < n) {
		const char c = sql[i];
		if (c == '\'' || c == '"') {
			const char quote = c;
			const size_t start = i++;
			while (i < n) {
				if (sql[i] == quote) {
					if (i + 1 < n && sql[i + 1] == quote) {
						i += 2;
						continue;
					}
					i++;
					break;
				}
				i++;
			}
			toks.push_back({quote == '"' ? 'i' : 'o', sql.substr(start, i - start)});
		} else if (IsIdentifierStart(c)) {
			const size_t start = i++;
			while (i < n && IsIdentifierChar(sql[i])) {
				i++;
			}
			toks.push_back({'i', sql.substr(start, i - start)});
		} else if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
			const size_t start = i++;
			while (i < n && (sql[i] == ' ' || sql[i] == '\n' || sql[i] == '\t' || sql[i] == '\r')) {
				i++;
			}
			toks.push_back({'s', sql.substr(start, i - start)});
		} else {
			toks.push_back({'o', string(1, c)});
			i++;
		}
	}
	// Collapse "<ident A> AS <ident B>" to "<ident A>" when A and B are textually identical.
	vector<bool> drop(toks.size(), false);
	for (size_t k = 0; k < toks.size(); k++) {
		if (toks[k].kind != 'i' || toks[k].text != "AS") {
			continue;
		}
		long p = static_cast<long>(k) - 1;
		while (p >= 0 && toks[p].kind == 's') {
			p--;
		}
		long q = static_cast<long>(k) + 1;
		while (q < static_cast<long>(toks.size()) && toks[q].kind == 's') {
			q++;
		}
		if (p >= 0 && q < static_cast<long>(toks.size()) && toks[p].kind == 'i' && toks[q].kind == 'i' &&
		    toks[p].text == toks[q].text) {
			for (long m = p + 1; m <= q; m++) {
				drop[m] = true;
			}
		}
	}
	string out;
	out.reserve(sql.size());
	for (size_t k = 0; k < toks.size(); k++) {
		if (!drop[k]) {
			out += toks[k].text;
		}
	}
	return out;
}

string SubstituteColumnTokens(const string &sql, const std::unordered_map<string, string> &replacements) {
	if (replacements.empty()) {
		return sql;
	}
	std::string out;
	out.reserve(sql.size());
	const size_t n = sql.size();
	size_t i = 0;
	while (i < n) {
		const char c = sql[i];
		// Copy quoted string / identifier literals verbatim (handles doubled-quote escapes).
		if (c == '\'' || c == '"') {
			const char quote = c;
			out.push_back(c);
			i++;
			while (i < n) {
				out.push_back(sql[i]);
				if (sql[i] == quote) {
					// Doubled quote ('' or "") is an escaped quote — stay inside the literal.
					if (i + 1 < n && sql[i + 1] == quote) {
						out.push_back(sql[i + 1]);
						i += 2;
						continue;
					}
					i++;
					break;
				}
				i++;
			}
			continue;
		}
		// Greedily consume a maximal identifier token and try to replace it as a whole.
		if (IsIdentifierStart(c)) {
			const size_t start = i;
			i++;
			while (i < n && IsIdentifierChar(sql[i])) {
				i++;
			}
			const string token = sql.substr(start, i - start);
			auto it = replacements.find(token);
			// Do NOT rescan the inserted replacement: append it as-is (single pass, no cascade).
			out += (it != replacements.end()) ? it->second : token;
			continue;
		}
		out.push_back(c);
		i++;
	}
	return out;
}

//===--------------------------------------------------------------------===//
// Nondeterminism heuristic (shared by the lpts_check mode and the SQLStorm benchmark).
//===--------------------------------------------------------------------===//

static string LowerASCII(string input) {
	std::transform(input.begin(), input.end(), input.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return input;
}

static string NormalizeWhitespaceASCII(const string &input) {
	string result;
	bool last_was_space = true;
	for (char c : input) {
		if (std::isspace(static_cast<unsigned char>(c))) {
			if (!last_was_space) {
				result += ' ';
				last_was_space = true;
			}
			continue;
		}
		result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		last_was_space = false;
	}
	if (!result.empty() && result.back() != ' ') {
		result += ' ';
	}
	return " " + result;
}

static bool ContainsNormalizedPhrase(const string &sql, const string &phrase) {
	return NormalizeWhitespaceASCII(sql).find(NormalizeWhitespaceASCII(phrase)) != string::npos;
}

static bool HasFunctionCall(const string &sql, const string &function_name) {
	string lower_sql = LowerASCII(sql);
	string needle = LowerASCII(function_name) + "(";
	return lower_sql.find(needle) != string::npos;
}

static bool HasWindowFunctionCall(const string &sql, const string &function_name) {
	return HasFunctionCall(sql, function_name) && ContainsNormalizedPhrase(sql, "over");
}

bool IsLikelyNondeterministicSQL(const string &sql, string &reason) {
	// Order-sensitive aggregates (string_agg, group_concat, listagg, list, array_agg) concatenate/collect
	// their inputs in an order that is only fully defined when an in-aggregate ORDER BY names a UNIQUE key.
	// We cannot prove uniqueness from the SQL text, and a non-total ORDER BY (or none at all) leaves the
	// result order input-dependent — so the original and LPTS-rewritten plans may legitimately disagree on
	// the concatenation order. Treat any use as potentially nondeterministic, ordered or not.
	if (HasFunctionCall(sql, "string_agg") || HasFunctionCall(sql, "group_concat")) {
		reason = "order-sensitive aggregate (string_agg/group_concat) may have a non-total ordering";
		return true;
	}
	if (HasFunctionCall(sql, "listagg")) {
		reason = "order-sensitive aggregate (listagg) may have a non-total ordering";
		return true;
	}
	if (HasFunctionCall(sql, "list") || HasFunctionCall(sql, "array_agg")) {
		reason = "order-sensitive aggregate (list/array_agg) may have a non-total ordering";
		return true;
	}
	if (HasFunctionCall(sql, "random")) {
		reason = "volatile random() expression";
		return true;
	}
	if (HasWindowFunctionCall(sql, "row_number")) {
		reason = "row_number over potentially tied ordering keys";
		return true;
	}
	if (HasWindowFunctionCall(sql, "rank")) {
		reason = "rank over potentially tied ordering keys";
		return true;
	}
	if (HasWindowFunctionCall(sql, "dense_rank")) {
		reason = "dense_rank over potentially tied ordering keys";
		return true;
	}
	if (HasWindowFunctionCall(sql, "lag") || HasWindowFunctionCall(sql, "lead") ||
	    HasWindowFunctionCall(sql, "first_value") || HasWindowFunctionCall(sql, "last_value") ||
	    HasWindowFunctionCall(sql, "nth_value")) {
		reason = "window function over potentially tied ordering keys";
		return true;
	}
	if (ContainsNormalizedPhrase(sql, "limit") && ContainsNormalizedPhrase(sql, "order by")) {
		reason = "ORDER BY with LIMIT/OFFSET may have tied boundary rows";
		return true;
	}
	if (ContainsNormalizedPhrase(sql, "fetch first") && ContainsNormalizedPhrase(sql, "order by")) {
		reason = "ORDER BY with FETCH FIRST may have tied boundary rows";
		return true;
	}
	if (ContainsNormalizedPhrase(sql, "fetch next") && ContainsNormalizedPhrase(sql, "order by")) {
		reason = "ORDER BY with FETCH NEXT may have tied boundary rows";
		return true;
	}
	if (ContainsNormalizedPhrase(sql, "offset") && ContainsNormalizedPhrase(sql, "order by")) {
		reason = "ORDER BY with OFFSET may have tied boundary rows";
		return true;
	}
	if (HasFunctionCall(sql, "avg") || HasFunctionCall(sql, "stddev") || HasFunctionCall(sql, "stddev_pop") ||
	    HasFunctionCall(sql, "stddev_samp") || HasFunctionCall(sql, "variance") || HasFunctionCall(sql, "var_pop") ||
	    HasFunctionCall(sql, "var_samp")) {
		reason = "strict floating aggregate equality may depend on evaluation order";
		return true;
	}
	return false;
}

} // namespace duckdb
