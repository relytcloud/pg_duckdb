# Pull in libpgddb sources + duckdb-build wiring from the repo root.
ROOT_DIR := $(abspath $(CURDIR)/../..)
EXTENSION_CONFIGS := $(CURDIR)/pg_vortex_extensions.cmake
include $(ROOT_DIR)/Makefile

MODULE_big = pg_vortex
EXTENSION = pg_vortex
DATA = $(wildcard pg_vortex--*.sql)

SRCS = $(wildcard src/*.cpp src/*/*.cpp)
OBJS = $(SRCS:.cpp=.o)
C_SRCS = $(wildcard src/*.c src/*/*.c)
OBJS += $(C_SRCS:.c=.o)

# libpgddb objects compiled in-place at $(ROOT_DIR)/src/.
OBJS += $(PGDDB_OBJS)

# -lssl -lcrypto and --exclude-libs,ALL: see pg_duckdb's Makefile.
PG_VORTEX_LINK_FLAGS = $(FULL_DUCKDB_LIB) -lcurl -lssl -lcrypto -lstdc++ -llz4
ifeq ($(shell uname -s),Linux)
	PG_VORTEX_LINK_FLAGS += -Wl,--exclude-libs,ALL
endif
# duckdb-vortex's Rust deps reference macOS Security framework symbols.
# cmake propagates them via target_link_libraries; our bundle (object
# extraction) loses that metadata, so add them here.
ifeq ($(shell uname -s),Darwin)
	PG_VORTEX_LINK_FLAGS += -framework Security -framework CoreFoundation
endif

ERROR_ON_WARNING ?=
ifeq ($(ERROR_ON_WARNING), 1)
	ERROR_ON_WARNING_FLAG = -Werror
else
	ERROR_ON_WARNING_FLAG =
endif

COMPILER_FLAGS = -Wno-sign-compare -Wshadow -Wswitch -Wunused-parameter -Wunreachable-code -Wno-unknown-pragmas -Wall -Wextra ${ERROR_ON_WARNING_FLAG}

override PG_CPPFLAGS += -Iinclude $(PGDDB_INCLUDE) $(PGDDB_DUCKDB_INCLUDE) -isystem $(INCLUDEDIR_SERVER) ${COMPILER_FLAGS}
override PG_CXXFLAGS += -std=c++17 ${COMPILER_FLAGS} -Wno-register
# Same override pg_duckdb applies: the vendored pg_ruleutils_*.c files
# inherit PG's declaration-after-statement check, and with -Werror that
# becomes fatal. We don't care about C-style strictness for the
# Postgres-vendored helpers.
override PG_CFLAGS += -Wno-declaration-after-statement

SHLIB_LINK += $(PG_VORTEX_LINK_FLAGS)

REGRESS = basic
REGRESS_OPTS = --temp-instance=./tmp_check --temp-config=$(CURDIR)/regression.conf

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
PG_LIB := $(shell $(PG_CONFIG) --pkglibdir)
INCLUDEDIR_SERVER := $(shell $(PG_CONFIG) --includedir-server)
include $(PGXS)

# Consumer .o files depend on the duckdb submodule HEAD marker (defined in
# the root Makefile) so first-time builds init the submodule before
# compiling.
$(OBJS): $(PGDDB_DIR)/.git/modules/third_party/duckdb/HEAD

# shlib pulls in $(FULL_DUCKDB_LIB) before linking.
$(shlib): $(FULL_DUCKDB_LIB) $(OBJS)

clean-all: clean clean-duckdb
