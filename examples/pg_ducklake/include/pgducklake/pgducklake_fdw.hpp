#pragma once

/*
 * pgducklake_fdw.hpp — Foreign Data Wrapper for DuckLake tables
 *
 * Provides read-only access to DuckLake tables (both PostgreSQL-backed and
 * frozen HTTP-hosted) via PostgreSQL's FDW infrastructure.  Queries are
 * routed through DuckDB by registering the foreign tables as "external
 * DuckDB tables" with pg_duckdb's hook system.
 */

struct Query; /* forward-declare PostgreSQL Query node */

namespace pgducklake {
void InitFDW();
void RegisterForeignTablesInQuery(Query *query);
// Returns true if any RTE in the query references a ducklake_fdw foreign
// table. Used by the planner hook to route foreign-table queries through
// DuckDB.
bool QueryReferencesDucklakeForeignTable(Query *query);
} // namespace pgducklake
