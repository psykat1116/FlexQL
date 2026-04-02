# ================================================================
#  FlexQL — Makefile
#
#  Targets:
#    make all           — build server + client
#    make test          — build and run all tests
#    make benchmark     — build benchmark binary
#    make clean         — remove all build artifacts
#    make run-server    — start server on port 3000
#    make run-client    — start interactive client
#
#  Directory layout:
#    include/  — all .h header files (subdirs per module)
#    src/      — all .cpp source files (subdirs per module)
#    bin/      — final executables
#    build/    — intermediate .o object files
#    tests/    — test sources
# ================================================================

CXX      := g++
CXXFLAGS := -std=c++17 -O3 -Wall -Wextra -Wno-unused-function -pthread
INCLUDES := -I./include

# ── Source files grouped by module ───────────────────────────
SRCS_STORAGE := src/storage/page.cpp          \
                src/storage/table_manager.cpp
SRCS_CACHE   := src/cache/lru_cache.cpp
SRCS_INDEX   := src/index/bplus_tree.cpp
SRCS_PARSER  := src/parser/lexer.cpp          \
                src/parser/parser.cpp
SRCS_QUERY   := src/query/executor.cpp
SRCS_NETWORK := src/network/net_utils.cpp

SRCS_SHARED  := $(SRCS_STORAGE)  \
                $(SRCS_CACHE)    \
                $(SRCS_INDEX)    \
                $(SRCS_PARSER)   \
                $(SRCS_QUERY)    \
                $(SRCS_NETWORK)

obj = $(patsubst src/%.cpp, build/%.o, $(1))
tobj = $(patsubst tests/%.cpp, build/tests/%.o, $(1))

OBJS_SHARED    := $(call obj,$(SRCS_SHARED))
OBJS_SERVER    := $(call obj,src/server/main.cpp)
OBJS_CLIENT    := $(call obj,src/client/flexql_client.cpp) \
                  $(call obj,src/client/repl.cpp)
OBJS_TEST      := $(call tobj,tests/test_all.cpp)
OBJS_BENCHMARK := $(call tobj,tests/benchmark_flexql.cpp) \
                  $(call obj,src/client/flexql_client.cpp)

.PHONY: all clean test benchmark run-server run-client

# ── Default target ────────────────────────────────────────────
all: bin/flexql-server bin/flexql-client
	@echo ""
	@echo "Build successful!"
	@echo "  Start server:  ./bin/flexql-server 3000 ./data"
	@echo "  Start client:  ./bin/flexql-client 127.0.0.1 3000"
	@echo "  Run tests:     make test"
	@echo "  Benchmark:     make benchmark && ./bin/flexql-benchmark"

# ── Server binary ─────────────────────────────────────────────
bin/flexql-server: $(OBJS_SHARED) $(OBJS_SERVER)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $@ $^
	@echo "Linked: $@"

# ── Client binary ─────────────────────────────────────────────
bin/flexql-client: $(OBJS_SHARED) $(OBJS_CLIENT)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $@ $^
	@echo "Linked: $@"

# ── Test binary ───────────────────────────────────────────────
bin/flexql-test: $(OBJS_SHARED) $(OBJS_TEST)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $@ $^
	@echo "Linked: $@"

# ── Benchmark binary ──────────────────────────────────────────
bin/flexql-benchmark: $(OBJS_SHARED) $(OBJS_BENCHMARK)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $@ $^
	@echo "Linked: $@"

# ── Generic compile rule for src/ files ───────────────────────
build/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# ── Compile rule for test/benchmark files ─────────────────────
build/tests/%.o: tests/%.cpp
	@mkdir -p build/tests
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# ── test target ───────────────────────────────────────────────
test: bin/flexql-test
	@echo ""
	@echo "================================================"
	@echo "  Running FlexQL Test Suite"
	@echo "================================================"
	@./bin/flexql-test
	@echo ""

# ── benchmark target ──────────────────────────────────────────
benchmark: bin/flexql-benchmark
	@echo ""
	@echo "Benchmark binary built: ./bin/flexql-benchmark"
	@echo "Usage:"
	@echo "  Terminal 1: ./bin/flexql-server 3000 ./data"
	@echo "  Terminal 2: ./bin/flexql-benchmark [rows | --unit-test]"
	@echo "  Default:    ./bin/flexql-benchmark 10000000"
	@echo ""

# ── Convenience run targets ───────────────────────────────────
run-server: bin/flexql-server
	@mkdir -p data
	./bin/flexql-server 3000 ./data

run-client: bin/flexql-client
	./bin/flexql-client 127.0.0.1 3000

# ── Clean ─────────────────────────────────────────────────────
clean:
	rm -rf build/ bin/
	@echo "Cleaned build/ and bin/"
