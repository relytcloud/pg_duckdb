/*
 * pgducklake_types.cpp -- libpgddb type and row-deparse hooks.
 *
 * @scope backend: install GetPostgresDuckDBType + ConvertDuckToPostgresValue
 *                 + the row-deparse hooks (var_is_row, func_returns_row,
 *                 write_row_refname, is_fake_type)
 *
 * Two related concerns live here:
 *
 * 1) DuckDB STRUCT/UNION/MAP values need to round-trip through PG as a
 *    real PG type. We map them to ducklake.duckdb_struct (a varlena
 *    passthrough type) and serialize via pgddb::ConvertToStringDatum.
 *
 * 2) DuckLake metadata functions (snapshots, table_info, flush_inlined_data,
 *    duckdb_query, ...) declare PG return type SETOF ducklake.duckdb_row.
 *    The deparser must turn a top-level Var of type duckdb_row into
 *    `<refname>.*` so DuckDB returns all underlying columns, and must
 *    suppress the spurious `::duckdb_row` / `::duckdb_struct` cast that
 *    the standard PG deparser would otherwise emit. Without these hooks
 *    `SELECT * FROM ducklake.duckdb_query('SELECT id,name FROM t')`
 *    returns a single STRUCT column instead of two scalar columns.
 *
 * All hooks chain to a previously-installed prev_hook so multiple libpgddb
 * consumers can coexist in the same backend.
 */

// pgddb_types.hpp pulls in DuckDB headers and must parse before any PG
// header (FATAL macro collision in DuckDB's exception.hpp). Keep it ahead
// of pgddb_ruleutils.h, which includes postgres.h via its own extern "C".
#include "pgddb/pgddb_types.hpp"

#include "pgducklake/pgducklake_types.hpp"
#include "pgducklake/pgducklake_defs.hpp"

extern "C" {
#include "postgres.h"

#include "pgddb/pgddb_ruleutils.h"

#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "nodes/parsenodes.h"
#include "nodes/primnodes.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
}

namespace pgducklake {

// Captured at install time, called as fallthrough if not ours.
static pgddb::ConvertPostgresToBaseDuckColumnType_hook_t prev_ConvertPostgresToBaseDuckColumnType_hook = nullptr;
static pgddb::GetPostgresDuckDBType_hook_t prev_GetPostgresDuckDBType_hook = nullptr;
static pgddb::ConvertDuckToPostgresValue_hook_t prev_ConvertDuckToPostgresValue_hook = nullptr;
static pgddb_is_fake_type_hook_t prev_is_fake_type_hook = nullptr;
static pgddb_show_type_hook_t prev_show_type_hook = nullptr;
static pgddb_var_is_row_hook_t prev_var_is_row_hook = nullptr;
static pgddb_func_returns_row_hook_t prev_func_returns_row_hook = nullptr;
static pgddb_write_row_refname_hook_t prev_write_row_refname_hook = nullptr;
static pgddb_strip_first_subscript_hook_t prev_strip_first_subscript_hook = nullptr;

// Resolve a ducklake.<name> type's OID. Cached in a static after first
// resolution -- pg_type rows don't move during a backend's lifetime.
// Returns InvalidOid before CREATE EXTENSION pg_ducklake.
static Oid LookupDucklakeType(const char *type_name, Oid *cache) {
  if (OidIsValid(*cache))
    return *cache;
  Oid nsp = get_namespace_oid(PGDUCKLAKE_PG_SCHEMA, /*missing_ok=*/true);
  if (!OidIsValid(nsp))
    return InvalidOid;
  Oid type_oid =
      GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, PointerGetDatum(type_name), ObjectIdGetDatum(nsp));
  if (OidIsValid(type_oid))
    *cache = type_oid;
  return type_oid;
}

Oid LookupDucklakeDuckdbRowOid() {
  static Oid cached = InvalidOid;
  return LookupDucklakeType("duckdb_row", &cached);
}

Oid LookupDucklakeDuckdbStructOid() {
  static Oid cached = InvalidOid;
  return LookupDucklakeType("duckdb_struct", &cached);
}

static Oid DucklakeDuckdbRowOid() {
  return LookupDucklakeDuckdbRowOid();
}

static Oid DucklakeDuckdbStructOid() {
  return LookupDucklakeDuckdbStructOid();
}

static Oid DucklakeVariantOid() {
  static Oid cached = InvalidOid;
  return LookupDucklakeType("variant", &cached);
}

// --------------------------------------------------------------------------
// pgddb_types.hpp hooks: PG OID -> DuckDB base type, DuckDB type -> PG OID,
// DuckDB value -> PG Datum.
// --------------------------------------------------------------------------

