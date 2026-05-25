#pragma once

extern "C" {
#include "postgres.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
}

namespace pgduckdb {

/* Installs pg_duckdb's ruleutils hook impls into libpgddb. Called from _PG_init. */
void InitRuleutilsHooks();

} // namespace pgduckdb

extern "C" {

/*
 * pgduckdb_get_viewdef stays in pg_duckdb: it has pg_duckdb-specific view
 * handling that pg_ducklake does not exercise. The CREATE TABLE / ALTER
 * TABLE / RENAME deparsers moved to libpgddb's pgddb_get_tabledef family
 * (see pgddb/pgddb_ruleutils.h); pg_duckdb's policy is installed via
 * pgddb_validate_create_table_hook in InitRuleutilsHooks.
 */
char *pgduckdb_get_viewdef(const ViewStmt *stmt, const char *postgres_schema_name, const char *view_name,
                           const char *duckdb_query_string);

/*
 * The pg_duckdb db-and-schema policy. Also registered as
 * pgddb_db_and_schema_hook so libpgddb-side pgddb_db_and_schema_string can
 * dispatch through it.
 */
List *pgduckdb_db_and_schema(const char *postgres_schema_name, const char *duckdb_table_am_name);

} // extern "C"
