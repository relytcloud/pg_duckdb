#pragma once

/*
 * pgducklake_functions.hpp -- DuckLake function exposing.
 *
 * Registers wrapper table macros in DuckDB's system.main catalog that
 * bridge PG function names to ducklake_<name>(catalog, ...) globals.
 * Detection of DuckDB-only functions in the planner uses prosrc-based
 * IsDucklakeOnlyFunction below, no explicit registry is needed.
 */

namespace duckdb {
class DatabaseInstance;
}

/* Oid is `unsigned int` (postgres_ext.h). Forward-declare the C typedef
 * here to avoid pulling postgres.h into this header -- any consumer .cpp
 * that includes a DuckDB header before postgres.h would otherwise lose
 * DuckDB's Min/Max/FATAL etc. to PG's macros. */
extern "C" {
typedef unsigned int Oid;
}

namespace pgducklake {

void RegisterWrapperMacros(duckdb::DatabaseInstance &db);
void RegisterScalarMacros(duckdb::DatabaseInstance &db);
void RegisterCleanupFunction(duckdb::DatabaseInstance &db);
void RegisterCleanupOrphanedFilesFunction(duckdb::DatabaseInstance &db);
void RegisterCompactionFunctions(duckdb::DatabaseInstance &db);
void RegisterFlushInlinedDataFunction(duckdb::DatabaseInstance &db);

/*
 * Returns true if the function with the given OID lives in the ducklake
 * schema and was declared with prosrc='duckdb_only_function'. These are
 * the functions whose execution must be delegated to DuckDB -- the C-side
 * duckdb_only_function() stub errors when called directly.
 */
bool IsDucklakeOnlyFunction(Oid funcid);

} // namespace pgducklake