// ducklake.variant is a varlena text on the PG side. DuckDB has no native
// VARIANT type at this version, so we model it as VARCHAR and rely on the
// pg_variant_extract* scalar macros (registered in pgducklake_functions.cpp)
// for field-extraction operators.
static bool ConvertPostgresToBaseDuckColumnTypeHook(Oid pg_oid, duckdb::LogicalType &out) {
  if (OidIsValid(pg_oid) && pg_oid == DucklakeVariantOid()) {
    out = duckdb::LogicalType::VARCHAR;
    return true;
  }
  return prev_ConvertPostgresToBaseDuckColumnType_hook
             ? prev_ConvertPostgresToBaseDuckColumnType_hook(pg_oid, out)
             : false;
}

static bool GetPostgresDuckDBTypeHook(const duckdb::LogicalType &type, Oid &out) {
  switch (type.id()) {
  case duckdb::LogicalTypeId::STRUCT:
  case duckdb::LogicalTypeId::UNION:
  case duckdb::LogicalTypeId::MAP: {
    Oid struct_oid = DucklakeDuckdbStructOid();
    if (!OidIsValid(struct_oid))
      break;
    out = struct_oid;
    return true;
  }
  default:
    break;
  }
  return prev_GetPostgresDuckDBType_hook ? prev_GetPostgresDuckDBType_hook(type, out) : false;
}

static bool ConvertDuckToPostgresValueHook(Oid pg_oid, duckdb::Value &value, TupleTableSlot *slot, uint64_t col) {
  if (OidIsValid(pg_oid) && pg_oid == DucklakeDuckdbStructOid()) {
    slot->tts_values[col] = pgddb::ConvertToStringDatum(value);
    return true;
  }
  return prev_ConvertDuckToPostgresValue_hook ? prev_ConvertDuckToPostgresValue_hook(pg_oid, value, slot, col) : false;
}

// --------------------------------------------------------------------------
// pgddb_ruleutils.h hooks: duckdb_row deparse -> "<refname>.*"
// --------------------------------------------------------------------------

static bool IsFakeTypeHook(Oid type_oid) {
  if (!OidIsValid(type_oid))
    return prev_is_fake_type_hook ? prev_is_fake_type_hook(type_oid) : false;
  if (type_oid == DucklakeDuckdbRowOid() || type_oid == DucklakeDuckdbStructOid() ||
      type_oid == DucklakeVariantOid())
    return true;
  return prev_is_fake_type_hook ? prev_is_fake_type_hook(type_oid) : false;
}

// get_const_expr otherwise tacks "::ducklake.variant" onto every literal
// whose target column is variant, and DuckDB can't resolve that type.
// Returning -1 forces the deparser to suppress the cast.
//
// Also suppress `::numeric` with no typmod: DuckDB defaults plain `::numeric`
// to DECIMAL(18,3), which overflows for INSERT VALUES of large literals
// (e.g. 1.78e16) into a wider DECIMAL(p,s) column. Without the cast, DuckDB
// parses the quoted value as VARCHAR and coerces to the column type.
static int ShowTypeHook(Const *constval, int original_showtype) {
  if (constval && IsFakeTypeHook(constval->consttype))
    return -1;
  if (constval && constval->consttype == NUMERICOID && constval->consttypmod == -1)
    return -1;
  return prev_show_type_hook ? prev_show_type_hook(constval, original_showtype) : original_showtype;
}

static bool VarIsRowHook(Var *var) {
  if (var && var->vartype == DucklakeDuckdbRowOid())
    return true;
  return prev_var_is_row_hook ? prev_var_is_row_hook(var) : false;
}

static bool FuncReturnsRowHook(RangeTblFunction *rtfunc) {
  if (rtfunc && rtfunc->funcexpr && IsA(rtfunc->funcexpr, FuncExpr)) {
    FuncExpr *fexpr = castNode(FuncExpr, rtfunc->funcexpr);
    if (fexpr->funcresulttype == DucklakeDuckdbRowOid())
      return true;
  }
  return prev_func_returns_row_hook ? prev_func_returns_row_hook(rtfunc) : false;
}

// Top-level: emit `<refname>.*` so DuckDB returns all underlying columns
// instead of packing them into a STRUCT. Non-top-level (e.g. r['col']):
// just return the alias unchanged.
static char *WriteRowRefnameHook(StringInfo buf, char *refname, bool is_top_level) {
  appendStringInfoString(buf, quote_identifier(refname));
  if (is_top_level) {
    appendStringInfoString(buf, ".*");
    return NULL;
  }
  return refname;
}

