/*
* Regression tests for multipart_parser
*
* run all checks with: make tests
*
* every check_*() function returns 0 on success and 1 on failure
*/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "multipart_parser.h"

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    check failed at line %d: %s\n", __LINE__, #cond); \
        return 1; \
    } \
} while (0)

static const multipart_parser_settings settings_none = { 0 };

static const char body[] =
    "--abc\r\n"
    "Content-Disposition: form-data; name=\"a\"\r\n"
    "\r\n"
    "VALUE\r\n"
    "--abc--\r\n";

/* ---- F1: part data span must be correct across any feed split ---- */

static const char *feed_base;
static size_t feed_len;
static int feed_bad_span;
static int feed_pause;
static char part_data[64];
static size_t part_data_len;

static int add_part_data(const char *at, size_t length) {
    if (at < feed_base) {
        return 0;
    }
    if ((size_t)(at - feed_base) > feed_len ||
            length > feed_len - (size_t)(at - feed_base)) {
        return 0;
    }
    if (length > sizeof(part_data) - part_data_len) {
        return 0;
    }
    memcpy(part_data + part_data_len, at, length);
    part_data_len += length;
    return 1;
}

static long on_header(multipart_parser *p, const char *at, size_t length) {
    (void)p;
    (void)at;
    (void)length;
    return MPPE_OK;
}

static long on_part_data(multipart_parser *p, const char *at, size_t length) {
    (void)p;
    if (!add_part_data(at, length)) {
        feed_bad_span = 1;
        return MPPE_ERROR;
    }
    return feed_pause ? MPPE_PAUSED : MPPE_OK;
}

static long on_notify(multipart_parser *p) {
    (void)p;
    return MPPE_OK;
}

static const multipart_parser_settings settings_feeds = {
    .on_header_field = on_header,
    .on_header_value = on_header,
    .on_part_data = on_part_data,
    .on_part_data_begin = on_notify,
    .on_headers_complete = on_notify,
    .on_part_data_end = on_notify,
    .on_body_end = on_notify,
};

/* feed body with one cut after `split` bytes, `pause` makes on_part_data pause */
static int parse_with_split(size_t split, int pause) {
    multipart_parser parser;
    size_t len = sizeof(body) - 1;
    size_t pos = 0;
    size_t avail = split;

    feed_pause = pause;
    feed_bad_span = 0;
    part_data_len = 0;
    memset(&parser, 0, sizeof(parser));
    if (multipart_parser_init(&parser, "abc", 3, &settings_feeds) != 0) {
        return 1;
    }

    while (pos < len) {
        size_t parsed;
        if (avail <= pos) {
            avail = len;
        }
        feed_base = body + pos;
        feed_len = avail - pos;
        parsed = multipart_parser_execute(&parser, body + pos, avail - pos);
        if (parsed == MPPE_ERROR) {
            return 1;
        }
        if (parsed > avail - pos) {
            return 1;
        }
        if (parsed == 0) {
            if (avail >= len) {
                return 1;
            }
            avail = len;
            continue;
        }
        pos += parsed;
    }

    if (feed_bad_span != 0) {
        return 1;
    }
    if (part_data_len != 5 || memcmp(part_data, "VALUE", 5) != 0) {
        return 1;
    }
    return 0;
}

static int check_part_data_across_feeds(void) {
    size_t len = sizeof(body) - 1;
    size_t split;
    int pause;

    for (split = 0; split <= len; split++) {
        for (pause = 0; pause < 2; pause++) {
            if (parse_with_split(split, pause) != 0) {
                printf("    split=%zu pause=%d\n", split, pause);
                return 1;
            }
        }
    }
    return 0;
}

/* ---- EXTRA: OK mode must not depend on how the input is fed ----
 *
 * two defects outside the audit's F1..F4 list, both in MPPE_OK mode:
 *   (a) a CRLF inside part data is lost when a feed ends on its CR
 *   (b) execute() returns 0 after having advanced state, so re-feeding the
 *       same bytes corrupts the parse and a boundary is read as part data
 */

static char feed_buffer[256];
static size_t feed_buffer_len;
static int feed_zero_returns;
static int part_data_begin_count;

struct feed
{
    multipart_parser parser;
};

/* append data and feed it, keeping unconsumed bytes like a streaming consumer */
static int feed_add(struct feed *f, const char *data, size_t length) {
    size_t parsed;

    if (length > sizeof(feed_buffer) - feed_buffer_len) {
        return 1;
    }
    memcpy(feed_buffer + feed_buffer_len, data, length);
    feed_buffer_len += length;
    while (feed_buffer_len > 0) {
        feed_base = feed_buffer;
        feed_len = feed_buffer_len;
        parsed = multipart_parser_execute(&f->parser, feed_buffer, feed_buffer_len);
        if (parsed == MPPE_ERROR) {
            return 1;
        }
        if (parsed > feed_buffer_len) {
            return 1;
        }
        if (parsed == 0) {
            feed_zero_returns++;
            return 0;
        }
        memmove(feed_buffer, feed_buffer + parsed, feed_buffer_len - parsed);
        feed_buffer_len -= parsed;
    }
    return 0;
}

