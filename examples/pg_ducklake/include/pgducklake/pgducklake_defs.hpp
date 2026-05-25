// PostgreSQL extension name
#define PGDUCKLAKE_PG_EXTENSION "pg_ducklake"

// Catalog in DuckDB. Where DuckLake metadata + tables live.
#define PGDUCKLAKE_DUCKDB_CATALOG        "pgducklake"
#define PGDUCKLAKE_DUCKDB_CATALOG_QUOTED "'pgducklake'"

// Companion catalog backed by libpgddb's PostgresStorageExtension. Lets
// DuckDB queries reach PG heap tables / foreign tables / views.
#define PGDUCKLAKE_PG_STORAGE_CATALOG "pgduckdb"

// Metadata schema in PostgreSQL.
#define PGDUCKLAKE_PG_SCHEMA        "ducklake"
#define PGDUCKLAKE_PG_SCHEMA_QUOTED "'ducklake'"

// PostgreSQL table access method name (matches `CREATE ACCESS METHOD
// ducklake` in pg_ducklake--0.1.0.sql). Also the catalog prefix returned
// by pgducklake's pgddb::table_am_get_name_hook so the lib deparser can
// recognize ducklake-AM relations.
#define PGDUCKLAKE_TABLE_AM "ducklake"

namespace pgducklake {
// Install the table-AM name hook into libpgddb. Called from _PG_init.
void InitTableAmHook();
} // namespace pgducklake

// Index access method.
#define PGDUCKLAKE_SORTED_AM "ducklake_sorted"
