CC := gcc
CFLAGS := -std=c23 -Wall -Wextra -Werror -g -O1 -Iinclude -Isrc -Itests

SRC := src/rbtree.c src/pool.c
TSRC := tests/test_rbtree.c tests/fault_alloc.c tests/sweep.c tests/test_pool.c
BIN := build/test_rbtree
FUZZBIN := build/fuzz

all: $(BIN) $(FUZZBIN)

$(BIN): $(SRC) $(TSRC) include/rbtree.h src/pool.h tests/fault_alloc.h tests/sweep.h tests/test_pool.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) $(TSRC) -o $@

$(FUZZBIN): $(SRC) tests/fuzz.c tests/fault_alloc.c include/rbtree.h src/pool.h tests/fault_alloc.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) tests/fuzz.c tests/fault_alloc.c -o $@

test: $(BIN) $(FUZZBIN)
	./$(BIN) && ./$(BIN) --pooled && ./$(FUZZBIN) 100000 && ./$(FUZZBIN) 100000 --pooled

asan: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
asan: clean test

memcheck: all
	valgrind --leak-check=full --show-leak-kinds=all \
	--error-exitcode=1 ./$(BIN)
	valgrind --leak-check=full --show-leak-kinds=all \
	--error-exitcode=1 ./$(BIN) --pooled
	valgrind --leak-check=full --show-leak-kinds=all \
	--error-exitcode=1 ./$(FUZZBIN) 20000
	valgrind --leak-check=full --show-leak-kinds=all \
	--error-exitcode=1 ./$(FUZZBIN) 20000 --pooled

clean:
	rm -rf build

.PHONY: all test asan memcheck clean
