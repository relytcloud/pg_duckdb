/*
 * pgducklake_duckdb_query.cpp -- Execute DuckDB queries against libpgddb.
 *
 * @scope backend: last_error thread_local
 *
 * Drives DuckDB queries directly through libpgddb's DuckDBManager
 * connection. The upstream design routed every query through
 * pg_duckdb's duckdb.raw_query() UDF (PG SPI -> pg_duckdb's planner ->
 * DuckDB) so pg_ducklake could stay PG-only; the libpgddb consumer
 * model gives us the connection in process, so we use it directly.
 *
 * Used by DDL triggers, VACUUM, freeze, FDW attach, and the utility
 * hook.
 */

#include <duckdb/common/string_util.hpp>
#include <duckdb/main/connection.hpp>
#include <duckdb/parser/keyword_helper.hpp>

#include "pgducklake/pgducklake_duckdb_query.hpp"
#include "pgducklake/pgducklake_guc.hpp"

#include "pgddb/pgddb_duckdb.hpp"

#include <string>

extern "C" {
#include "postgres.h"

#include "utils/elog.h"
}

namespace pgducklake {

/*
 * Execute a DuckDB query on libpgddb's cached connection.
 *
 * Returns 0 on success, 1 on error.
 * On error, sets *errmsg_out to a thread-local copy of the message.
 */
int ExecuteDuckDBQuery(const char *query, const char **errmsg_out) {
  static thread_local std::string last_error;

  try {
    auto *conn = ::pgddb::DuckDBManager::GetConnection();
    auto result = conn->Query(query);
    if (result->HasError()) {
      last_error = result->GetError();
      if (errmsg_out)
        *errmsg_out = last_error.c_str();
      return 1;
    }
    return 0;
  } catch (const std::exception &e) {
    last_error = e.what();
    if (errmsg_out)
      *errmsg_out = last_error.c_str();
    return 1;
  }
}

void SyncDefaultTablePathToDuckDB() {
  if (default_table_path && default_table_path[0] != '\0') {
    std::string set_query =
        "SET ducklake_default_table_path = " + duckdb::KeywordHelper::WriteQuoted(std::string(default_table_path));
    const char *errmsg = nullptr;
    if (ExecuteDuckDBQuery(set_query.c_str(), &errmsg) != 0) {
      elog(WARNING, "failed to sync ducklake.default_table_path to DuckDB: %s", errmsg ? errmsg : "unknown");
    }
  }
}

} // namespace pgducklake
