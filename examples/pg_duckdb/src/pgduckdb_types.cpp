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

// pgduckdb composite types are stored as text Datums on the PG side.
// All three converters (struct, union, map) stringify the duckdb value.

namespace {

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

} // namespace

// Prev-hook slots: captured at registration, called as fallthrough.
static pgddb::ConvertPostgresToBaseDuckColumnType_hook_t prev_ConvertPostgresToBaseDuckColumnType_hook = nullptr;
static pgddb::GetPostgresDuckDBType_hook_t prev_GetPostgresDuckDBType_hook = nullptr;
static pgddb::GetPostgresArrayDuckDBType_hook_t prev_GetPostgresArrayDuckDBType_hook = nullptr;
static pgddb::ConvertDuckToPostgresValue_hook_t prev_ConvertDuckToPostgresValue_hook = nullptr;
static pgddb::ConvertUnsupportedNumericToDouble_hook_t prev_ConvertUnsupportedNumericToDouble_hook = nullptr;

static bool
ConvertPostgresToBaseDuckColumnType(Oid pg_oid, duckdb::LogicalType &out) {
	if (pg_oid == DuckdbStructOid() || pg_oid == DuckdbStructArrayOid()) {
		out = duckdb::LogicalTypeId::STRUCT;
		return true;
	}
	if (pg_oid == DuckdbUnionOid() || pg_oid == DuckdbUnionArrayOid()) {
		out = duckdb::LogicalTypeId::UNION;
		return true;
	}
	if (pg_oid == DuckdbMapOid() || pg_oid == DuckdbMapArrayOid()) {
		out = duckdb::LogicalTypeId::MAP;
		return true;
	}
	return prev_ConvertPostgresToBaseDuckColumnType_hook
	           ? prev_ConvertPostgresToBaseDuckColumnType_hook(pg_oid, out)
	           : false;
}

static bool
GetPostgresDuckDBType(const duckdb::LogicalType &type, Oid &out) {
	switch (type.id()) {
	case duckdb::LogicalTypeId::STRUCT:
		out = DuckdbStructOid();
		return true;
	case duckdb::LogicalTypeId::UNION:
		out = DuckdbUnionOid();
		return true;
	case duckdb::LogicalTypeId::MAP:
		out = DuckdbMapOid();
		return true;
	default:
		break;
	}
	return prev_GetPostgresDuckDBType_hook ? prev_GetPostgresDuckDBType_hook(type, out) : false;
}

static bool
GetPostgresArrayDuckDBType(const duckdb::LogicalType &type, Oid &out) {
	switch (type.id()) {
	case duckdb::LogicalTypeId::STRUCT:
		out = DuckdbStructArrayOid();
		return true;
	case duckdb::LogicalTypeId::UNION:
		out = DuckdbUnionArrayOid();
		return true;
	case duckdb::LogicalTypeId::MAP:
		out = DuckdbMapArrayOid();
		return true;
	default:
		break;
	}
	return prev_GetPostgresArrayDuckDBType_hook ? prev_GetPostgresArrayDuckDBType_hook(type, out) : false;
}

static bool
ConvertDuckToPostgresValue(Oid pg_oid, duckdb::Value &value, TupleTableSlot *slot, uint64_t col) {
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
	return prev_ConvertDuckToPostgresValue_hook
	           ? prev_ConvertDuckToPostgresValue_hook(pg_oid, value, slot, col)
	           : false;
}

static bool
ConvertUnsupportedNumericToDouble(void) {
	if (duckdb_convert_unsupported_numeric_to_double) {
		return true;
	}
	return prev_ConvertUnsupportedNumericToDouble_hook ? prev_ConvertUnsupportedNumericToDouble_hook() : false;
}

void
InitTypeHooks() {
	prev_ConvertPostgresToBaseDuckColumnType_hook = pgddb::ConvertPostgresToBaseDuckColumnType_hook;
	pgddb::ConvertPostgresToBaseDuckColumnType_hook = ConvertPostgresToBaseDuckColumnType;

	prev_GetPostgresDuckDBType_hook = pgddb::GetPostgresDuckDBType_hook;
	pgddb::GetPostgresDuckDBType_hook = GetPostgresDuckDBType;

	prev_GetPostgresArrayDuckDBType_hook = pgddb::GetPostgresArrayDuckDBType_hook;
	pgddb::GetPostgresArrayDuckDBType_hook = GetPostgresArrayDuckDBType;

	prev_ConvertDuckToPostgresValue_hook = pgddb::ConvertDuckToPostgresValue_hook;
	pgddb::ConvertDuckToPostgresValue_hook = ConvertDuckToPostgresValue;

	prev_ConvertUnsupportedNumericToDouble_hook = pgddb::ConvertUnsupportedNumericToDouble_hook;
	pgddb::ConvertUnsupportedNumericToDouble_hook = ConvertUnsupportedNumericToDouble;
}

} // namespace pgduckdb
