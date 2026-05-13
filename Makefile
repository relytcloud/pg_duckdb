# Root Makefile. Two roles:
#
# 1. Delegating: `make examples/<name>/<target>` forwards to
#    examples/<name>/Makefile via $(MAKE) -C.
# 2. libpgddb source list: extension Makefiles `include` this file to pull
#    in the variables below. They append PGDDB_OBJS to their OBJS so the
#    libpgddb sources get compiled into their dylib.

PGDDB_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PGDDB_INCLUDE := -I$(PGDDB_DIR)/include
PGDDB_CPP_SRCS := $(wildcard $(PGDDB_DIR)/src/*.cpp $(PGDDB_DIR)/src/*/*.cpp)
PGDDB_C_SRCS := $(wildcard $(PGDDB_DIR)/src/*.c $(PGDDB_DIR)/src/*/*.c)
PGDDB_SRCS := $(PGDDB_CPP_SRCS) $(PGDDB_C_SRCS)
PGDDB_OBJS := $(PGDDB_CPP_SRCS:.cpp=.o) $(PGDDB_C_SRCS:.c=.o)

examples/%:
	$(MAKE) -C $(@D) $(@F)
