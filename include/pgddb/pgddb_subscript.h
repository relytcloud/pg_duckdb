#pragma once

extern "C" {
#include "postgres.h"

#include "nodes/subscripting.h"
}

namespace pgddb::pg {

/* Text-keyed subscript routines: first index must be a TEXT Const
 * ("r['col']"). Used by row and struct pseudo-types. */
extern const SubscriptRoutines duckdb_row_subscript_routines;

/* Loose subscript routines: any subscript expression. Used by
 * unresolved_type and map pseudo-types. */
extern const SubscriptRoutines duckdb_loose_subscript_routines;

} // namespace pgddb::pg
