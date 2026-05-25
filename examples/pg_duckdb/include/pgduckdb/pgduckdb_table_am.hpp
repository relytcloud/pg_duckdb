#include "pgddb/pg/declarations.hpp"

namespace pgduckdb {

bool IsDuckdbTableAm(const TableAmRoutine *am);

/* Install pg_duckdb's table-AM name hook into libpgddb so the lib
 * deparser and planner walker can identify duckdb_methods relations.
 * Called once from _PG_init. */
void InitTableAmHook();

} // namespace pgduckdb
