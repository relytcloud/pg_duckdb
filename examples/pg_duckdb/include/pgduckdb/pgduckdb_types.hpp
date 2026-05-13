#pragma once

namespace pgduckdb {

// Install pg_duckdb's type-extension hook implementations into libpgddb.
// Called from _PG_init.
void InitTypeHooks();

} // namespace pgduckdb
