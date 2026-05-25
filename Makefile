# Root Makefile. Three roles:
#
# 1. Delegating: `make examples/<name>/<target>` forwards to
#    examples/<name>/Makefile via $(MAKE) -C.
# 2. libpgddb source list: extension Makefiles `include` this file to pull
#    in PGDDB_INCLUDE / PGDDB_OBJS / PGDDB_DUCKDB_INCLUDE. They append
#    PGDDB_OBJS to OBJS so the libpgddb sources get bundled into their dylib.
# 3. DuckDB submodule + build. Each consumer's EXTENSION_CONFIGS produces
#    its own libduckdb_bundle.a in a tagged build/<type>-<tag>/ subdir;
#    that .a is statically linked into the consumer's .so. We never publish
#    libduckdb.so at $(PG_LIB), so consumers can't overwrite each other's
#    runtime duckdb. cmake is driven directly (duckdb's `make release` /
#    `make bundle-library` hard-code build/release/ so they can't share).

PGDDB_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PGDDB_INCLUDE := -I$(PGDDB_DIR)/include
PGDDB_DUCKDB_INCLUDE := -isystem $(PGDDB_DIR)/third_party/duckdb/src/include \
                        -isystem $(PGDDB_DIR)/third_party/duckdb/third_party/re2
PGDDB_CPP_SRCS := $(wildcard $(PGDDB_DIR)/src/*.cpp $(PGDDB_DIR)/src/*/*.cpp)
PGDDB_C_SRCS := $(wildcard $(PGDDB_DIR)/src/*.c $(PGDDB_DIR)/src/*/*.c)
PGDDB_SRCS := $(PGDDB_CPP_SRCS) $(PGDDB_C_SRCS)
PGDDB_OBJS := $(PGDDB_CPP_SRCS:.cpp=.o) $(PGDDB_C_SRCS:.c=.o)

# --- DuckDB submodule build ---

# set to `make` to disable ninja
DUCKDB_GEN ?= ninja
# used to know what version of extensions to download
DUCKDB_VERSION = v1.5.2
# duckdb build tweaks
DUCKDB_CMAKE_VARS = -DCXX_EXTRA=-fvisibility=default -DBUILD_SHELL=0 -DBUILD_PYTHON=0 -DBUILD_UNITTESTS=0 -DOVERRIDE_GIT_DESCRIBE=v1.5.2
# set to 1 to disable asserts in DuckDB. This is particularly useful in combinition with MotherDuck.
# When asserts are enabled the released motherduck extension will fail some of
# those asserts. By disabling asserts it's possible to run a debug build of
# DuckDB agains the release build of MotherDuck.
DUCKDB_DISABLE_ASSERTIONS ?= 0

DUCKDB_BUILD_CXX_FLAGS=
ifeq ($(DUCKDB_BUILD), Debug)
	DUCKDB_BUILD_CXX_FLAGS = -g -O0 -D_GLIBCXX_ASSERTIONS
	DUCKDB_BUILD_TYPE = debug
	DUCKDB_CMAKE_BUILD_TYPE = Debug
	DUCKDB_EXTRA_CMAKE_VARS = -DDEBUG_MOVE=1
else
	DUCKDB_BUILD_TYPE = release
	DUCKDB_CMAKE_BUILD_TYPE = Release
endif
ifeq ($(DUCKDB_DISABLE_ASSERTIONS), 1)
	DUCKDB_EXTRA_CMAKE_VARS += -DDISABLE_ASSERTIONS=1
endif
ifeq ($(DUCKDB_GEN), ninja)
	DUCKDB_CMAKE_GENERATOR := -G Ninja
	DUCKDB_CMAKE_FORCE_COLOR := -DFORCE_COLORED_OUTPUT=1
endif

# Per-consumer build dir, keyed by the EXTENSION_CONFIGS content hash
# (plus a human-readable basename prefix) so two consumers whose configs
# happen to share a basename can still coexist, and re-pointing a
# consumer at a different config naturally lands in a new dir.
DUCKDB_CONSUMER_TAG := $(if $(EXTENSION_CONFIGS),$(basename $(notdir $(EXTENSION_CONFIGS)))-$(shell shasum -a 256 '$(EXTENSION_CONFIGS)' 2>/dev/null | cut -c1-8),default)
DUCKDB_BUILD_DIR := $(PGDDB_DIR)/third_party/duckdb/build/$(DUCKDB_BUILD_TYPE)-$(DUCKDB_CONSUMER_TAG)
FULL_DUCKDB_LIB = $(DUCKDB_BUILD_DIR)/libduckdb_bundle.a

