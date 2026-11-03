/*
 * honggfuzz target for the multipart parser
 *
 * One input is one multipart body with the boundary "hello world". The body is
 * fed through several schedules, plain and with callbacks that pause on every
 * event, so that execute() sees a feed end at many byte offsets. A data
 * callback must report a span inside the input of the current execute() call;
 * a violation aborts the run and honggfuzz keeps the input.
 *
 * An input that parses completely in one call must parse completely, and
 * report the same part data, under every schedule and in pause mode. A
 * difference aborts as well.
 *
 * Build and run through the Makefile:
 *   make fuzz HONGGFUZZ_DIR=/path/to/honggfuzz
 */
#include <libhfuzz/libhfuzz.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "multipart_parser.h"

#define BOUNDARY "hello world"
#define BOUNDARY_LEN (sizeof(BOUNDARY) - 1)
#define MAX_FUZZ_INPUT 65536

#define FNV_OFFSET 0xcbf29ce484222325ULL
#define FNV_PRIME 0x100000001b3ULL
#define SEED_INIT 0x9e3779b97f4a7c15ULL

enum schedule {
    sched_whole,
    sched_byte,
    sched_three,
    sched_random,
    sched_crlf,
    sched_count,
};

enum run_status {
    run_error = -1,
    run_done = 0,
    run_stalled = 1,
};

static const uint8_t *g_data;
static size_t g_data_len;
static uint64_t g_seed;
static size_t g_max_calls;

/* the input of the running execute() call, for the span check */
static size_t g_feed_off;
static size_t g_feed_len;
static size_t g_max_span_end;

static int g_pause;

/* concatenated part data of the running execute_body() call */
static uint64_t g_part_data;

static void fail(const char *what) {
    fprintf(stderr, "multipart_fuzz: %s (input %zu bytes)\n", what, g_data_len);
    abort();
}

