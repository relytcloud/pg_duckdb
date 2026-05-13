#pragma once

extern "C" {
#include "postgres.h"
}

namespace pgddb {

Datum DetoastPostgresDatum(struct varlena *value, bool *should_free);

} // namespace pgddb
