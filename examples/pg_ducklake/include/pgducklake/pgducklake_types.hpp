#pragma once

extern "C" {
#include "postgres.h"
}

namespace pgducklake {

// Install libpgddb type-conversion hooks (GetPostgresDuckDBType +
// ConvertDuckToPostgresValue). Maps DuckDB STRUCT/UNION/MAP results to
// the ducklake.duckdb_struct passthrough type so DuckLake metadata
// functions returning STRUCT (flush_inlined_data, freeze, etc.) don't
// hit a "Could not convert DuckDB type" error.
void InitTypeHooks();

// Lookup OIDs for the pseudo-types declared in pg_ducklake--*.sql.
// Returns InvalidOid before CREATE EXTENSION runs. Cached per-process.
Oid LookupDucklakeDuckdbRowOid();
Oid LookupDucklakeDuckdbStructOid();

} // namespace pgducklake
