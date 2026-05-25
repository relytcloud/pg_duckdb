#pragma once

#include "pgddb/pg/declarations.hpp"

namespace pgddb {

/*
 * Hook returns the DuckDB catalog name for the consumer's table AM, or
 * nullptr if `am` is not a consumer-managed AM. Each consumer sets this
 * once in _PG_init. Used by the lib deparser (catalog prefix) and the
 * lib planner walker (separate consumer relations from PG heap tables).
 */
typedef const char *(*table_am_get_name_hook_t)(const TableAmRoutine *am);
extern table_am_get_name_hook_t table_am_get_name_hook;

const char *TableAmGetName(const TableAmRoutine *am);
const char *TableAmGetName(Oid relid);

} // namespace pgddb