static long on_part_data_begin_counted(multipart_parser *p) {
    (void)p;
    part_data_begin_count++;
    return MPPE_OK;
}

static const multipart_parser_settings settings_counted = {
    .on_header_field = on_header,
    .on_header_value = on_header,
    .on_part_data = on_part_data,
    .on_part_data_begin = on_part_data_begin_counted,
    .on_headers_complete = on_notify,
    .on_part_data_end = on_notify,
    .on_body_end = on_notify,
};

static int feed_start(struct feed *f) {
    feed_buffer_len = 0;
    feed_zero_returns = 0;
    feed_pause = 0;
    part_data_len = 0;
    part_data_begin_count = 0;
    memset(&f->parser, 0, sizeof(f->parser));
    return multipart_parser_init(&f->parser, "abc", 3, &settings_counted) != 0;
}


/* feed `data` in chunks of `chunk` bytes, appending to the unconsumed bytes */
static int feed_chunks(struct feed *f, const char *data, size_t length, size_t chunk) {
    size_t pos = 0;

    while (pos < length) {
        size_t n = chunk < length - pos ? chunk : length - pos;
        if (feed_add(f, data + pos, n) != 0) {
            return 1;
        }
        pos += n;
    }
    if (feed_buffer_len != 0) {
        printf("    %zu bytes were not consumed\n", feed_buffer_len);
        return 1;
    }
    return 0;
}

/* (a) a CRLF inside part data must not be lost when a feed ends on its CR */
static int check_part_data_crlf_split(void) {
    static const char split_body[] = "--abc\r\n\r\nhello\r\nworld\r\n--abc--\r\n";
    static const char want[] = "hello\r\nworld";
    struct feed f;

    /* 3-byte feeds put a feed boundary right after the part-data CR */
    if (feed_start(&f) != 0) {
        return 1;
    }
    if (feed_chunks(&f, split_body, sizeof(split_body) - 1, 3) != 0) {
        return 1;
    }
    if (part_data_len != sizeof(want) - 1 || memcmp(part_data, want, part_data_len) != 0) {
        printf("    part data is \"%.*s\"\n", (int)part_data_len, part_data);
        return 1;
    }
    return 0;
}

/* (b) a 0 return consumes nothing: the parser must not advance its persisted
 * state for bytes it reports as not consumed, or the next feed reads the
 * boundary as part data */
static int check_zero_return_state(void) {
    static const char two_parts[] =
        "--abc\r\n"
        "A: 1\r\n"
        "\r\n"
        "ONE\r\n"
        "--abc\r\n"
        "B: 2\r\n"
        "\r\n"
        "TWO\r\n"
        "--abc--\r\n";
    struct feed f;

    /* 5-byte feeds make execute() stop inside the second boundary */
    if (feed_start(&f) != 0) {
        return 1;
    }
    if (feed_chunks(&f, two_parts, sizeof(two_parts) - 1, 5) != 0) {
        return 1;
    }
    if (feed_zero_returns == 0) {
        printf("    no 0 return, the check does not exercise the contract\n");
        return 1;
    }
    if (part_data_begin_count != 2) {
        printf("    part_data_begin fired %d times\n", part_data_begin_count);
        return 1;
    }
    if (part_data_len != 6 || memcmp(part_data, "ONETWO", 6) != 0) {
        printf("    part data is \"%.*s\"\n", (int)part_data_len, part_data);
        return 1;
    }
    return 0;
}

/* ---- F2: init() with boundary == NULL must not trust a garbage length ---- */

static int check_init_without_boundary(void) {
    multipart_parser *parser = malloc(sizeof(*parser));
    multipart_parser stack_parser;

    CHECK(parser != NULL);
    /* simulate a parser that was never initialised */
    memset(parser, 0xAA, sizeof(*parser));
    CHECK(multipart_parser_init(parser, NULL, 0, &settings_none) == 0);
    CHECK(parser->boundary_length == 0);
    free(parser);

    /* boundary == NULL keeps the boundary of an initialised parser */
    memset(&stack_parser, 0, sizeof(stack_parser));
    CHECK(multipart_parser_init(&stack_parser, "abc", 3, &settings_none) == 0);
    CHECK(multipart_parser_init(&stack_parser, NULL, 0, &settings_none) == 0);
    CHECK(stack_parser.boundary_length == 5);
    CHECK(multipart_parser_execute(&stack_parser, body, sizeof(body) - 1) == sizeof(body) - 1);
    return 0;
}

/* ---- F3: header value reported once when the feed ends on its CR ---- */

static char header_value[64];
static size_t header_value_len;
static int header_value_count;

static long on_header_value(multipart_parser *p, const char *at, size_t length) {
    (void)p;
    header_value_count++;
    if (length > sizeof(header_value) - header_value_len) {
        return MPPE_ERROR;
    }
    memcpy(header_value + header_value_len, at, length);
    header_value_len += length;
    return MPPE_OK;
}

