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

.PHONY: all clean test cases official handshake spec edge crash bench recall asan material

all: $(BIN)

$(BIN): $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC) $(LDLIBS)

# The public test package of the contest, executed over the stdio protocol.
cases: $(BIN)
	python3 tests/run_cases.py --binary ./$(BIN) --package mysqltest

# The official self test driver of the contest package.  It uses the same
# assertion semantics as the judge; --command runs the binary directly, so no
# docker is needed.  The image based run (--image) is what the platform does.
official: $(BIN)
	python3 protocol_check/run.py --command '["$(CURDIR)/$(BIN)","--data-dir","{data_dir}"]' \
	    --package mysqltest --output "protocol-results/public-$$(date +%Y%m%d-%H%M%S)"

# Only the startup and the shutdown, without any SQL.
handshake: $(BIN)
	python3 protocol_check/run.py --command '["$(CURDIR)/$(BIN)","--data-dir","{data_dir}"]' \
	    --handshake-only --output "protocol-results/handshake-$$(date +%Y%m%d-%H%M%S)"

# The acceptance check of the protocol specification.
spec: $(BIN)
	python3 tests/spec_check.py --binary ./$(BIN)

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
