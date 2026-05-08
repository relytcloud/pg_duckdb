#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;

void _PG_init(void);

void
_PG_init(void)
{
}

PG_FUNCTION_INFO_V1(vortex_version);
Datum
vortex_version(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text("pg_vortex 0.1.0"));
}
