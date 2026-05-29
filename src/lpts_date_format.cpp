#include "lpts_date_format.hpp"
#include "lpts_helpers.hpp"

#include <cctype>

namespace duckdb {

static void ThrowUnsupportedInputDialectFeature(SqlDialect dialect, const string &feature_name, const string &reason) {
	throw NotImplementedException("LPTS_UNSUPPORTED_INPUT_DIALECT_FEATURE: dialect=%s feature=%s reason=%s",
	                              SqlDialectToString(dialect), feature_name, reason);
}

static bool UsesJavaDateFormat(SqlDialect dialect) {
	return dialect == SqlDialect::SPARK || dialect == SqlDialect::HIVE;
}

static bool UsesPostgresDateFormat(SqlDialect dialect) {
	return dialect == SqlDialect::POSTGRES || dialect == SqlDialect::REDSHIFT;
}

static bool UsesSnowflakeDateFormat(SqlDialect dialect) {
	return dialect == SqlDialect::SNOWFLAKE;
}

static bool UsesMySQLMariaDBDateFormat(SqlDialect dialect) {
	return dialect == SqlDialect::MYSQL_MARIADB;
}

static bool IsDuckDBDialect(SqlDialect dialect) {
	return dialect == SqlDialect::DUCKDB;
}

static void ThrowUnsupportedDateFormatToken(SqlDialect dialect, char specifier) {
	string token = specifier == '\0' ? "<trailing %>" : "%" + string(1, specifier);
	ThrowLptsNotImplemented("LPTS_UNSUPPORTED_DATE_FORMAT_TOKEN", dialect, "date_format_token", token,
	                        "date format conversion", "no verified equivalent token for target dialect");
}

static string ConvertDuckDBDateFormatToJava(const string &format, SqlDialect dialect) {
	string result;
	for (idx_t i = 0; i < format.size(); i++) {
		if (format[i] != '%') {
			result += format[i];
			continue;
		}
		if (i + 1 >= format.size()) {
			ThrowUnsupportedDateFormatToken(dialect, '\0');
		}
		char specifier = format[++i];
		switch (specifier) {
		case 'Y':
			result += "yyyy";
			break;
		case 'y':
			result += "yy";
			break;
		case 'm':
			result += "MM";
			break;
		case 'd':
			result += "dd";
			break;
		case 'H':
			result += "HH";
			break;
		case 'I':
			result += "hh";
			break;
		case 'M':
			result += "mm";
			break;
		case 'S':
			result += "ss";
			break;
		case 'f':
			result += "SSSSSS";
			break;
		case 'z':
			result += "Z";
			break;
		case 'Z':
			result += "z";
			break;
		case 'a':
			result += "EEE";
			break;
		case 'A':
			result += "EEEE";
			break;
		case 'b':
			result += "MMM";
			break;
		case 'B':
			result += "MMMM";
			break;
		case '%':
			result += "%";
			break;
		default:
			ThrowUnsupportedDateFormatToken(dialect, specifier);
		}
	}
	return result;
}

static string ConvertDuckDBDateFormatToPostgres(const string &format, SqlDialect dialect) {
	string result;
	for (idx_t i = 0; i < format.size(); i++) {
		if (format[i] != '%') {
			result += format[i];
			continue;
		}
		if (i + 1 >= format.size()) {
			ThrowUnsupportedDateFormatToken(dialect, '\0');
		}
		char specifier = format[++i];
		switch (specifier) {
		case 'Y':
			result += "YYYY";
			break;
		case 'y':
			result += "YY";
			break;
		case 'm':
			result += "MM";
			break;
		case 'd':
			result += "DD";
			break;
		case 'H':
			result += "HH24";
			break;
		case 'I':
			result += "HH12";
			break;
		case 'M':
			result += "MI";
			break;
		case 'S':
			result += "SS";
			break;
		case 'f':
			result += "US";
			break;
		case 'z':
			result += "TZH:TZM";
			break;
		case 'Z':
			result += "TZ";
			break;
		case 'a':
			result += "Dy";
			break;
		case 'A':
			result += "Day";
			break;
		case 'b':
			result += "Mon";
			break;
		case 'B':
			result += "Month";
			break;
		case '%':
			result += "%";
			break;
		default:
			ThrowUnsupportedDateFormatToken(dialect, specifier);
		}
	}
	return result;
}

static string ConvertDuckDBDateFormatToSnowflake(const string &format, SqlDialect dialect) {
	string result;
	for (idx_t i = 0; i < format.size(); i++) {
		if (format[i] != '%') {
			result += format[i];
			continue;
		}
		if (i + 1 >= format.size()) {
			ThrowUnsupportedDateFormatToken(dialect, '\0');
		}
		char specifier = format[++i];
		switch (specifier) {
		case 'Y':
			result += "YYYY";
			break;
		case 'y':
			result += "YY";
			break;
		case 'm':
			result += "MM";
			break;
		case 'd':
			result += "DD";
			break;
		case 'H':
			result += "HH24";
			break;
		case 'I':
			result += "HH12";
			break;
		case 'M':
			result += "MI";
			break;
		case 'S':
			result += "SS";
			break;
		case 'f':
			result += "FF6";
			break;
		case 'z':
			result += "TZHTZM";
			break;
		case 'Z':
			result += "TZH";
			break;
		case 'a':
			result += "DY";
			break;
		case 'A':
			result += "DY";
			break;
		case 'b':
			result += "MON";
			break;
		case 'B':
			result += "MMMM";
			break;
		case '%':
			result += "%";
			break;
		default:
			ThrowUnsupportedDateFormatToken(dialect, specifier);
		}
	}
	return result;
}

static string ConvertDuckDBDateFormatToMySQLMariaDB(const string &format, SqlDialect dialect) {
	string result;
	for (idx_t i = 0; i < format.size(); i++) {
		if (format[i] != '%') {
			result += format[i];
			continue;
		}
		if (i + 1 >= format.size()) {
			ThrowUnsupportedDateFormatToken(dialect, '\0');
		}
		char specifier = format[++i];
		switch (specifier) {
		case 'Y':
			result += "%Y";
			break;
		case 'y':
			result += "%y";
			break;
		case 'm':
			result += "%m";
			break;
		case 'd':
			result += "%d";
			break;
		case 'H':
			result += "%H";
			break;
		case 'I':
			result += "%h";
			break;
		case 'M':
			result += "%i";
			break;
		case 'S':
			result += "%s";
			break;
		case 'f':
			result += "%f";
			break;
		case 'j':
			result += "%j";
			break;
		case 'p':
			result += "%p";
			break;
		case 'U':
			result += "%U";
			break;
		case 'w':
			result += "%w";
			break;
		case 'a':
			result += "%a";
			break;
		case 'W':
		case 'A':
			result += "%W";
			break;
		case 'b':
			result += "%b";
			break;
		case 'B':
			result += "%M";
			break;
		case '%':
			result += "%%";
			break;
		default:
			ThrowUnsupportedDateFormatToken(dialect, specifier);
		}
	}
	return result;
}

bool TryConvertDuckDBDateFormatForDialect(const string &format, SqlDialect dialect, string &result) {
	if (UsesJavaDateFormat(dialect)) {
		result = ConvertDuckDBDateFormatToJava(format, dialect);
		return true;
	}
	if (UsesSnowflakeDateFormat(dialect)) {
		result = ConvertDuckDBDateFormatToSnowflake(format, dialect);
		return true;
	}
	if (UsesMySQLMariaDBDateFormat(dialect)) {
		result = ConvertDuckDBDateFormatToMySQLMariaDB(format, dialect);
		return true;
	}
	if (UsesPostgresDateFormat(dialect)) {
		result = ConvertDuckDBDateFormatToPostgres(format, dialect);
		return true;
	}
	return false;
}

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

string ConvertInputDateFormatToDuckDB(const string &format, SqlDialect dialect, InputDateFormatStyle style) {
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

} // namespace duckdb
