#include "sql_dialect.hpp"
#include "lpts_helpers.hpp"

#include "duckdb/parser/keyword_helper.hpp"

namespace duckdb {

SqlDialect ParseSqlDialectSetting(const string &value, const string &setting_name) {
	string normalized = SQLToLowercase(value);
	if (normalized == "duckdb") {
		return SqlDialect::DUCKDB;
	}
	if (normalized == "postgres" || normalized == "postgresql") {
		return SqlDialect::POSTGRES;
	}
	if (normalized == "spark") {
		return SqlDialect::SPARK;
	}
	if (normalized == "hive") {
		return SqlDialect::HIVE;
	}
	if (normalized == "trino" || normalized == "presto") {
		return SqlDialect::TRINO_PRESTO;
	}
	if (normalized == "snowflake") {
		return SqlDialect::SNOWFLAKE;
	}
	if (normalized == "bigquery" || normalized == "bq") {
		return SqlDialect::BIGQUERY;
	}
	if (normalized == "redshift") {
		return SqlDialect::REDSHIFT;
	}
	if (normalized == "mysql" || normalized == "mariadb") {
		return SqlDialect::MYSQL_MARIADB;
	}
	throw InvalidInputException(
	    "Unknown %s '%s'. Valid values: 'duckdb', 'postgres', 'spark', 'hive', 'trino', 'presto', "
	    "'snowflake', 'bigquery', 'redshift', 'mysql', 'mariadb'",
	    setting_name, value);
}

SqlDialect ParseSqlDialect(const string &value) {
	return ParseSqlDialectSetting(value, "lpts_dialect");
}

string SqlDialectToString(SqlDialect dialect) {
	switch (dialect) {
	case SqlDialect::DUCKDB:
		return "duckdb";
	case SqlDialect::POSTGRES:
		return "postgres";
	case SqlDialect::SPARK:
		return "spark";
	case SqlDialect::HIVE:
		return "hive";
	case SqlDialect::TRINO_PRESTO:
		return "trino_presto";
	case SqlDialect::SNOWFLAKE:
		return "snowflake";
	case SqlDialect::BIGQUERY:
		return "bigquery";
	case SqlDialect::REDSHIFT:
		return "redshift";
	case SqlDialect::MYSQL_MARIADB:
		return "mysql_mariadb";
	default:
		return "unknown";
	}
}

bool DialectUsesBacktickQuotedIdentifiers(SqlDialect dialect) {
	return dialect == SqlDialect::SPARK || dialect == SqlDialect::HIVE || dialect == SqlDialect::BIGQUERY ||
	       dialect == SqlDialect::MYSQL_MARIADB;
}

bool DialectUsesUnqualifiedTableNames(SqlDialect dialect) {
	return dialect == SqlDialect::POSTGRES || dialect == SqlDialect::REDSHIFT;
}

bool DialectUsesSchemaQualifiedTableNames(SqlDialect dialect) {
	return dialect == SqlDialect::HIVE || dialect == SqlDialect::MYSQL_MARIADB;
}

bool DialectUsesSingleQuotedTablePath(SqlDialect dialect) {
	return dialect == SqlDialect::BIGQUERY;
}

string DialectQuoteIdent(const string &name, SqlDialect dialect) {
	if (DialectUsesBacktickQuotedIdentifiers(dialect)) {
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
	return KeywordHelper::WriteOptionallyQuoted(name);
}

} // namespace duckdb
