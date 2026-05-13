#include "duckdb.hpp"

#include "pgddb/pgddb_types.hpp"
#include "pgddb/pgddb_types_array.hpp"

#include "pgduckdb/pgduckdb_types.hpp"
#include "pgduckdb/pgduckdb_guc.hpp"
#include "pgduckdb/pgduckdb_metadata_cache.hpp"

extern "C" {
#include "postgres.h"
#include "utils/array.h"
}

namespace pgduckdb {

namespace {

// pgduckdb composite types are stored as text Datums on the PG side.
// All three converters (struct, union, map) just stringify the duckdb value.

struct StructArray {
	static ArrayType *
	ConstructArray(Datum *datums, bool *nulls, int ndims, int *dims, int *lower_bound) {
		return construct_md_array(datums, nulls, ndims, dims, lower_bound, DuckdbStructOid(), -1, false, 'i');
	}
	static Datum
	ConvertToPostgres(const duckdb::Value &val) {
		return pgddb::ConvertToStringDatum(val);
	}
};

struct UnionArray {
	static ArrayType *
	ConstructArray(Datum *datums, bool *nulls, int ndims, int *dims, int *lower_bound) {
		return construct_md_array(datums, nulls, ndims, dims, lower_bound, DuckdbUnionOid(), -1, false, 'i');
	}
	static Datum
	ConvertToPostgres(const duckdb::Value &val) {
		return pgddb::ConvertToStringDatum(val);
	}
};

struct MapArray {
	static ArrayType *
	ConstructArray(Datum *datums, bool *nulls, int ndims, int *dims, int *lower_bound) {
		return construct_md_array(datums, nulls, ndims, dims, lower_bound, DuckdbMapOid(), -1, false, 'i');
	}
	static Datum
	ConvertToPostgres(const duckdb::Value &val) {
		return pgddb::ConvertToStringDatum(val);
	}
};

// Prev-hook slots: captured at registration, called as fallthrough.
pgddb::ConvertPostgresToBaseDuckColumnType_hook_t prev_pg_to_duck_hook = nullptr;
pgddb::GetPostgresDuckDBType_hook_t prev_duck_to_pg_hook = nullptr;
pgddb::GetPostgresArrayDuckDBType_hook_t prev_duck_to_pg_array_hook = nullptr;
pgddb::ConvertDuckToPostgresValue_hook_t prev_convert_duck_to_pg_hook = nullptr;
pgddb::ConvertUnsupportedNumericToDouble_hook_t prev_numeric_to_double_hook = nullptr;

bool
PgToDuckImpl(Oid pg_oid, duckdb::LogicalType *out) {
	if (pg_oid == DuckdbStructOid() || pg_oid == DuckdbStructArrayOid()) {
		*out = duckdb::LogicalTypeId::STRUCT;
		return true;
	}
	if (pg_oid == DuckdbUnionOid() || pg_oid == DuckdbUnionArrayOid()) {
		*out = duckdb::LogicalTypeId::UNION;
		return true;
	}
	if (pg_oid == DuckdbMapOid() || pg_oid == DuckdbMapArrayOid()) {
		*out = duckdb::LogicalTypeId::MAP;
		return true;
	}
	return prev_pg_to_duck_hook ? prev_pg_to_duck_hook(pg_oid, out) : false;
}

bool
DuckToPgImpl(const duckdb::LogicalType &type, Oid *out) {
	switch (type.id()) {
	case duckdb::LogicalTypeId::STRUCT:
		*out = DuckdbStructOid();
		return true;
	case duckdb::LogicalTypeId::UNION:
		*out = DuckdbUnionOid();
		return true;
	case duckdb::LogicalTypeId::MAP:
		*out = DuckdbMapOid();
		return true;
	default:
		break;
	}
	return prev_duck_to_pg_hook ? prev_duck_to_pg_hook(type, out) : false;
}

bool
DuckToPgArrayImpl(const duckdb::LogicalType &type, Oid *out) {
	switch (type.id()) {
	case duckdb::LogicalTypeId::STRUCT:
		*out = DuckdbStructArrayOid();
		return true;
	case duckdb::LogicalTypeId::UNION:
		*out = DuckdbUnionArrayOid();
		return true;
	case duckdb::LogicalTypeId::MAP:
		*out = DuckdbMapArrayOid();
		return true;
	default:
		break;
	}
	return prev_duck_to_pg_array_hook ? prev_duck_to_pg_array_hook(type, out) : false;
}

bool
ConvertDuckToPgImpl(Oid pg_oid, duckdb::Value &value, TupleTableSlot *slot, uint64_t col) {
	if (pg_oid == DuckdbStructOid() || pg_oid == DuckdbUnionOid() || pg_oid == DuckdbMapOid()) {
		slot->tts_values[col] = pgddb::ConvertToStringDatum(value);
		return true;
	}
	if (pg_oid == DuckdbStructArrayOid()) {
		pgddb::ConvertDuckToPostgresArray<StructArray>(slot, value, col);
		return true;
	}
	if (pg_oid == DuckdbUnionArrayOid()) {
		pgddb::ConvertDuckToPostgresArray<UnionArray>(slot, value, col);
		return true;
	}
	if (pg_oid == DuckdbMapArrayOid()) {
		pgddb::ConvertDuckToPostgresArray<MapArray>(slot, value, col);
		return true;
	}
	return prev_convert_duck_to_pg_hook ? prev_convert_duck_to_pg_hook(pg_oid, value, slot, col) : false;
}

bool
NumericToDoubleImpl(void) {
	if (duckdb_convert_unsupported_numeric_to_double) {
		return true;
	}
	return prev_numeric_to_double_hook ? prev_numeric_to_double_hook() : false;
}

} // namespace

void
RegisterTypeHooks() {
	prev_pg_to_duck_hook = pgddb::ConvertPostgresToBaseDuckColumnType_hook;
	pgddb::ConvertPostgresToBaseDuckColumnType_hook = PgToDuckImpl;

	prev_duck_to_pg_hook = pgddb::GetPostgresDuckDBType_hook;
	pgddb::GetPostgresDuckDBType_hook = DuckToPgImpl;

	prev_duck_to_pg_array_hook = pgddb::GetPostgresArrayDuckDBType_hook;
	pgddb::GetPostgresArrayDuckDBType_hook = DuckToPgArrayImpl;

	prev_convert_duck_to_pg_hook = pgddb::ConvertDuckToPostgresValue_hook;
	pgddb::ConvertDuckToPostgresValue_hook = ConvertDuckToPgImpl;

	prev_numeric_to_double_hook = pgddb::ConvertUnsupportedNumericToDouble_hook;
	pgddb::ConvertUnsupportedNumericToDouble_hook = NumericToDoubleImpl;
}

} // namespace pgduckdb
