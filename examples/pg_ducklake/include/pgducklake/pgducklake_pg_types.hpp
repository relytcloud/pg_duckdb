#pragma once

// Vendored type conversion utilities from pg_duckdb
// These functions convert PostgreSQL types to DuckDB types
// Simplified to support only the types needed for DuckLake metadata

#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/types.hpp"

extern "C" {
#include "postgres.h"
#include "catalog/pg_attribute.h"
}

namespace pgducklake {

// Detoast a PostgreSQL varlena value
// Returns the detoasted value and sets should_free if the caller must free it
Datum DetoastPostgresDatum(struct varlena *attr, bool *should_free);

// Convert PostgreSQL Datum value to DuckDB Vector at the given offset
void ConvertPostgresToDuckValue(Oid attr_type, Datum value, duckdb::Vector &result, uint64_t offset);

// Convert PostgreSQL column attribute to DuckDB LogicalType
duckdb::LogicalType ConvertPostgresToDuckColumnType(Form_pg_attribute &attribute);

} // namespace pgducklake