// Deparse `r['col']` on a duckdb_row Var as `r.col` for DuckDB. r is a
// function alias in the FROM clause whose underlying table function
// expands to real columns, so dot-access is the natural DuckDB syntax.
// Returns the SubscriptingRef with the first index stripped (so any
// trailing nested subscripts still print as `[...]`).
static SubscriptingRef *StripFirstSubscriptHook(SubscriptingRef *sbsref, StringInfo buf) {
  if (!sbsref || !IsA(sbsref->refexpr, Var)) {
    return prev_strip_first_subscript_hook ? prev_strip_first_subscript_hook(sbsref, buf) : sbsref;
  }
  Var *var = (Var *)sbsref->refexpr;
  if (var->vartype != DucklakeDuckdbRowOid()) {
    return prev_strip_first_subscript_hook ? prev_strip_first_subscript_hook(sbsref, buf) : sbsref;
  }
  if (sbsref->refupperindexpr == NIL) {
    return sbsref;
  }
  Const *first = castNode(Const, linitial(sbsref->refupperindexpr));
  Oid typoutput;
  bool typIsVarlena;
  getTypeOutputInfo(first->consttype, &typoutput, &typIsVarlena);
  char *colname = OidOutputFunctionCall(typoutput, first->constvalue);
  appendStringInfo(buf, ".%s", quote_identifier(colname));

  SubscriptingRef *shorter = (SubscriptingRef *)copyObjectImpl(sbsref);
  shorter->refupperindexpr = list_delete_first(shorter->refupperindexpr);
  if (shorter->reflowerindexpr) {
    shorter->reflowerindexpr = list_delete_first(shorter->reflowerindexpr);
  }
  return shorter;
}

void InitTypeHooks() {
  prev_ConvertPostgresToBaseDuckColumnType_hook = pgddb::ConvertPostgresToBaseDuckColumnType_hook;
  pgddb::ConvertPostgresToBaseDuckColumnType_hook = ConvertPostgresToBaseDuckColumnTypeHook;

  prev_GetPostgresDuckDBType_hook = pgddb::GetPostgresDuckDBType_hook;
  pgddb::GetPostgresDuckDBType_hook = GetPostgresDuckDBTypeHook;

  prev_ConvertDuckToPostgresValue_hook = pgddb::ConvertDuckToPostgresValue_hook;
  pgddb::ConvertDuckToPostgresValue_hook = ConvertDuckToPostgresValueHook;

  prev_is_fake_type_hook = pgddb_is_fake_type_hook;
  pgddb_is_fake_type_hook = IsFakeTypeHook;

  prev_show_type_hook = pgddb_show_type_hook;
  pgddb_show_type_hook = ShowTypeHook;

  prev_var_is_row_hook = pgddb_var_is_row_hook;
  pgddb_var_is_row_hook = VarIsRowHook;

  prev_func_returns_row_hook = pgddb_func_returns_row_hook;
  pgddb_func_returns_row_hook = FuncReturnsRowHook;

  prev_write_row_refname_hook = pgddb_write_row_refname_hook;
  pgddb_write_row_refname_hook = WriteRowRefnameHook;

  prev_strip_first_subscript_hook = pgddb_strip_first_subscript_hook;
  pgddb_strip_first_subscript_hook = StripFirstSubscriptHook;
}

} // namespace pgducklake

// ---------------------------------------------------------------------------
// SQL-callable C entry points for ducklake.duckdb_row and
// ducklake.duckdb_struct. Bodies delegate to libpgddb's shared
// SubscriptRoutines; this TU owns the fmgr V1 wrappers that
// pg_ducklake's SQL binds to.
// ---------------------------------------------------------------------------

#include "pgddb/pgddb_subscript.h"
#include "pgddb/utility/cpp_wrapper.hpp"

extern "C" {

DECLARE_PG_FUNCTION(duckdb_row_in) {
  elog(ERROR, "Creating the ducklake.duckdb_row type is not supported");
}

DECLARE_PG_FUNCTION(duckdb_row_out) {
  elog(ERROR, "Converting a ducklake.duckdb_row to a string is not supported");
}

DECLARE_PG_FUNCTION(duckdb_row_subscript) {
  PG_RETURN_POINTER(&pgddb::pg::duckdb_row_subscript_routines);
}

DECLARE_PG_FUNCTION(duckdb_struct_in) {
  elog(ERROR, "Creating the ducklake.duckdb_struct type is not supported");
}

DECLARE_PG_FUNCTION(duckdb_struct_out) {
  return textout(fcinfo);
}

DECLARE_PG_FUNCTION(duckdb_struct_subscript) {
  PG_RETURN_POINTER(&pgddb::pg::duckdb_row_subscript_routines);
}

} // extern "C"
