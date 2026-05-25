/*
 * pgducklake_duckdb.cpp -- DuckLake catalog lifecycle in DuckDB
 *
 * Manages the "pgducklake" DuckLake catalog attached inside DuckDB.
 * Three lifecycles exist:
 *
 *   _PG_init() (once per backend):
 *     DuckLakeMetadataManager::Register("pgducklake", ...)
 *
 *   First CREATE EXTENSION (DuckDB not yet initialized):
 *     ducklake_initialize()          -- SQL script entry point
 *       -> ExecuteDuckDBQuery("SELECT 1")
 *           -> DuckDBManager::Initialize()
 *               -> ducklake_load_extension()   [callback from pg_duckdb]
 *                   -> LoadStaticExtension
 *                   -> ducklake_attach_catalog()
 *
 *   DROP + CREATE EXTENSION (DuckDB already alive):
 *     DROP EXTENSION pg_ducklake
 *       -> DucklakeUtilityHook        [pgducklake_hooks.cpp]
 *           -> ducklake_detach_catalog()
 *     CREATE EXTENSION pg_ducklake
 *       -> ducklake_initialize()
 *           -> ExecuteDuckDBQuery("SELECT 1")   (no-op, DuckDB exists)
 *           -> ducklake_attach_catalog()        (catalog was detached)
 *
 *   duckdb.recycle_ddb() (DuckDB instance destroyed and recreated):
 *     recycle_ddb()
 *       -> DuckDBManager::Reset()               [destroys DuckDB instance]
 *     next query
 *       -> DuckDBManager::Initialize()
 *           -> ducklake_load_extension()         [callback from pg_duckdb]
 *               -> LoadStaticExtension
 *               -> ducklake_attach_catalog()
 *     (metadata manager already registered in _PG_init, no re-registration)
 *
 * Query execution against DuckDB is handled via pg_duckdb's raw_query() UDF
 * through PostgreSQL's SPI in the PostgreSQL-facing translation units.
 */

#include "pgducklake/pgducklake_defs.hpp"
#include "pgducklake/pgducklake_duckdb.hpp"
#include "pgducklake/pgducklake_duckdb_query.hpp"
#include "pgducklake/pgducklake_functions.hpp"
#include "pgducklake/pgducklake_time_travel.hpp"

#include "pgddb/catalog/pgddb_storage.hpp"
#include "pgddb/pgddb_duckdb.hpp"
#include "pgddb/pg/transactions.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/transaction_context.hpp"
#include "ducklake_extension.hpp"

extern "C" {
#include "pgddb/pgddb_ruleutils.h"

#include "nodes/pg_list.h"
}

#include <cstring>
#include <filesystem>

extern "C" {
#include "postgres.h"

#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/extension.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
}

static duckdb::DuckDB *ducklake_duckdb_instance = nullptr;

duckdb::DuckDB *ducklake_get_duckdb_database() {
  return ducklake_duckdb_instance;
}

void ducklake_detach_catalog() {
  const char *errmsg;
  int ret = pgducklake::ExecuteDuckDBQuery("DETACH DATABASE IF EXISTS " PGDUCKLAKE_DUCKDB_CATALOG, &errmsg);
  if (ret != 0) {
    elog(WARNING, "Failed to detach DuckLake catalog: %s", errmsg ? errmsg : "unknown error");
  }
}

