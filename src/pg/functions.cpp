#include "pgddb/pg/functions.hpp"
#include "duckdb/common/exception.hpp"

#include "pgddb/pgddb_utils.hpp"

extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
}

namespace pgddb::pg {

std::string
GetArgString(PG_FUNCTION_ARGS, int argno) {
	if (PG_NARGS() <= argno) {
		throw duckdb::InvalidInputException("argument %d is required", argno);
	}

	if (PG_ARGISNULL(argno)) {
		throw duckdb::InvalidInputException("argument %d cannot be NULL", argno);
	}

	return DatumToString(PG_GETARG_DATUM(argno));
}

Datum
GetArgDatum(PG_FUNCTION_ARGS, int argno) {
	if (PG_NARGS() <= argno) {
		throw duckdb::InvalidInputException("argument %d is required", argno);
	}

	if (PG_ARGISNULL(argno)) {
		throw duckdb::InvalidInputException("argument %d cannot be NULL", argno);
	}

	return PG_GETARG_DATUM(argno);
}

namespace {
char *
DatumToCstring(Datum datum) {
	return text_to_cstring(DatumGetTextPP(datum));
}
} // namespace

std::string
DatumToString(Datum datum) {
	return std::string(PostgresFunctionGuard(DatumToCstring, datum));
}

} // namespace pgddb::pg
