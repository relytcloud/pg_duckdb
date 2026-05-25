/*
 * pgddb_table_am.cpp -- hook + wrappers for resolving a table AM to its
 * DuckDB catalog name. Each consumer sets table_am_get_name_hook once in
 * _PG_init; the lib deparser and planner walker call TableAmGetName.
 */

#include "pgddb/pgddb_table_am.hpp"

extern "C" {
#include "postgres.h"

#include "utils/rel.h"
}

namespace pgddb {

table_am_get_name_hook_t table_am_get_name_hook = nullptr;

const char *
TableAmGetName(const TableAmRoutine *am) {
	return table_am_get_name_hook ? table_am_get_name_hook(am) : nullptr;
}

const char *
TableAmGetName(Oid relid) {
	if (relid == InvalidOid) {
		return nullptr;
	}
	auto rel = RelationIdGetRelation(relid);
	const char *name = TableAmGetName(rel->rd_tableam);
	RelationClose(rel);
	return name;
}

} // namespace pgddb