void ducklake_attach_catalog() {
  /* METADATA_CATALOG points the DuckLakeTransaction metadata connection's
   * search path at the pgducklake catalog itself (instead of the default
   * __ducklake_metadata_pgducklake, which does not exist because pg_ducklake
   * keeps metadata in PostgreSQL, not in a separate DuckDB database).
   * This lets DuckDB-native queries (read_blob, etc.) on the metadata
   * connection resolve system functions through normal catalog search. */
  duckdb::string query =
      "ATTACH 'ducklake:" PGDUCKLAKE_DUCKDB_CATALOG ":' AS " PGDUCKLAKE_DUCKDB_CATALOG
      "(METADATA_SCHEMA " PGDUCKLAKE_PG_SCHEMA_QUOTED ", METADATA_CATALOG " PGDUCKLAKE_DUCKDB_CATALOG;
  if (creating_extension) {
    /* First-time init: create local data directory and pass it as DATA_PATH
     * so DuckLake stores it in the catalog metadata. */
    auto data_path = duckdb::StringUtil::Format("%s/pg_ducklake", DataDir);
    try {
      std::filesystem::create_directory(data_path);
    } catch (const std::filesystem::filesystem_error &e) {
      ereport(ERROR, (errcode(ERRCODE_IO_ERROR),
                      errmsg("failed to create DuckLake data directory \"%s\": %s", data_path.c_str(), e.what())));
    }
    query += ", DATA_PATH '" + data_path + "'";
  }
  /* On subsequent ATTACHes, omit DATA_PATH so DuckLake reads it from its
   * stored catalog metadata. This avoids mismatch errors when the data_path
   * has been changed (e.g. to an S3 bucket via ducklake.set_option). */
  query += ")";

  elog(DEBUG1, "Executing query: %s", query.c_str());

  const char *errmsg;
  int ret = pgducklake::ExecuteDuckDBQuery(query.c_str(), &errmsg);

  if (ret != 0) {
    elog(ERROR, "Failed to attach DuckLake catalog: %s", errmsg);
  }
}

namespace pgducklake {
void ResetDirectInsertCaches();
} // namespace pgducklake

void ducklake_load_extension(duckdb::DuckDB &db) {
  ducklake_duckdb_instance = &db;
  pgducklake::ResetDirectInsertCaches();
  db.LoadStaticExtension<duckdb::DucklakeExtension>();
  pgducklake::RegisterTimeTravelFunction(*db.instance);
  pgducklake::RegisterWrapperMacros(*db.instance);
  pgducklake::RegisterScalarMacros(*db.instance);
  pgducklake::RegisterCleanupFunction(*db.instance);
  pgducklake::RegisterCleanupOrphanedFilesFunction(*db.instance);
  pgducklake::RegisterCompactionFunctions(*db.instance);
  pgducklake::RegisterFlushInlinedDataFunction(*db.instance);

  ducklake_attach_catalog();
}

// libpgddb manager binding. Subclasses pgddb::DuckDBManager so the first
// ExecuteDuckDBQuery() in a backend brings up DuckDB with pg_ducklake's
// PostgresStorageExtension registered under "pgducklake" (DuckLake's
// ATTACH connection string asks for it) and ducklake_load_extension()
// invoked to load the static extension and attach the catalog.
//
// In upstream pg_ducklake this work fires from a callback pg_duckdb's own
// DuckDBManager invokes via RegisterDuckdbLoadExtension. The Phase 0
// contract shim no-ops that registration; this subclass drives the same
// steps in process instead.
namespace pgducklake {
class DuckDBManager : public ::pgddb::DuckDBManager {
public:
	void
	OnInit(duckdb::DBConfig &config) override {
		// DuckLake's FDW path attaches a remote DuckLake via "ducklake:..."
		// URIs, which pull in the postgres_scanner DuckDB extension at
		// runtime. The system-installed postgres_scanner.duckdb_extension
		// has no Anthropic-signed signature in the test environment, so
		// DuckDB refuses to load it under the default policy. Allow
		// unsigned extensions so the FDW tests can attach upstream
		// postgres-backed DuckLake catalogs.
		config.SetOptionByName("allow_unsigned_extensions", duckdb::Value::BOOLEAN(true));
	}

	void
	OnPostInit(duckdb::ClientContext &context) override {
		auto &dbconfig = duckdb::DBConfig::GetConfig(*database->instance);
		/*
		 * Register PostgresStorageExtension under PGDUCKLAKE_PG_STORAGE_CATALOG
		 * (NOT PGDUCKLAKE_DUCKDB_CATALOG). This is the catalog that the
		 * DuckLake metadata manager's queries reference -- they hardcode
		 * "FROM pgduckdb.<schema>.<table>" to scan PG heap tables via
		 * libpgddb. The DuckLake catalog itself
		 * (PGDUCKLAKE_DUCKDB_CATALOG = "pgducklake") is created separately
		 * by ducklake_attach_catalog via ATTACH 'ducklake:...' -- that
		 * one uses DuckLake's own storage extension.
		 */
		duckdb::StorageExtension::Register(dbconfig, PGDUCKLAKE_PG_STORAGE_CATALOG,
		                                   duckdb::make_shared_ptr<::pgddb::PostgresStorageExtension>());
		ducklake_load_extension(*database);
		/*
		 * Now ATTACH the storage catalog so queries can resolve
		 * `<storage_catalog>.schema.table` references. Mirrors upstream
		 * pg_duckdb's `ATTACH DATABASE 'pgduckdb' (TYPE pgduckdb)`.
		 */
		::pgddb::DuckDBManager::DuckDBQueryOrThrow(
		    context, "ATTACH DATABASE '" PGDUCKLAKE_PG_STORAGE_CATALOG "' (TYPE " PGDUCKLAKE_PG_STORAGE_CATALOG ")");
	}
};
} // namespace pgducklake

