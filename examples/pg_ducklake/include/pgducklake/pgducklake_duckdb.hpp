#pragma once

/*
 * pgducklake_duckdb.hpp -- C++ interface for DuckDB/DuckLake operations
 *
 * Provides functions for DuckLake extension lifecycle management.
 * Query execution against DuckDB is done via pg_duckdb's raw_query() UDF
 * through PostgreSQL's SPI, not through direct DuckDB instance access.
 */

namespace duckdb {
class DuckDB;
}

/*
 * Callback invoked by pg_duckdb during DuckDBManager::Initialize().
 * Receives a reference to the DuckDB instance and loads
 * the DuckLake static extension into it.
 */
void ducklake_load_extension(duckdb::DuckDB &db);

/* Returns the DuckDB instance, used by FDW for column inference. */
duckdb::DuckDB *ducklake_get_duckdb_database();

/* Detach the "pgducklake" DuckLake catalog.  Called by the utility hook
 * after DROP EXTENSION so that a subsequent CREATE EXTENSION can
 * attach a fresh catalog. */
void ducklake_detach_catalog();

/* Attach the "pgducklake" DuckLake catalog.  Called during initial
 * extension load (ducklake_load_extension) and on re-create
 * (ducklake_initialize). */
void ducklake_attach_catalog();

namespace pgducklake {

/* Installs pg_ducklake's libpgddb ruleutils hooks (pgddb_db_and_schema_hook)
 * so pgddb_get_tabledef etc. resolve the DuckDB catalog/schema correctly
 * for ducklake-AM tables. Called from _PG_init. */
void InitRuleutilsHooks();

/* Register PG XactCallback that mirrors PG PRE_COMMIT/ABORT to DuckDB's
 * DuckLake transaction. Without this, DuckDB never commits its in-memory
 * transaction state and subsequent statements see stale snapshots. Called
 * from _PG_init. */
void RegisterXactCallback();

/*
 * Toggle the SUBXACT_EVENT_START_SUB guard. Set true around code paths that
 * legitimately need to open a PG subtransaction while DuckDB has an active
 * transaction (e.g. DuckLake metadata commit's FlushChanges retry loop);
 * set false everywhere else. Backs the pgduckdb::DuckdbAllowSubtransaction
 * contract shim.
 */
void SetAllowSubtransaction(bool allow);

} // namespace pgducklake
