#pragma once

#include "duckdb.hpp"

namespace pgddb {

namespace ddb {
bool DidWrites();
bool DidWrites(duckdb::ClientContext &context);
} // namespace ddb

class DuckDBManager;

// Extension defined manager initializer.
extern duckdb::unique_ptr<DuckDBManager> GetManagerInstance();

class DuckDBManager {
public:
	DuckDBManager()
	    : database(nullptr), connection(nullptr), default_dbname("<!UNSET!>") {
	}
	virtual ~DuckDBManager() = default;

	static inline bool
	IsInitialized() {
		return manager_instance != nullptr && manager_instance->database != nullptr;
	}

	static inline DuckDBManager &
	Get() {
		if (!manager_instance) {
			manager_instance = GetManagerInstance();
		}
		if (!manager_instance->database) {
			manager_instance->Initialize();
		}
		return *manager_instance;
	}

	static duckdb::unique_ptr<duckdb::Connection> CreateConnection();
	static duckdb::Connection *GetConnection(bool force_transaction = false);
	static duckdb::Connection *GetConnectionUnsafe();

	inline const std::string &
	GetDefaultDBName() const {
		return default_dbname;
	}

	inline duckdb::DuckDB &
	GetDatabase() {
		return *database;
	}

	static void Reset();

private:
	DuckDBManager(const DuckDBManager &) = delete;
	DuckDBManager &operator=(const DuckDBManager &) = delete;

	static duckdb::unique_ptr<DuckDBManager> manager_instance;

	void Initialize();

protected:
	virtual void
	OnInit(duckdb::DBConfig & /*config*/) {
	}
	virtual void
	OnPostInit(duckdb::ClientContext & /*context*/) {
	}
	virtual void
	RefreshConnectionState(duckdb::ClientContext & /*context*/) {
	}


	virtual std::string
	ConnectionString() {
		return {};
	}

	virtual void
	RequireExecution() {
	}

	// Whether GetConnection should open a DuckDB transaction. Default uses
	// PG's IsInTransactionBlock at the top-level (no function-context
	// tracking). pg_duckdb overrides this to consult its own
	// top_level_statement flag so DuckDB joins the outer PG transaction
	// when invoked from inside a plpgsql function.
	virtual bool ShouldBeginTransaction();

	/*
	 * FIXME: Use a unique_ptr instead of a raw pointer. For now this is not
	 * possible though, as the MotherDuck extension causes an ABORT when the
	 * DuckDB database its destructor is run at the exit of the process.  This
	 * then in turn crashes Postgres, which we obviously dont't want. Not
	 * running the destructor also doesn't really have any downsides, as the
	 * process is going to die anyway. It's probably even a tiny bit more
	 * efficient not to run the destructor at all. But we should still fix
	 * this, because running the destructor is a good way to find bugs (such
	 * as the one reported in #279).
	 */
	duckdb::DuckDB *database;
	duckdb::unique_ptr<duckdb::Connection> connection;
	std::string default_dbname;

public:
	static duckdb::unique_ptr<duckdb::QueryResult> DuckDBQueryOrThrow(duckdb::ClientContext &context,
	                                                                 const std::string &query);
	static duckdb::unique_ptr<duckdb::QueryResult> DuckDBQueryOrThrow(duckdb::Connection &connection,
	                                                                 const std::string &query);
	static duckdb::unique_ptr<duckdb::QueryResult> DuckDBQueryOrThrow(const std::string &query);
};

// Set the directory to which DuckDB writes temp files
extern char *duckdb_temp_directory;
// Set the directory to where DuckDB stores extensions in
extern char *duckdb_extension_directory;
// The maximum amount of data stored inside DuckDB's 'temp_directory' (when set) (e.g., 1GB)
extern char *duckdb_max_temp_directory_size;
// The maximum memory DuckDB can use in MB (e.g., 4096 for 4GB)
extern int duckdb_maximum_memory;
// Maximum number of DuckDB threads per Postgres backend
extern int duckdb_threads;

} // namespace pgddb
