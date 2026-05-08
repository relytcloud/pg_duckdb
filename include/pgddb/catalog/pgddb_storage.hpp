#pragma once

#include "duckdb/storage/storage_extension.hpp"

#include "pgduckdb/utility/cpp_only_file.hpp" // Must be last include.

namespace pgddb {

class PostgresStorageExtension : public duckdb::StorageExtension {
public:
	PostgresStorageExtension();
};

} // namespace pgddb
