#pragma once

#include "duckdb/common/types/data_chunk.hpp"
#include "pgddb/pg/declarations.hpp"

#include "pgddb/utility/cpp_only_file.hpp" // Must be last include.

namespace pgddb {

struct PostgresScanGlobalState;
struct PostgresScanLocalState;

// DuckDB has date starting from 1/1/1970 while PG starts from 1/1/2000
constexpr int32_t PGDUCKDB_DUCK_DATE_OFFSET = 10957;
constexpr int64_t PGDUCKDB_DUCK_TIMESTAMP_OFFSET =
    static_cast<int64_t>(PGDUCKDB_DUCK_DATE_OFFSET) * static_cast<int64_t>(86400000000) /* USECS_PER_DAY */;

// Check from regress/sql/date.sql
#define PG_MINYEAR  (-4713)
#define PG_MINMONTH (11)
#define PG_MINDAY   (24)
#define PG_MAXYEAR  (5874897)
#define PG_MAXMONTH (12)
#define PG_MAXDAY   (31)

const duckdb::date_t PGDUCKDB_PG_MIN_DATE_VALUE = duckdb::Date::FromDate(PG_MINYEAR, PG_MINMONTH, PG_MINDAY);
const duckdb::date_t PGDUCKDB_PG_MAX_DATE_VALUE = duckdb::Date::FromDate(PG_MAXYEAR, PG_MAXMONTH, PG_MAXDAY);

// Check ValidTimestampOrTimestampTz() for the logic, These values are counted from 1/1/1970
constexpr int64_t PGDUCKDB_MAX_TIMESTAMP_VALUE = 9223371244800000000;
constexpr int64_t PGDUCKDB_MIN_TIMESTAMP_VALUE = -210866803200000000;

void CheckForUnsupportedPostgresType(duckdb::LogicalType type);
duckdb::LogicalType ConvertPostgresToDuckColumnType(Form_pg_attribute &attribute);
Oid GetPostgresDuckDBType(const duckdb::LogicalType &type, bool throw_error = false);
int32_t GetPostgresDuckDBTypemod(const duckdb::LogicalType &type);
duckdb::Value ConvertPostgresParameterToDuckValue(Datum value, Oid postgres_type);
void ConvertPostgresToDuckValue(Oid attr_type, Datum value, duckdb::Vector &result, uint64_t offset);
bool ConvertDuckToPostgresValue(TupleTableSlot *slot, duckdb::Value &value, uint64_t col);
void InsertTupleIntoChunk(duckdb::DataChunk &output, PostgresScanLocalState &scan_local_state, TupleTableSlot *slot);
void InsertTuplesIntoChunk(duckdb::DataChunk &output, PostgresScanLocalState &scan_local_state, TupleTableSlot **slots,
                           int num_slots);

// Public helpers exposed for consumer-side hook implementations.
bool IsNestedType(duckdb::LogicalTypeId type_id);
const duckdb::LogicalType &GetChildType(const duckdb::LogicalType &type);
Datum ConvertToStringDatum(const duckdb::Value &value);

// Type extension hooks. Standard PG hook pattern (function pointer global plus
// prev_hook chaining). Consumer extensions install their impls in _PG_init.

// Called in the default branch of the PG-Oid -> DuckDB LogicalType switch
// when the Oid isn't a built-in PG type. Return true and fill out for your
// custom type; chain to prev_hook otherwise.
typedef bool (*ConvertPostgresToBaseDuckColumnType_hook_t)(Oid pg_oid, duckdb::LogicalType &out);
extern ConvertPostgresToBaseDuckColumnType_hook_t ConvertPostgresToBaseDuckColumnType_hook;

// Called when libpgddb's LogicalType -> PG Oid switch has no built-in case
// (currently STRUCT / UNION / MAP). Return true and fill out.
typedef bool (*GetPostgresDuckDBType_hook_t)(const duckdb::LogicalType &type, Oid &out);
extern GetPostgresDuckDBType_hook_t GetPostgresDuckDBType_hook;

// Same as above but for the LIST/ARRAY element direction (returns the array form).
typedef bool (*GetPostgresArrayDuckDBType_hook_t)(const duckdb::LogicalType &type, Oid &out);
extern GetPostgresArrayDuckDBType_hook_t GetPostgresArrayDuckDBType_hook;

// Called in the default branch of ConvertDuckToPostgresValue when the PG Oid
// isn't a built-in. Return true if the hook stored the value into the slot.
typedef bool (*ConvertDuckToPostgresValue_hook_t)(Oid pg_oid, duckdb::Value &value, TupleTableSlot *slot, uint64_t col);
extern ConvertDuckToPostgresValue_hook_t ConvertDuckToPostgresValue_hook;

// Policy: should an unsupported-precision NUMERIC fall back to DOUBLE (true)
// or throw an UnsupportedPostgresType error (false)? Default false.
typedef bool (*ConvertUnsupportedNumericToDouble_hook_t)(void);
extern ConvertUnsupportedNumericToDouble_hook_t ConvertUnsupportedNumericToDouble_hook;

} // namespace pgddb
