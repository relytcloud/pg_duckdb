#pragma once

#include "pgddb/pgddb_duckdb.hpp"

namespace pgduckdb {

// pgduckdb::DuckDBManager extends pgddb::DuckDBManager with pg_duckdb's
// specific init/refresh policy: pg_duckdb GUC -> DBConfig mapping,
// MotherDuck connection string, duckdb storage extension registration,
// secrets + extensions-table refresh, the duckdb.postgres_role permission
// check, and the disabled_filesystems / azure_transport_option_type
// per-connection settings. Wired into libpgddb via
// pgddb::GetManagerInstance() (defined in pgduckdb_duckdb.cpp).
class DuckDBManager : public pgddb::DuckDBManager {
public:
	DuckDBManager() = default;

	// Called from pgduckdb_userdata_cache when user-mapping changes
	// invalidate the in-DuckDB secrets cache. Static so the caller doesn't
	// need a typed reference.
	static void InvalidateSecretsIfInitialized();

	void OnInit(duckdb::DBConfig &config) override;
	void OnPostInit(duckdb::ClientContext &context) override;
	void RefreshConnectionState(duckdb::ClientContext &context) override;

	std::string ConnectionString() override;
	void RequireExecution() override;
	bool ShouldBeginTransaction() override;

private:
	void LoadSecrets(duckdb::ClientContext &context);
	void DropSecrets(duckdb::ClientContext &context);
	void LoadExtensions(duckdb::ClientContext &context);
	void InstallExtensions(duckdb::ClientContext &context);

	int64_t extensions_table_current_seq_ = 0;
	bool secrets_valid_ = false;
};

} // namespace pgduckdb