# Consumer-provided absolute path to its *_extensions.cmake. Empty = no
# third-party extensions baked in.
EXTENSION_CONFIGS ?=

.PHONY: duckdb clean-duckdb

duckdb: $(FULL_DUCKDB_LIB)

$(PGDDB_DIR)/.git/modules/third_party/duckdb/HEAD:
	git -C $(PGDDB_DIR) submodule update --init --recursive

$(FULL_DUCKDB_LIB): $(PGDDB_DIR)/.git/modules/third_party/duckdb/HEAD $(EXTENSION_CONFIGS)
	mkdir -p $(DUCKDB_BUILD_DIR)/vcpkg_installed
	cmake -S $(PGDDB_DIR)/third_party/duckdb -B $(DUCKDB_BUILD_DIR) \
		$(DUCKDB_CMAKE_GENERATOR) $(DUCKDB_CMAKE_FORCE_COLOR) \
		-DENABLE_SANITIZER=FALSE -DENABLE_UBSAN=0 \
		$(DUCKDB_CMAKE_VARS) $(DUCKDB_EXTRA_CMAKE_VARS) \
		-DDUCKDB_EXTENSION_CONFIGS="$(EXTENSION_CONFIGS)" \
		-DLOCAL_EXTENSION_REPO="" \
		-DOVERRIDE_GIT_DESCRIBE="$(DUCKDB_VERSION)" \
		-DDUCKDB_EXPLICIT_VERSION="" \
		-DCMAKE_BUILD_TYPE=$(DUCKDB_CMAKE_BUILD_TYPE)
	cmake --build $(DUCKDB_BUILD_DIR) --config $(DUCKDB_CMAKE_BUILD_TYPE)
	@# Inline of duckdb's bundle-setup + bundle-library-o targets (see
	@# third_party/duckdb/Makefile `bundle-setup` / `bundle-library-o` /
	@# `bundle-library`). Future duckdb-submodule bumps should diff
	@# against those targets to catch divergence.
	@#
	@# We whitelist archive patterns rather than globbing extension/*/*.a:
	@# lib*_extension.a is the loader stub each extension emits, and
	@# lib*_duckdb.a is the auxiliary archive pattern duckdb-vortex (and
	@# similar Rust-backed extensions) drop alongside it. The narrow
	@# whitelist avoids accidentally scooping up test fixtures cmake may
	@# leave under extension/ in future versions.
	rm -f $(DUCKDB_BUILD_DIR)/libduckdb_bundle.a
	rm -rf $(DUCKDB_BUILD_DIR)/bundle
	mkdir -p $(DUCKDB_BUILD_DIR)/bundle
	cp $(DUCKDB_BUILD_DIR)/src/libduckdb_static.a $(DUCKDB_BUILD_DIR)/bundle/.
	cp $(DUCKDB_BUILD_DIR)/third_party/*/libduckdb_*.a $(DUCKDB_BUILD_DIR)/bundle/.
	find $(DUCKDB_BUILD_DIR)/extension -maxdepth 2 \
		\( -name 'lib*_extension.a' -o -name 'lib*_duckdb.a' \
		   -o -name 'libduckdb_generated_extension_loader.a' \) \
		-exec cp {} $(DUCKDB_BUILD_DIR)/bundle/. \;
	find $(DUCKDB_BUILD_DIR)/vcpkg_installed -name '*.a' -exec cp {} $(DUCKDB_BUILD_DIR)/bundle/. \;
	cd $(DUCKDB_BUILD_DIR)/bundle && \
		find . -name '*.a' -exec mkdir -p {}.objects \; -exec mv {} {}.objects \; && \
		find . -name '*.a' -execdir $(AR) -x {} \;
	cd $(DUCKDB_BUILD_DIR)/bundle && echo ./*/*.o | xargs $(AR) cr ../libduckdb_bundle.a

clean-duckdb:
	rm -rf $(PGDDB_DIR)/third_party/duckdb/build

# Delegate make examples/<name>/<target> to examples/<name>/Makefile.
examples/%:
	$(MAKE) -C $(@D) $(@F)