static uint64_t rand_next(uint64_t *state) {
    uint64_t x = *state;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

static void hash_part_data(const char *at, size_t length) {
    size_t i;

    for (i = 0; i < length; i++) {
        g_part_data ^= (unsigned char)at[i];
        g_part_data *= FNV_PRIME;
    }
}

static void check_span(const char *what, const char *at, size_t length) {
    uintptr_t begin = (uintptr_t)(g_data + g_feed_off);
    uintptr_t pos = (uintptr_t)at;

    if (pos < begin || pos - begin > g_feed_len || length > g_feed_len - (pos - begin)) {
        fprintf(stderr,
            "multipart_fuzz: %s span outside feed: feed %zu bytes, span %zu bytes, input %zu\n",
            what, g_feed_len, length, g_data_len);
        abort();
    }
    if (pos - begin + length > g_max_span_end) {
        g_max_span_end = pos - begin + length;
    }
}

static long cb_header_field(multipart_parser *parser, const char *at, size_t length) {
    (void)parser;
    check_span("header field", at, length);
    return g_pause ? MPPE_PAUSED : MPPE_OK;
}

static long cb_header_value(multipart_parser *parser, const char *at, size_t length) {
    (void)parser;
    check_span("header value", at, length);
    return g_pause ? MPPE_PAUSED : MPPE_OK;
}

static long cb_part_data(multipart_parser *parser, const char *at, size_t length) {
    (void)parser;
    check_span("part data", at, length);
    hash_part_data(at, length);
    return g_pause ? MPPE_PAUSED : MPPE_OK;
}

static long cb_part_data_begin(multipart_parser *parser) {
    (void)parser;
    return g_pause ? MPPE_PAUSED : MPPE_OK;
}

static long cb_headers_complete(multipart_parser *parser) {
    (void)parser;
    return g_pause ? MPPE_PAUSED : MPPE_OK;
}

static long cb_part_data_end(multipart_parser *parser) {
    (void)parser;
    return g_pause ? MPPE_PAUSED : MPPE_OK;
}

static long cb_body_end(multipart_parser *parser) {
    (void)parser;
    return g_pause ? MPPE_PAUSED : MPPE_OK;
}

static const multipart_parser_settings g_settings = {
    .on_header_field = cb_header_field,
    .on_header_value = cb_header_value,
    .on_part_data = cb_part_data,
    .on_part_data_begin = cb_part_data_begin,
    .on_headers_complete = cb_headers_complete,
    .on_part_data_end = cb_part_data_end,
    .on_body_end = cb_body_end,
};

/* the next feed end offset after avail, by schedule */
static size_t next_avail(size_t avail, enum schedule sched, uint64_t *rng) {
    size_t step = 1;

    switch (sched) {
        case sched_whole:
            step = g_data_len;
            break;
        case sched_byte:
            step = 1;
            break;
        case sched_three:
            step = 3;
            break;
        case sched_random:
            step = 1 + (size_t)(rand_next(rng) % 7);
            break;
        case sched_crlf: {
            size_t pos = avail;

            while (pos < g_data_len && g_data[pos] != '\r' && g_data[pos] != '\n') {
                pos++;
            }
            step = pos - avail + 1;
            break;
        }
        case sched_count:
            break;
    }
    if (step > g_data_len - avail) {
        step = g_data_len - avail;
    }
    return avail + step;
}

static int execute_body(enum schedule sched, int pause) {
    multipart_parser parser;
    uint64_t rng = g_seed;
    size_t pos = 0;
    size_t avail = 0;
    size_t calls = 0;
    int retried = 0;

    g_pause = pause;
    g_part_data = FNV_OFFSET;

    memset(&parser, 0, sizeof(parser));
    if (multipart_parser_init(&parser, BOUNDARY, BOUNDARY_LEN, &g_settings) != 0) {
        fail("parser init failed");
    }

    for (;;) {
        size_t parsed;

        while (pos < avail) {
            if (calls++ >= g_max_calls) {
                fail("parser does not make progress");
            }
            g_feed_off = pos;
            g_feed_len = avail - pos;
            g_max_span_end = 0;
            parsed = multipart_parser_execute(&parser, (const char *)g_data + pos, g_feed_len);
            if (parsed == MPPE_ERROR) {
                return run_error;
            }
            if (parsed > g_feed_len) {
                fail("parser consumed more bytes than given");
            }
            if (g_max_span_end > parsed) {
                fail("callback span ends after the consumed bytes");
            }
            if (parsed == 0) {
                break;
            }
            retried = 0;
            pos += parsed;
        }
        if (pos == avail) {
            if (avail == g_data_len) {
                return run_done;
            }
            avail = next_avail(avail, sched, &rng);
            continue;
        }
        /* bytes remain and the parser consumed nothing */
        if (avail == g_data_len) {
            if (retried) {
                return run_stalled;
            }
            /* a notify callback paused at offset 0 can continue on a retry */
            retried = 1;
            continue;
        }
        avail = next_avail(avail, sched, &rng);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    uint64_t expected;
    int reference;
    int pause;
    enum schedule sched;
    size_t i;

    if (size > MAX_FUZZ_INPUT) {
        size = MAX_FUZZ_INPUT;
    }
    if (size == 0) {
        return 0;
    }
    g_data = data;
    g_data_len = size;
    g_max_calls = size * 8 + 1024;

    g_seed = SEED_INIT;
    for (i = 0; i < size; i++) {
        g_seed ^= data[i];
        g_seed *= FNV_PRIME;
    }
    if (g_seed == 0) {
        g_seed = SEED_INIT;
    }

    reference = execute_body(sched_whole, 0);
    expected = g_part_data;

    for (sched = 0; sched < sched_count; sched++) {
        for (pause = 0; pause <= 1; pause++) {
            int status;

            if (sched == sched_whole && pause == 0) {
                continue;
            }
            status = execute_body(sched, pause);
            if (reference != run_done) {
                continue;
            }
            if (status != run_done) {
                fprintf(stderr,
                    "multipart_fuzz: complete input does not complete with schedule %d, pause %d\n",
                    (int)sched, pause);
                abort();
            }
            if (g_part_data != expected) {
                fprintf(stderr,
                    "multipart_fuzz: part data depends on the feeds, schedule %d, pause %d\n",
                    (int)sched, pause);
                abort();
            }
        }
    }
    return 0;
}
