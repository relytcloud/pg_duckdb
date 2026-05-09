#include "pgddb/pg/guc.hpp"
#include "pgddb/pgddb_utils.hpp"

extern "C" {
#include "postgres.h"
#include "utils/guc.h"
}

namespace pgddb::pg {

const char *
GetConfigOption(const char *name, bool missing_ok, bool restrict_privileged) {
	return PostgresFunctionGuard(::GetConfigOption, name, missing_ok, restrict_privileged);
}
} // namespace pgddb::pg