namespace pgddb {
duckdb::unique_ptr<DuckDBManager>
GetManagerInstance() {
	return duckdb::make_uniq<pgducklake::DuckDBManager>();
}
} // namespace pgddb

namespace pgducklake {

/*
 * pgddb_db_and_schema_hook impl: route the relation to the right DuckDB
 * catalog based on its table-AM.
 *
 *   ducklake AM   -> PGDUCKLAKE_DUCKDB_CATALOG    (DuckLake catalog)
 *   anything else -> PGDUCKLAKE_PG_STORAGE_CATALOG (PostgresStorageExtension)
 *
 * The companion storage catalog is ATTACHed in OnPostInit and lets DuckDB
 * queries reach PG heap tables, foreign tables, and views. Schema name is
 * the PG schema name unchanged in both cases. No MotherDuck /
 * multi-database routing applies in pg_ducklake.
 */
static List *
DbAndSchemaForDucklake(const char *postgres_schema_name, const char *table_am_name) {
	// pgducklake_table.cpp registers the ducklake AM under
	// PGDUCKLAKE_TABLE_AM. Any other relation (PG heap, foreign table,
	// view) returns nullptr or a different identifier; route those to the
	// PostgresStorageExtension catalog so DuckDB can still read them.
	if (table_am_name == nullptr || strcmp(PGDUCKLAKE_TABLE_AM, table_am_name) != 0)
		return list_make2((void *)PGDUCKLAKE_PG_STORAGE_CATALOG, (void *)postgres_schema_name);
	return list_make2((void *)PGDUCKLAKE_DUCKDB_CATALOG, (void *)postgres_schema_name);
}

/*
 * pgddb_column_type_name_hook impl: the standard PG deparser writes
 * `ducklake.variant` for a variant column, which DuckDB resolves as
 * type "variant" in schema "ducklake" -- which doesn't exist. Map it
 * to plain VARCHAR; field extraction works through the pg_variant_extract*
 * scalar macros (registered in pgducklake_functions.cpp) that parse the
 * underlying JSON-as-text representation. Matches what
 * ConvertPostgresToBaseDuckColumnTypeHook returns for the same OID.
 *
 * Returns NULL for non-variant types so the deparser falls through to
 * PG's format_type_with_typemod.
 */
static char *
DucklakeColumnTypeName(Oid type_oid, int32_t /*typemod*/) {
	if (!OidIsValid(type_oid))
		return nullptr;
	Oid ducklake_nsp = get_namespace_oid(PGDUCKLAKE_PG_SCHEMA, /*missing_ok=*/true);
	if (!OidIsValid(ducklake_nsp))
		return nullptr;
	Oid variant_oid =
	    GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, PointerGetDatum("variant"), ObjectIdGetDatum(ducklake_nsp));
	if (type_oid == variant_oid)
		return pstrdup("VARCHAR");
	return nullptr;
}

/*
 * pgddb_function_name_hook impl: when a ducklake-only function (prosrc
 * 'duckdb_only_function') is deparsed for DuckDB execution, rewrite the
 * name to "system.main.<func_name>" so DuckDB resolves the wrapper macro
 * registered under DEFAULT_SCHEMA (see RegisterWrapperMacros). Returns
 * NULL for any other function so libpgddb deparser falls through to its
 * standard name resolution.
 */
static char *
DucklakeFunctionName(Oid function_oid, bool *use_variadic_p) {
	if (!IsDucklakeOnlyFunction(function_oid))
		return nullptr;
	if (use_variadic_p)
		*use_variadic_p = false;
	char *func_name = get_func_name(function_oid);
	// ducklake.duckdb_query() is a thin wrapper over DuckDB's built-in
	// query() table function. Deparse directly to "query(...)" rather
	// than routing through a system.main wrapper macro.
	if (std::strcmp(func_name, "duckdb_query") == 0)
		return pstrdup("query");
	return psprintf("system.main.%s", quote_identifier(func_name));
}

