CFLAGS ?= -std=gnu99 -O4 -Wall -fPIC
EXAMPLE_BINARIES = examples/callback_parser examples/pause_parser
TEST_BINARIES = test/multipart_test
TEST_OUT_DIR = test/out
TEST_READ_SIZES = 1 2 3 5 7

default: multipart_parser.o

multipart_parser.o: multipart_parser.c multipart_parser.h

solib: multipart_parser.o
	$(CC) $(CFLAGS) -shared -Wl,-soname,libmultipart.so -o libmultipart.so multipart_parser.o

examples: $(EXAMPLE_BINARIES)

examples/% : examples/%.c multipart_parser.o
	$(CC) $(CFLAGS) -I. $< multipart_parser.o -o examples/$*

test/% : test/%.c multipart_parser.o
	$(CC) $(CFLAGS) -I. $< multipart_parser.o -o test/$*

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
