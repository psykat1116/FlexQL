CXX      := g++
CXXFLAGS := -std=c++17 -O3 -Wall -Wextra -Wno-unused-function -pthread
INCLUDES := -I./include

SRCS_STORAGE := src/storage/page.cpp          \
                src/storage/table_manager.cpp
SRCS_CACHE   := src/cache/lru_cache.cpp
SRCS_INDEX   := src/index/bplus_tree.cpp
SRCS_PARSER  := src/parser/lexer.cpp          \
                src/parser/parser.cpp
SRCS_QUERY   := src/query/executor.cpp
SRCS_NETWORK := src/network/net_utils.cpp
SRCS_WORKER  := src/worker/worker.cpp

SRCS_SHARED  := $(SRCS_STORAGE)  \
                $(SRCS_CACHE)    \
                $(SRCS_INDEX)    \
                $(SRCS_PARSER)   \
                $(SRCS_QUERY)    \
                $(SRCS_NETWORK)

obj = $(patsubst src/%.cpp, build/%.o, $(1))
tobj = $(patsubst tests/%.cpp, build/tests/%.o, $(1))

OBJS_SHARED    := $(call obj,$(SRCS_SHARED))
OBJS_WORKER    := $(call obj,$(SRCS_WORKER))
OBJS_SERVER    := $(call obj,src/server/main.cpp)
OBJS_CLIENT    := $(call obj,src/client/flexql_client.cpp) \
                  $(call obj,src/client/repl.cpp)
OBJS_TEST      := $(call tobj,tests/test_all.cpp)
OBJS_BENCHMARK := $(call tobj,tests/benchmark_flexql.cpp) \
                  $(call obj,src/client/flexql_client.cpp)

.PHONY: all clean test benchmark run-server run-client

all: bin/flexql-server bin/flexql-client
	@echo ""
	@echo "Build successful!"
	@echo "Start server:  ./bin/flexql-server 3000 ./database"
	@echo "Start client:  ./bin/flexql-client 127.0.0.1 3000"
	@echo "Run tests:     make test"
	@echo "Benchmark:     make benchmark && ./bin/flexql-benchmark"

bin/flexql-server: $(OBJS_SHARED) $(OBJS_WORKER) $(OBJS_SERVER)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $@ $^
	@echo "Linked: $@"

bin/flexql-client: $(OBJS_SHARED) $(OBJS_CLIENT)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $@ $^
	@echo "Linked: $@"

bin/flexql-benchmark: $(OBJS_SHARED) $(OBJS_BENCHMARK)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $@ $^
	@echo "Linked: $@"

build/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

build/tests/%.o: tests/%.cpp
	@mkdir -p build/tests
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

benchmark: bin/flexql-benchmark
	@echo ""
	@echo "Benchmark binary built: ./bin/flexql-benchmark"
	@echo "Usage:"
	@echo "  Terminal 1: ./bin/flexql-server 3000 ./database"
	@echo "  Terminal 2: ./bin/flexql-benchmark [rows | --unit-test]"
	@echo "  Default:    ./bin/flexql-benchmark 10000000"
	@echo ""

run-server: bin/flexql-server
	@mkdir -p database
	./bin/flexql-server 3000 ./database

run-client: bin/flexql-client
	./bin/flexql-client 127.0.0.1 3000

clean:
	rm -rf build/ bin/
	@echo "Cleaned build/ and bin/"
