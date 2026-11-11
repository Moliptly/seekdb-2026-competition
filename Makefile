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
# The identifier of the build: the hash of the sources.  It is printed on
# stderr when the database starts, so the log of a case shows which build
# produced it.
BUILD_ID := $(shell cat $(SRC) $(HDR) | sha256sum | cut -c1-12)
BUILD_FLAGS := -DMVDB_BUILD_ID='"$(BUILD_ID)"'

.PHONY: all clean test cases official handshake spec edge crash bench recall asan material fuzz hiddenlike

all: $(BIN)

$(BIN): $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(BUILD_FLAGS) $(LDFLAGS) -o $@ $(SRC) $(LDLIBS)

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

# The approximate mode has to agree with the exact one, the index has to
# follow the records of the table and the vector search has to match a brute
# force truth.
# Cases written in the style of the mysqltest package, with the expected
# results derived from the rules by hand.
hiddenlike: $(BIN)
	python3 protocol_check/run.py --command '["./$(BIN)","--data-dir","{data_dir}"]' \
	    --package tests/hidden_like \
	    --output "protocol-results/hidden-like-$(shell date +%Y%m%d-%H%M%S)"

fuzz: $(BIN)
	python3 tests/ann_fuzz.py --binary ./$(BIN) --rounds 120 --large 12000
	python3 tests/topk_fuzz.py --binary ./$(BIN) --rounds 120
	python3 tests/index_maintenance_test.py --binary ./$(BIN)

# The same tests with the address and undefined behaviour sanitizers.
asan: $(SRC) $(HDR)
	$(CC) -std=gnu11 -O1 -g -fsanitize=address,undefined \
	    -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation \
	    $(BUILD_FLAGS) -o $(BIN)-asan $(SRC) $(LDLIBS)

# Rebuild the agent prompt material from the recorded experiments.
material:
	python3 tests/build_conversation.py

clean:
	rm -f $(BIN) $(BIN)-asan
