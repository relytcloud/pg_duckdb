# pg_ducklake as a libpgddb consumer.
#
# Build wiring follows examples/pg_vortex/Makefile: include the root Makefile
# to pull in PGDDB_INCLUDE / PGDDB_OBJS / FULL_DUCKDB_LIB, then bundle the
# libpgddb sources into pg_ducklake.so. The ducklake DuckDB extension is
# baked into libduckdb_bundle.a via DUCKDB_EXTENSION_CONFIGS, so no separate
# cmake invocation or -force_load is needed.

# EXTENSION_CONFIGS must be set before including the root Makefile because
# DUCKDB_CONSUMER_TAG hashes it.
ROOT_DIR := $(abspath $(CURDIR)/../..)
EXTENSION_CONFIGS := $(CURDIR)/pg_ducklake_extensions.cmake
include $(ROOT_DIR)/Makefile

MODULE_big = pg_ducklake
EXTENSION = pg_ducklake
DATA = pg_ducklake.control $(wildcard sql/pg_ducklake--*.sql)

SRCS = $(wildcard src/*.cpp src/*/*.cpp)
OBJS = $(SRCS:.cpp=.o)
C_SRCS = $(wildcard src/*.c src/*/*.c)
OBJS += $(C_SRCS:.c=.o)

# libpgddb objects compiled in-place at $(ROOT_DIR)/src/.
OBJS += $(PGDDB_OBJS)

DUCKLAKE_INCLUDE = -I$(CURDIR)/third_party/ducklake/src/include

override PG_CPPFLAGS += -Iinclude $(PGDDB_INCLUDE) $(PGDDB_DUCKDB_INCLUDE) $(DUCKLAKE_INCLUDE) -isystem $(INCLUDEDIR_SERVER)
override PG_CXXFLAGS += -std=c++17 -Wno-register

# pg_ducklake's source directly references DuckLakeMetadataManager /
# PostgresMetadataManager symbols from the ducklake DuckDB extension. The
# bundle (libduckdb_bundle.a) contains them, but the linker won't pull them
# in just because pg_ducklake.cpp inherits from those classes -- vtable
# references don't drive archive member inclusion. Force-load the
# pre-bundle libducklake_extension.a explicitly to keep all its .o's.
DUCKLAKE_EXT_LIB := $(DUCKDB_BUILD_DIR)/extension/ducklake/libducklake_extension.a

PG_DUCKLAKE_LINK_FLAGS = $(FULL_DUCKDB_LIB) -lstdc++ -llz4
ifeq ($(shell uname -s),Darwin)
	PG_DUCKLAKE_LINK_FLAGS += -Wl,-force_load,$(DUCKLAKE_EXT_LIB)
else
	PG_DUCKLAKE_LINK_FLAGS += -Wl,--whole-archive $(DUCKLAKE_EXT_LIB) -Wl,--no-whole-archive
	PG_DUCKLAKE_LINK_FLAGS += -Wl,--exclude-libs,ALL
endif

SHLIB_LINK += $(PG_DUCKLAKE_LINK_FLAGS)

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
INCLUDEDIR_SERVER := $(shell $(PG_CONFIG) --includedir-server)
include $(PGXS)

# Consumer .o files depend on the duckdb submodule HEAD marker so first-time
# builds init the submodule before compiling.
$(OBJS): $(PGDDB_DIR)/.git/modules/third_party/duckdb/HEAD

# shlib pulls in $(FULL_DUCKDB_LIB) before linking.
$(shlib): $(FULL_DUCKDB_LIB) $(OBJS)

# Project formatting (project files only, not third_party/).
FORMAT_FILES = $(shell find src/ include/ -name '*.cpp' -o -name '*.hpp' -o -name '*.h')

.PHONY: format check-format check-regression clean-regression check-isolation clean-isolation clean-all

format:
	clang-format -i $(FORMAT_FILES)

check-format:
	clang-format --dry-run -Werror $(FORMAT_FILES)

installcheck: all install
	$(MAKE) check-regression check-isolation

check-regression:
	$(MAKE) -C test/regression check-regression

clean-regression:
	$(MAKE) -C test/regression clean-regression

check-isolation:
	$(MAKE) -C test/isolation check-isolation

clean-isolation:
	$(MAKE) -C test/isolation clean-isolation

clean-all: clean clean-regression clean-isolation clean-duckdb
