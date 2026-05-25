/*
 * pgduckdb_subscript.cpp -- SQL-callable subscript fmgr entry points.
 * Bodies delegate to libpgddb's SubscriptRoutines; this TU only owns the
 * fmgr V1 wrappers that pg_duckdb's SQL binds to.
 */

// cpp_wrapper.hpp must precede postgres.h-bearing headers: it pulls in
// DuckDB exception.hpp where FATAL is an enum, before PG redefines FATAL
// as a macro.
#include "pgddb/utility/cpp_wrapper.hpp"

#include "pgddb/pgddb_subscript.h"

extern "C" {
#include "fmgr.h"
}

extern "C" {

DECLARE_PG_FUNCTION(duckdb_row_subscript) {
	PG_RETURN_POINTER(&pgddb::pg::duckdb_row_subscript_routines);
}

DECLARE_PG_FUNCTION(duckdb_struct_subscript) {
	PG_RETURN_POINTER(&pgddb::pg::duckdb_row_subscript_routines);
}

DECLARE_PG_FUNCTION(duckdb_unresolved_type_subscript) {
	PG_RETURN_POINTER(&pgddb::pg::duckdb_loose_subscript_routines);
}

DECLARE_PG_FUNCTION(duckdb_map_subscript) {
	PG_RETURN_POINTER(&pgddb::pg::duckdb_loose_subscript_routines);
}

} // extern "C"
