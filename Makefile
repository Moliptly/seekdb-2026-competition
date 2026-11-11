# MiniVectorDB - build and local test entry points.
#
# The database is plain C11 without any third party dependency, so a single
# compiler invocation produces the binary.

CC      ?= gcc
CFLAGS  ?= -std=gnu11 -O3 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
LDFLAGS ?=
LDLIBS  ?= -lm

SRC := $(wildcard src/*.c)
HDR := src/mvdb.h
BIN := minivectordb

.PHONY: all clean test cases edge crash bench recall asan material

all: $(BIN)

$(BIN): $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC) $(LDLIBS)

# The public test package of the contest, executed over the stdio protocol.
cases: $(BIN)
	python3 tests/run_cases.py --binary ./$(BIN) --package mysqltest

# The edge cases that the public package does not cover.
edge: $(BIN)
	python3 tests/edge_test.py --binary ./$(BIN)

# Crash, durability and recovery behaviour.
crash: $(BIN)
	python3 tests/crash_test.py --binary ./$(BIN)

# The performance benchmark of the contest (10k rows, 128 dimensions,
# 1000 TopK queries).
bench: $(BIN)
	python3 tests/bench.py --binary ./$(BIN) --mode ann --index

# The recall of the approximate mode against the exact ground truth.
recall: $(BIN)
	python3 tests/recall.py --binary ./$(BIN)

# The same tests with the address and undefined behaviour sanitizers.
asan: $(SRC) $(HDR)
	$(CC) -std=gnu11 -O1 -g -fsanitize=address,undefined \
	    -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation \
	    -o $(BIN)-asan $(SRC) $(LDLIBS)

# Rebuild the agent prompt material from the recorded experiments.
material:
	python3 tests/build_conversation.py

clean:
	rm -f $(BIN) $(BIN)-asan
