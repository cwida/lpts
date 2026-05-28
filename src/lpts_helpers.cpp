#include "lpts_helpers.hpp"

#include "duckdb/parser/keyword_helper.hpp"

#include <regex>

namespace duckdb {

string DialectQuoteIdent(const string &name, SqlDialect dialect) {
	if (dialect == SqlDialect::SPARK || dialect == SqlDialect::HIVE || dialect == SqlDialect::BIGQUERY) {
		// Spark/Hive/BigQuery use backticks; embedded backticks must be doubled.
		std::ostringstream out;
		out << '`';
		for (char c : name) {
			if (c == '`') {
				out << '`' << '`';
			} else {
				out << c;
			}
		}
		out << '`';
		return out.str();
	}
	// DUCKDB / POSTGRES — fall back to DuckDB's helper, which only quotes when
	// the identifier is a reserved keyword or contains special chars.
	return KeywordHelper::WriteOptionallyQuoted(name);
}

string VecToSeparatedList(vector<string> input_list, const string &separator) {
	std::ostringstream ret_str;
	for (size_t i = 0; i < input_list.size(); ++i) {
		ret_str << input_list[i];
		if (i != input_list.size() - 1) {
			ret_str << separator;
		}
	}
	return ret_str.str();
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
	if (dialect == SqlDialect::BIGQUERY) {
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
	if (dialect == SqlDialect::HIVE) {
		if (schema.empty()) {
			return DialectQuoteTableWithOptionalSuffix(table_name, dialect);
		}
		return DialectQuoteIdent(schema, dialect) + "." + DialectQuoteTableWithOptionalSuffix(table_name, dialect);
	}
	return DialectQuoteIdent(catalog, dialect) + "." + DialectQuoteIdent(schema, dialect) + "." +
	       DialectQuoteTableWithOptionalSuffix(table_name, dialect);
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

} // namespace duckdb
