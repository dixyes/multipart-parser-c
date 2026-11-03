CFLAGS ?= -std=gnu99 -O4 -Wall -fPIC
EXAMPLE_BINARIES = examples/callback_parser examples/pause_parser
TEST_BINARIES = test/multipart_test
TEST_OUT_DIR = test/out
TEST_READ_SIZES = 1 2 3 5 7

FUZZ_BIN = test/out/multipart_fuzz
FUZZ_CORPUS = test/fuzz-corpus
FUZZ_OUT_DIR = test/out/fuzz
# empty FUZZ_RUN_TIME runs until interrupted
FUZZ_RUN_TIME ?=
FUZZ_ARGS = $(if $(FUZZ_RUN_TIME),--run_time $(FUZZ_RUN_TIME),)
FUZZ_THREADS ?= 2
FUZZ_MAX_INPUT ?= 4096
# a built honggfuzz tree; without it hfuzz-clang and honggfuzz must be in PATH
HONGGFUZZ_DIR ?=
FUZZ_CC ?= $(if $(HONGGFUZZ_DIR),$(HONGGFUZZ_DIR)/hfuzz_cc/hfuzz-clang,hfuzz-clang)
FUZZER ?= $(if $(HONGGFUZZ_DIR),$(HONGGFUZZ_DIR)/honggfuzz,honggfuzz)

default: multipart_parser.o

multipart_parser.o: multipart_parser.c multipart_parser.h

solib: multipart_parser.o
	$(CC) $(CFLAGS) -shared -Wl,-soname,libmultipart.so -o libmultipart.so multipart_parser.o

examples: $(EXAMPLE_BINARIES)

examples/% : examples/%.c multipart_parser.o
	$(CC) $(CFLAGS) -I. $< multipart_parser.o -o examples/$*

test/% : test/%.c multipart_parser.o
	$(CC) $(CFLAGS) -I. $< multipart_parser.o -o test/$*

$(FUZZ_BIN): test/multipart_fuzz.c multipart_parser.c multipart_parser.h
	@mkdir -p $(dir $@)
	HFUZZ_CC_ASAN=1 HFUZZ_CC_UBSAN=1 $(FUZZ_CC) -std=gnu99 -O1 -g -Wall -Wextra -I. \
	    test/multipart_fuzz.c multipart_parser.c -o $@

check-fuzz-tools:
	@command -v "$(FUZZ_CC)" >/dev/null 2>&1 || { \
	    echo "hfuzz-clang not found, set HONGGFUZZ_DIR to a built honggfuzz tree" >&2; exit 1; }
	@command -v "$(FUZZER)" >/dev/null 2>&1 || { \
	    echo "honggfuzz not found, set HONGGFUZZ_DIR to a built honggfuzz tree" >&2; exit 1; }

fuzz: check-fuzz-tools $(FUZZ_BIN)
	@mkdir -p $(FUZZ_OUT_DIR)/seeds $(FUZZ_OUT_DIR)/corpus $(FUZZ_OUT_DIR)/work $(FUZZ_OUT_DIR)/crashes
	cp test/hello_world.data $(FUZZ_OUT_DIR)/seeds/hello_world.bin
	cp $(FUZZ_CORPUS)/*.bin $(FUZZ_OUT_DIR)/seeds/
	$(FUZZER) -i $(FUZZ_OUT_DIR)/seeds -o $(FUZZ_OUT_DIR)/corpus \
	    -W $(FUZZ_OUT_DIR)/work --crashdir $(FUZZ_OUT_DIR)/crashes \
	    $(FUZZ_ARGS) -n $(FUZZ_THREADS) -t 5 -F $(FUZZ_MAX_INPUT) --sanitizers \
	    -- $(FUZZ_BIN)
	@echo "crash files in $(FUZZ_OUT_DIR)/crashes:"
	@ls -1 $(FUZZ_OUT_DIR)/crashes 2>/dev/null || true

tests: $(EXAMPLE_BINARIES) $(TEST_BINARIES)
	@mkdir -p $(TEST_OUT_DIR)
	./test/multipart_test
	@echo "checking example output, whole buffer"
	cat test/hello_world.data | examples/callback_parser > $(TEST_OUT_DIR)/callback_parser.out
	diff -u test/hello_world.expected $(TEST_OUT_DIR)/callback_parser.out
	cat test/hello_world.data | examples/pause_parser > $(TEST_OUT_DIR)/pause_parser.out
	diff -u test/hello_world.expected $(TEST_OUT_DIR)/pause_parser.out
	@set -e; \
	for size in $(TEST_READ_SIZES); do \
	    echo "checking example output, read size $$size"; \
	    cat test/hello_world.data | examples/callback_parser "hello world" $$size > $(TEST_OUT_DIR)/callback_parser.$$size.out; \
	    diff -u test/hello_world.expected $(TEST_OUT_DIR)/callback_parser.$$size.out; \
	    cat test/hello_world.data | examples/pause_parser "hello world" $$size > $(TEST_OUT_DIR)/pause_parser.$$size.out; \
	    diff -u test/hello_world.expected $(TEST_OUT_DIR)/pause_parser.$$size.out; \
	done
	@echo "all tests passed"

clean:
	-rm -f *.o *.so
	-rm $(EXAMPLE_BINARIES)
	-rm -f $(TEST_BINARIES)
	-rm -rf $(TEST_OUT_DIR)
