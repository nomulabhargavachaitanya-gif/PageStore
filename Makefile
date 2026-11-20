CC       := cc
CFLAGS   := -std=c11 -O2 -Wall -Wextra -Werror -Wpedantic -pthread -Iinclude
SANFLAGS := -std=c11 -g -O1 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer -Iinclude

CORE_SRC := src/crc32.c \
            src/hashtable.c \
            src/avl.c \
            src/bloom.c \
            src/wal.c \
            src/btree.c \
            src/varrec.c \
            src/secindex.c \
            src/bufferpool.c \
            src/pagestore.c

CORE_OBJ := $(patsubst src/%.c,build/%.o,$(CORE_SRC))
HEADERS  := include/pagestore.h \
            include/avl.h \
            include/hashtable.h \
            include/bufferpool.h \
            include/crc32.h \
            include/bloom.h \
            include/wal.h \
            include/btree.h \
            include/varrec.h \
            include/secindex.h

.PHONY: all server test test-asan test-fuzz bench bench-ci clean

all: build/pagestore build/pagestored

server: build/pagestored

build:
	@mkdir -p build

build/%.o: src/%.c $(HEADERS) | build
	$(CC) $(CFLAGS) -c $< -o $@

build/pagestore: $(CORE_OBJ) cli/main.c | build
	$(CC) $(CFLAGS) $(CORE_OBJ) cli/main.c -o $@

build/pagestored: $(CORE_OBJ) cli/server.c | build
	$(CC) $(CFLAGS) $(CORE_OBJ) cli/server.c -o $@

build/test_main: $(CORE_OBJ) tests/test_main.c | build
	$(CC) $(CFLAGS) $(CORE_OBJ) tests/test_main.c -o $@

build/fuzz_page: $(CORE_OBJ) fuzz/fuzz_page.c | build
	$(CC) $(CFLAGS) $(CORE_OBJ) fuzz/fuzz_page.c -o $@

build/bench_main: $(CORE_OBJ) bench/bench_main.c | build
	$(CC) $(CFLAGS) $(CORE_OBJ) bench/bench_main.c -o $@

test: build/test_main build/fuzz_page
	./build/test_main
	./build/fuzz_page

test-fuzz: build/fuzz_page
	./build/fuzz_page

test-asan: | build
	$(CC) $(SANFLAGS) $(CORE_SRC) tests/test_main.c -o build/test_asan
	./build/test_asan

bench: build/bench_main
	./build/bench_main

bench-ci: build/bench_main
	@set -e; out="$$(./build/bench_main)"; \
	  echo "$$out"; \
	  ops=$$(echo "$$out" | awk '/Warm pool/ {gsub(/[()]/, "", $$8); printf "%d", $$8}'); \
	  if [ -z "$$ops" ] || [ "$$ops" -lt 1000000 ]; then \
	    echo "FAIL: warm pool throughput ($$ops ops/sec) below 1,000,000 ops/sec baseline"; \
	    exit 1; \
	  fi; \
	  echo "PASS: warm pool throughput $$ops ops/sec >= 1,000,000 ops/sec baseline"

clean:
	rm -rf build /tmp/pagestore_fuzz_db
