/*
 * pgducklake_admin.cpp -- ducklake.recycle_ddb / duckdb_raw_query / duckdb_query
 *
 * @scope extension: SQL-level admin utilities exposed in the ducklake schema
 *
 * Mirrors pg_duckdb's duckdb.recycle_ddb / duckdb.raw_query / duckdb.query.
 *
 *   ducklake.recycle_ddb()           -- tear down + recreate DuckDBManager
 *   ducklake.duckdb_raw_query(text)  -- run an arbitrary string on DuckDB
 *   ducklake.duckdb_query(text)      -- SETOF ducklake.duckdb_row, routed by planner
 *
 * duckdb_query() is a `duckdb_only_function`-marked SQL stub; its body
 * lives in pgducklake_pgduckdb_shim.cpp and only fires if the planner
 * intercept fails to route it. DucklakeFunctionName rewrites the deparsed
 * name to DuckDB's built-in query() table function.
 */

// DuckDB headers parse first to avoid FATAL macro collision with postgres.h.
#include "duckdb/common/types.hpp"

#include "pgducklake/pgducklake_duckdb_query.hpp"

#include "pgddb/pgddb_duckdb.hpp"
#include "pgddb/pg/transactions.hpp"

extern "C" {
#include "postgres.h"

#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/elog.h"
}

extern "C" {

PG_FUNCTION_INFO_V1(ducklake_recycle_ddb);
Datum
ducklake_recycle_ddb(PG_FUNCTION_ARGS) {
	// Recycling tears down a DuckDB instance that may have an open
	// transaction tied to the current PG transaction. Match pg_duckdb's
	// guard.
	::pgddb::pg::PreventInTransactionBlock(true, "ducklake.recycle_ddb()");
	::pgddb::DuckDBManager::Reset();
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(ducklake_duckdb_raw_query);
Datum
ducklake_duckdb_raw_query(PG_FUNCTION_ARGS) {
	if (PG_ARGISNULL(0))
		ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("query must not be NULL")));
	const char *query = text_to_cstring(PG_GETARG_TEXT_PP(0));
	const char *errmsg_out = nullptr;
	if (::pgducklake::ExecuteDuckDBQuery(query, &errmsg_out) != 0)
		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("%s", errmsg_out ? errmsg_out : "unknown error")));
	PG_RETURN_VOID();
}

} // extern "C"
