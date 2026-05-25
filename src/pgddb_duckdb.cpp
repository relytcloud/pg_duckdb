#include "pgddb/pgddb_duckdb.hpp"

#include <filesystem>

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"

#include "pgddb/pg/guc.hpp"
#include "pgddb/pg/transactions.hpp"
#include "pgddb/utility/signal_guard.hpp"

extern "C" {
#include "postgres.h"
}

namespace pgddb {

namespace ddb {
bool
DidWrites() {
	if (!DuckDBManager::IsInitialized()) {
		return false;
	}

	auto connection = DuckDBManager::GetConnectionUnsafe();
	auto &context = *connection->context;
	return DidWrites(context);
}

bool
DidWrites(duckdb::ClientContext &context) {
	if (!context.transaction.HasActiveTransaction()) {
		return false;
	}
	return context.ActiveTransaction().ModifiedDatabase() != nullptr;
}
} // namespace ddb

duckdb::unique_ptr<DuckDBManager> DuckDBManager::manager_instance;

bool
DuckDBManager::ShouldBeginTransaction() {
	return pgddb::pg::IsInTransactionBlock(true);
}

char *duckdb_temp_directory = strdup("");
char *duckdb_extension_directory = strdup("");
char *duckdb_max_temp_directory_size = strdup("");
int duckdb_maximum_memory = 0;
int duckdb_threads = -1;


namespace {

template <typename T>
std::string
ToString(T value) {
	return std::to_string(value);
}

template <>
std::string
ToString(char *value) {
	return std::string(value);
}

} // anonymous namespace

// DuckDB v1.5 removed direct field access on DBConfigOptions for most settings;
// route everything through SetOptionByName. See duckdb/pg_duckdb#1025.
#define SET_DUCKDB_OPTION(ddb_option_name)                                                                             \
	config.SetOptionByName(#ddb_option_name, duckdb::Value(duckdb_##ddb_option_name));                                 \
	elog(DEBUG2, "[pgddb] Set DuckDB option: '" #ddb_option_name "'=%s", ToString(duckdb_##ddb_option_name).c_str());

void
DuckDBManager::Initialize() {
	elog(DEBUG2, "(pgddb/DuckDBManager) Creating DuckDB instance");

	// Block signals before initializing DuckDB to ensure signal is handled by the Postgres main thread only
	pgddb::ThreadSignalBlockGuard guard;

	duckdb::DBConfig config;
	config.SetOptionByName("default_null_order", "postgres");

	OnInit(config);

	SET_DUCKDB_OPTION(temp_directory);
	SET_DUCKDB_OPTION(extension_directory);
	if (duckdb_temp_directory && strlen(duckdb_temp_directory) > 0) {
		std::filesystem::create_directories(duckdb_temp_directory);
	}
	if (duckdb_extension_directory && strlen(duckdb_extension_directory) > 0) {
		std::filesystem::create_directories(duckdb_extension_directory);
	}

	if (duckdb_maximum_memory > 0) {
		// Convert the memory limit from MB (as set by Postgres GUC_UNIT_MB, which is actually MiB; see
		// memory_unit_conversion_table in guc.c) to a string with the "MiB" suffix, as required by DuckDB's memory
		// parser. This ensures the value is interpreted correctly by DuckDB.
		std::string memory_limit = std::to_string(duckdb_maximum_memory) + "MiB";
		config.options.maximum_memory = duckdb::DBConfig::ParseMemoryLimit(memory_limit);
		elog(DEBUG2, "[pgddb] Set DuckDB option: 'maximum_memory'=%dMB", duckdb_maximum_memory);
	}
	if (duckdb_max_temp_directory_size != NULL && strlen(duckdb_max_temp_directory_size) != 0) {
		config.SetOptionByName("max_temp_directory_size", duckdb_max_temp_directory_size);
		elog(DEBUG2, "[pgddb] Set DuckDB option: 'max_temp_directory_size'=%s", duckdb_max_temp_directory_size);
	}

	if (duckdb_threads > -1) {
		SET_DUCKDB_OPTION(threads);
	}

	std::string connection_string;
	connection_string = ConnectionString();
	std::string pg_time_zone(pgddb::pg::GetConfigOption("TimeZone"));

	database = new duckdb::DuckDB(connection_string, &config);

	connection = duckdb::make_uniq<duckdb::Connection>(*database);

	auto &context = *connection->context;

	auto &db_manager = duckdb::DatabaseManager::Get(context);
	default_dbname = db_manager.GetDefaultDatabase(context);
	DuckDBQueryOrThrow(context, "SET TimeZone =" + duckdb::KeywordHelper::WriteQuoted(pg_time_zone));

	OnPostInit(context);
}

void
DuckDBManager::Reset() {
	if (!manager_instance) {
		return;
	}
	manager_instance->connection = nullptr;
	delete manager_instance->database;
	manager_instance->database = nullptr;
}

/*
 * Creates a new connection to the global DuckDB instance. This should only be
 * used in some rare cases, where a temporary new connection is needed instead
 * of the global cached connection that is returned by GetConnection.
 */
duckdb::unique_ptr<duckdb::Connection>
DuckDBManager::CreateConnection() {
	auto &instance = Get();
	instance.RequireExecution();

	auto connection = duckdb::make_uniq<duckdb::Connection>(*instance.database);
	auto &context = *connection->context;

	instance.RefreshConnectionState(context);

	return connection;
}

/* Returns the cached connection to the global DuckDB instance. */
duckdb::Connection *
DuckDBManager::GetConnection(bool force_transaction) {
	auto &instance = Get();
	instance.RequireExecution();

	auto &context = *instance.connection->context;

	if (!context.transaction.HasActiveTransaction()) {
		if (IsSubTransaction()) {
			throw duckdb::NotImplementedException("SAVEPOINT and subtransactions are not supported in DuckDB");
		}

		if (force_transaction || instance.ShouldBeginTransaction()) {
			/*
			 * We only want to open a new DuckDB transaction if we're already
			 * in a Postgres transaction block. Always opening a transaction
			 * incurs a significant performance penalty for single statement
			 * queries on MotherDuck. This is because a second round-trip is
			 * needed to send the COMMIT to MotherDuck when Postgres its
			 * transaction finishes. So we only want to do this when actually
			 * necessary.
			 */
			instance.connection->BeginTransaction();
		}
	}

	instance.RefreshConnectionState(context);

	return instance.connection.get();
}

/*
 * Returns the cached connection to the global DuckDB instance, but does not do
 * any checks required to correctly initialize the DuckDB transaction nor
 * refreshes the secrets/extensions/etc. Only use this in rare cases where you
 * know for sure that the connection is already initialized correctly for the
 * current query, and you just want a pointer to it.
 */
duckdb::Connection *
DuckDBManager::GetConnectionUnsafe() {
	auto &instance = Get();
	return instance.connection.get();
}

} // namespace pgddb