static int check_header_value_split(void) {
    static const char split_body[] = "--abc\r\nX: v\r\n\r\nZ\r\n--abc--\r\n";
    static const multipart_parser_settings settings_value = {
        .on_header_value = on_header_value,
    };
    const char *value = strstr(split_body, "v\r\n\r\n");
    multipart_parser parser;
    size_t len = sizeof(split_body) - 1;
    size_t split;

    CHECK(value != NULL);
    /* the first feed ends right after the CR of the header value */
    split = (size_t)(value - split_body) + 2;
    header_value_count = 0;
    header_value_len = 0;
    memset(&parser, 0, sizeof(parser));
    CHECK(multipart_parser_init(&parser, "abc", 3, &settings_value) == 0);
    CHECK(multipart_parser_execute(&parser, split_body, split) == split);
    CHECK(multipart_parser_execute(&parser, split_body + split, len - split) == len - split);
    CHECK(header_value_count == 1);
    CHECK(header_value_len == 1 && header_value[0] == 'v');
    return 0;
}

/* ---- F4: header field names accept the full tchar set ---- */

static char header_field[64];
static size_t header_field_len;

static long on_header_field(multipart_parser *p, const char *at, size_t length) {
    (void)p;
    if (length > sizeof(header_field) - header_field_len) {
        return MPPE_ERROR;
    }
    memcpy(header_field + header_field_len, at, length);
    header_field_len += length;
    return MPPE_OK;
}

/* returns the parsed length, or MPPE_ERROR */
static size_t parse_header_field(const char *name, multipart_parser *parser) {
    static const multipart_parser_settings settings_field = {
        .on_header_field = on_header_field,
    };
    char buffer[256];
    size_t len = (size_t)snprintf(buffer, sizeof(buffer),
            "--abc\r\n%s: v\r\n\r\nZ\r\n--abc--\r\n", name);

    header_field_len = 0;
    memset(parser, 0, sizeof(*parser));
    if (multipart_parser_init(parser, "abc", 3, &settings_field) != 0) {
        return MPPE_ERROR;
    }
    if (multipart_parser_execute(parser, buffer, len) != len) {
        return MPPE_ERROR;
    }
    if (header_field_len != strlen(name) || memcmp(header_field, name, header_field_len) != 0) {
        return MPPE_ERROR;
    }
    return len;
}

static int check_header_field_chars(void) {
    static const char *names[] = {
        "X-Foo2", "X_Foo", "X.Foo", "Content-Type1", "X*Foo",
    };
    static const char bad[] = "--abc\r\nX@Foo: v\r\n\r\nZ\r\n--abc--\r\n";
    multipart_parser parser;
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        CHECK(parse_header_field(names[i], &parser) != MPPE_ERROR);
    }

    /* characters outside tchar are still rejected */
    memset(&parser, 0, sizeof(parser));
    CHECK(multipart_parser_init(&parser, "abc", 3, &settings_none) == 0);
    CHECK(multipart_parser_execute(&parser, bad, sizeof(bad) - 1) == MPPE_ERROR);
    CHECK(parser.error_reason == MPPE_INVALID_HEADER_FIELD_CHAR);
    return 0;
}

/* ---- F8: error reporting must not abort the process ---- */

static int check_error_msg(void) {
    multipart_parser parser;
    char msg[128];
    int len;

    memset(&parser, 0, sizeof(parser));

    parser.error_reason = MPPE_EARLY_EOF;
    len = multipart_parser_error_msg(&parser, msg, sizeof(msg));
    CHECK(len > 0 && strstr(msg, "early EOF") != NULL);

    parser.error_reason = MPPE_UNKNOWN;
    len = multipart_parser_error_msg(&parser, msg, sizeof(msg));
    CHECK(len > 0 && strstr(msg, "unknown parser error") != NULL);

    parser.error_reason = MPPE_INVALID_HEADER_FIELD_CHAR;
    parser.error_unexpected = (char)-1; /* 0xff */
    len = multipart_parser_error_msg(&parser, msg, sizeof(msg));
    CHECK(len > 0 && strstr(msg, "\\xff") != NULL);
    return 0;
}

/* ---- runner ---- */

static int run(const char *name, int (*check)(void)) {
    int failed = check();
    printf("%-32s %s\n", name, failed ? "FAILED" : "ok");
    fflush(stdout);
    return failed;
}

int main(void) {
    int failed = 0;

    printf("regression tests\n");
    failed += run("part data across feeds", check_part_data_across_feeds);
    failed += run("part data CRLF split", check_part_data_crlf_split);
    failed += run("zero return state", check_zero_return_state);
    failed += run("init without boundary", check_init_without_boundary);
    failed += run("header value split", check_header_value_split);
    failed += run("header field chars", check_header_field_chars);
    failed += run("error message", check_error_msg);

    if (failed != 0) {
        printf("%d check(s) failed\n", failed);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