void
InitRuleutilsHooks() {
	pgddb_db_and_schema_hook = DbAndSchemaForDucklake;
	pgddb_function_name_hook = DucklakeFunctionName;
	pgddb_column_type_name_hook = DucklakeColumnTypeName;
}

/*
 * PG -> DuckDB transaction sync.
 *
 * DuckLake materializes its catalog and inlined data in PG tables via SPI.
 * Those writes ride PG's transaction. DuckDB itself maintains its own
 * transaction (DuckLakeTransaction) on the pgducklake catalog: bumping
 * snapshot ids, tracking inlined inserts, etc. Without a callback that
 * mirrors PG's PRE_COMMIT / ABORT to DuckDB, DuckDB's transaction stays
 * open after each implicit-autocommit statement, so subsequent statements
 * see a stale view and the next backend never observes the writes
 * (DuckLake metadata Iterators rely on snapshot_id from a committed row).
 *
 * Lifted from pg_duckdb's DuckdbXactCallback minus its MotherDuck-, mixed-
 * write-, and command-id-tracking machinery (pg_ducklake doesn't need them
 * since its writes go through PG's heap, not a separate DuckDB store).
 */
static void
DuckLakeXactCallback(XactEvent event, void * /*arg*/) {
	if (!::pgddb::DuckDBManager::IsInitialized()) {
		return;
	}
	auto *connection = ::pgddb::DuckDBManager::GetConnectionUnsafe();
	if (!connection) {
		return;
	}
	auto &context = *connection->context;
	if (!context.transaction.HasActiveTransaction()) {
		return;
	}
	switch (event) {
	case XACT_EVENT_PRE_COMMIT:
	case XACT_EVENT_PARALLEL_PRE_COMMIT:
		context.transaction.Commit();
		break;
	case XACT_EVENT_ABORT:
	case XACT_EVENT_PARALLEL_ABORT:
		context.transaction.Rollback(nullptr);
		break;
	case XACT_EVENT_PREPARE:
	case XACT_EVENT_PRE_PREPARE:
	case XACT_EVENT_COMMIT:
	case XACT_EVENT_PARALLEL_COMMIT:
		break;
	}
}

/*
 * Backing store for the DuckdbAllowSubtransaction() contract. DuckLake's
 * metadata-commit path needs to open a PG subtransaction so its retry loop
 * can catch unique-violation conflicts; we flip this guard on around that
 * specific BeginInternalSubTransaction. Any other SAVEPOINT attempt while
 * DuckDB has an active transaction is rejected by DuckLakeSubXactCallback
 * below.
 */
bool allow_subtransaction = false;

void
SetAllowSubtransaction(bool allow) {
	allow_subtransaction = allow;
}

/*
 * PG -> DuckDB SAVEPOINT guard. Mirrors pg_duckdb's DuckdbSubXactCallback:
 * if DuckDB is holding an active transaction and the user issues a
 * SAVEPOINT (or BeginInternalSubTransaction without the allow_subtransaction
 * gate), throw so the inconsistency surfaces immediately instead of
 * corrupting PG's snapshot bookkeeping at parent commit time.
 */
static void
DuckLakeSubXactCallback(SubXactEvent event, SubTransactionId /*my_subid*/, SubTransactionId /*parent_subid*/,
                        void * /*arg*/) {
	if (!::pgddb::DuckDBManager::IsInitialized()) {
		return;
	}
	auto *connection = ::pgddb::DuckDBManager::GetConnectionUnsafe();
	if (!connection) {
		return;
	}
	auto &context = *connection->context;
	if (!context.transaction.HasActiveTransaction()) {
		return;
	}
	if (event == SUBXACT_EVENT_START_SUB && !allow_subtransaction) {
		throw duckdb::NotImplementedException("SAVEPOINT is not supported in DuckDB");
	}
}

void
RegisterXactCallback() {
	::pgddb::pg::RegisterXactCallback(DuckLakeXactCallback, nullptr);
	::pgddb::pg::RegisterSubXactCallback(DuckLakeSubXactCallback, nullptr);
}

} // namespace pgducklake
