#pragma once

extern "C" {
#include "postgres.h"
#include "nodes/extensible.h"
}

namespace pgddb {

extern CustomScanMethods scan_methods;
void InitNode(void);

} // namespace pgddb
