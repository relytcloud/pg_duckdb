/*
 * pgducklake.cpp -- PostgreSQL extension bootstrap entry points.
 *
 * Defines module metadata and _PG_init(), wiring GUC registration, pg_duckdb
 * callback registration, and pg_ducklake hook initialization.
 */

#include "pgducklake/pgducklake_defs.hpp"
#include "pgducklake/pgducklake_metadata_manager.hpp"
#include "storage/ducklake_metadata_manager.hpp"

#include "pgducklake/pgducklake_direct_insert.hpp"
#include "pgducklake/pgducklake_direct_insert_stats.hpp"
#include "pgducklake/pgducklake_duckdb.hpp"
#include "pgducklake/pgducklake_fdw.hpp"
#include "pgducklake/pgducklake_functions.hpp"
#include "pgducklake/pgducklake_guc.hpp"
#include "pgducklake/pgducklake_hooks.hpp"
#include "pgducklake/pgducklake_maintenance.hpp"
#include "pgducklake/pgducklake_types.hpp"
#include "pgddb/pgddb_node.hpp"

extern "C" {
#include "postgres.h"

#include "fmgr.h"
}

extern "C" {

#ifdef PG_MODULE_MAGIC_EXT
#ifndef PG_DUCKLAKE_VERSION
// Should always be defined via build system, but keep a fallback here for
// static analysis tools etc.
#define PG_DUCKLAKE_VERSION "unknown"
#endif
PG_MODULE_MAGIC_EXT(.name = "pg_ducklake", .version = PG_DUCKLAKE_VERSION);
#else
PG_MODULE_MAGIC;
#endif

void _PG_init(void) {
  // Register metadata manager factory in DuckLake's process-global registry.
  duckdb::DuckLakeMetadataManager::Register(PGDUCKLAKE_DUCKDB_CATALOG, pgducklake::PgDuckLakeMetadataManager::Create);
  // Register DuckLake GUCs
  pgducklake::RegisterGUCs();
  // Register shared memory and background maintenance launcher
  pgducklake::InitMaintenanceShmem();
  pgducklake::RegisterMaintenanceLauncher();
  // Register shared memory for direct-insert planner/exec counters
  pgducklake::InitDirectInsertStatsShmem();
  // Register libpgddb's CustomScan node (for DuckDB-routed plans) and
  // pg_ducklake's own direct-insert CustomScan.
  pgddb::InitNode();
  pgducklake::RegisterDirectInsertNode();
  // Install the table-AM name hook so the lib deparser/planner can
  // recognize ducklake_methods relations.
  pgducklake::InitTableAmHook();
  // Install pg_ducklake planner/utility hooks.
  pgducklake::InitHooks();
  // Install libpgddb ruleutils hooks (db_and_schema policy).
  pgducklake::InitRuleutilsHooks();
  // Install libpgddb type hooks (DuckDB STRUCT -> ducklake.duckdb_struct).
  pgducklake::InitTypeHooks();
  // Mirror PG transaction events to DuckDB's DuckLake transaction.
  pgducklake::RegisterXactCallback();
  // Register FDW callbacks and hooks.
  pgducklake::InitFDW();
}

} // extern "C"
